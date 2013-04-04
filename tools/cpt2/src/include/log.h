#ifndef __CPT2_LOG_H__
#define __CPT2_LOG_H__

/*
 * FIXME We use own trivial log engine based on printf
 * calls. Need to rework crtools log engine for reuse.
 */

#include <stdio.h>

#include "log-levels.h"

#ifndef LOG_PREFIX
# define LOG_PREFIX
#endif

extern unsigned int loglevel_get(void);

#define print_on_level(level, fmt, ...)						\
do {										\
	if (level == LOG_MSG || level <= loglevel_get())			\
		printf(fmt,  ##__VA_ARGS__);					\
} while (0)

#define pr_msg(fmt, ...)							\
	print_on_level(LOG_MSG,							\
		       fmt, ##__VA_ARGS__)

#define pr_info(fmt, ...)							\
	print_on_level(LOG_INFO,						\
		       LOG_PREFIX fmt, ##__VA_ARGS__)

#define pr_err(fmt, ...)							\
	print_on_level(LOG_ERROR,						\
		       "Error (%s:%d): " LOG_PREFIX fmt,			\
		       __FILE__, __LINE__, ##__VA_ARGS__)

#define pr_err_once(fmt, ...)							\
	do {									\
		static bool __printed;						\
		if (!__printed) {						\
			pr_err(fmt, ##__VA_ARGS__);				\
			__printed = 1;						\
		}								\
	} while (0)

#define pr_warn(fmt, ...)							\
	print_on_level(LOG_WARN,						\
		       "Warn  (%s:%d): " LOG_PREFIX fmt,			\
		       __FILE__, __LINE__, ##__VA_ARGS__)

#define pr_debug(fmt, ...)							\
	print_on_level(LOG_DEBUG,						\
		       LOG_PREFIX fmt, ##__VA_ARGS__)

#define pr_perror(fmt, ...)							\
	pr_err(fmt ": %m\n", ##__VA_ARGS__)

#endif /* __CPT2_LOG_H__ */