#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <elf.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "asm/types.h"
#include "asm/parasite-syscall.h"

#include "parasite-syscall.h"
#include "parasite.h"
#include "compiler.h"
#include "kerndat.h"
#include "vdso.h"
#include "util.h"
#include "log.h"
#include "mem.h"
#include "vma.h"

#ifdef LOG_PREFIX
# undef LOG_PREFIX
#endif
#define LOG_PREFIX "vdso: "

struct vdso_symtable vdso_sym_rt = VDSO_SYMTABLE_INIT;
u64 vdso_pfn = VDSO_BAD_PFN;

static int vdso_fill_self_symtable(struct vdso_symtable *s)
{
	char buf[512];
	int ret = -1;
	FILE *maps;

	VDSO_INIT_SYMTABLE(s);

	maps = fopen("/proc/self/maps", "r");
	if (!maps) {
		pr_perror("Can't open self-vma");
		return -1;
	}

	while (fgets(buf, sizeof(buf), maps)) {
		unsigned long start, end;

		if (strstr(buf, "[vdso]") == NULL)
			continue;

		ret = sscanf(buf, "%lx-%lx", &start, &end);
		if (ret != 2) {
			ret = -1;
			pr_err("Can't find vDSO bounds\n");
			break;
		}

		s->vma_start = start;
		s->vma_end = end;

		ret = vdso_fill_symtable((void *)start, end - start, s);
		break;
	}

	fclose(maps);
	return ret;
}

int vdso_init(void)
{
	int ret = -1, fd;
	off_t off;

	if (vdso_fill_self_symtable(&vdso_sym_rt))
		return -1;

	fd = open_proc(getpid(), "pagemap");
	if (fd < 0)
		return -1;

	off = (vdso_sym_rt.vma_start / PAGE_SIZE) * sizeof(u64);
	if (lseek(fd, off, SEEK_SET) != off) {
		pr_perror("Failed to seek address %lx\n", vdso_sym_rt.vma_start);
		goto out;
	}

	ret = read(fd, &vdso_pfn, sizeof(vdso_pfn));
	if (ret < 0 || ret != sizeof(vdso_pfn)) {
		pr_perror("Can't read pme for pid %d", getpid());
		ret = -1;
	} else {
		vdso_pfn = PME_PFRAME(vdso_pfn);
		ret = 0;
	}
out:
	close(fd);
	return ret;
}

/*
 * Find out proxy vdso vma and drop it from the list. Also
 * fix vdso status on vmas if wrong status found.
 */
int parasite_fixup_vdso(struct parasite_ctl *ctl, pid_t pid,
			struct vm_area_list *vma_area_list)
{
	unsigned long proxy_addr = VDSO_BAD_ADDR;
	struct parasite_vdso_vma_entry *args;
	struct vma_area *marked = NULL;
	struct vma_area *vma;
	int fd, ret = -1;
	off_t off;
	u64 pfn;

	args = parasite_args(ctl, struct parasite_vdso_vma_entry);
	fd = open_proc(pid, "pagemap");
	if (fd < 0)
		return -1;

	list_for_each_entry(vma, &vma_area_list->h, list) {
		if (!vma_area_is(vma, VMA_AREA_REGULAR))
			continue;

		if ((vma->vma.prot & VDSO_PROT) != VDSO_PROT)
			continue;

		if (vma->vma.start > TASK_SIZE)
			continue;

		/*
		 * I need to poke every potentially marked vma,
		 * otherwise if task never called for vdso functions
		 * page frame number won't be reported.
		 */
		args->start = vma->vma.start;
		args->len = vma_area_len(vma);

		if (parasite_execute_daemon(PARASITE_CMD_CHECK_VDSO_MARK, ctl)) {
			pr_err("vdso: Parasite failed to poke for mark\n");
			ret = -1;
			goto err;
		}

		/*
		 * Defer handling marked vdso.
		 */
		if (unlikely(args->is_marked)) {
			BUG_ON(args->proxy_addr == VDSO_BAD_ADDR);
			BUG_ON(marked);
			marked = vma;
			proxy_addr = args->proxy_addr;
			continue;
		}

		off = (vma->vma.start / PAGE_SIZE) * sizeof(u64);
		if (lseek(fd, off, SEEK_SET) != off) {
			pr_perror("Failed to seek address %lx\n",
				  (long unsigned int)vma->vma.start);
			ret = -1;
			goto err;
		}

		ret = read(fd, &pfn, sizeof(pfn));
		if (ret < 0 || ret != sizeof(pfn)) {
			pr_perror("Can't read pme for pid %d", pid);
			ret = -1;
			goto err;
		}

		pfn = PME_PFRAME(pfn);
		BUG_ON(!pfn);

		/*
		 * Set proper VMA statuses.
		 */
		if (pfn == vdso_pfn) {
			if (!vma_area_is(vma, VMA_AREA_VDSO)) {
				pr_debug("vdso: Restore status by pfn at %lx\n",
					 (long)vma->vma.start);
				vma->vma.status |= VMA_AREA_VDSO;
			}
		} else {
			if (vma_area_is(vma, VMA_AREA_VDSO)) {
				pr_debug("vdso: Drop mishinted status at %lx\n",
					 (long)vma->vma.start);
				vma->vma.status &= ~VMA_AREA_VDSO;
			}
		}
	}

	/*
	 * There is marked vdso, it means such vdso is autogenerated
	 * and must be dropped from vma list.
	 */
	if (marked) {
		pr_debug("vdso: Found marked at %lx (proxy at %lx)\n",
			 (long)marked->vma.start, (long)proxy_addr);

		/*
		 * Don't forget to restore the proxy vdso status, since
		 * it's being not recognized by the kernel as vdso.
		 */
		list_for_each_entry(vma, &vma_area_list->h, list) {
			if (vma->vma.start == proxy_addr) {
				vma->vma.status |= VMA_AREA_REGULAR | VMA_AREA_VDSO;
				pr_debug("vdso: Restore proxy status at %lx\n",
					 (long)vma->vma.start);
				break;
			}
		}

		pr_debug("vdso: Droppping marked vdso at %lx\n",
			 (long)vma->vma.start);
		list_del(&marked->list);
		xfree(marked);
	}
	ret = 0;
err:
	close(fd);
	return ret;
}
