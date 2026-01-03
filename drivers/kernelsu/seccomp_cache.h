#ifndef __KSU_H_KERNEL_COMPAT
#define __KSU_H_KERNEL_COMPAT

#include <linux/fs.h>
#include <linux/unistd.h>
#include <linux/version.h>

#ifndef SECCOMP_ARCH_NATIVE_NR
#define SECCOMP_ARCH_NATIVE_NR NR_syscalls
#endif

#ifdef CONFIG_COMPAT
#ifndef SECCOMP_ARCH_COMPAT_NR
#define SECCOMP_ARCH_COMPAT_NR __NR_compat_syscalls
#endif
#endif

extern void ksu_seccomp_clear_cache(struct seccomp_filter *filter, int nr);
extern void ksu_seccomp_allow_cache(struct seccomp_filter *filter, int nr);

#endif
