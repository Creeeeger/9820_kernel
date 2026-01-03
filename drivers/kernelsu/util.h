#ifndef __KSU_UTIL_H
#define __KSU_UTIL_H

#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#ifndef preempt_enable_no_resched_notrace
#define preempt_enable_no_resched_notrace() \
do { \
	barrier(); \
	__preempt_count_dec(); \
} while (0)
#endif

#ifndef preempt_disable_notrace
#define preempt_disable_notrace() \
do { \
	__preempt_count_inc(); \
	barrier(); \
} while (0)
#endif

#ifndef KSU_HAVE_STRNCPY_FROM_USER_NOFAULT
static inline long ksu_strncpy_from_user_nofault(char *dst,
						const char __user *src,
						long count)
{
	long ret;

	pagefault_disable();
	ret = strncpy_from_user(dst, src, count);
	pagefault_enable();

	return ret;
}
#define strncpy_from_user_nofault ksu_strncpy_from_user_nofault
#endif

bool try_set_access_flag(unsigned long addr);

#endif
