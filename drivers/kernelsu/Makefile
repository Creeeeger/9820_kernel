kernelsu-objs := ksu.o
kernelsu-objs += allowlist.o
kernelsu-objs += app_profile.o
kernelsu-objs += apk_sign.o
kernelsu-objs += sucompat.o
kernelsu-objs += syscall_hook_manager.o
kernelsu-objs += throne_tracker.o
kernelsu-objs += pkg_observer.o
kernelsu-objs += setuid_hook.o
kernelsu-objs += kernel_umount.o
kernelsu-objs += supercalls.o
kernelsu-objs += feature.o
kernelsu-objs += ksud.o
kernelsu-objs += embed_ksud.o
kernelsu-objs += seccomp_cache.o
kernelsu-objs += file_wrapper.o
kernelsu-objs += util.o

kernelsu-objs += selinux/selinux.o
kernelsu-objs += selinux/sepolicy.o
kernelsu-objs += selinux/rules.o
ccflags-y += -I$(srctree)/security/selinux -I$(srctree)/security/selinux/include
ccflags-y += -I$(objtree)/security/selinux -include $(srctree)/include/uapi/asm-generic/errno.h

obj-$(CONFIG_KSU) += kernelsu.o

ifeq ($(shell grep -q "strncpy_from_user_nofault" $(srctree)/include/linux/uaccess.h; echo $$?),0)
ccflags-y += -DKSU_HAVE_STRNCPY_FROM_USER_NOFAULT
endif

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))

# Hard-coded KSU version/tag (no git dependency)
KSU_VERSION := 90000
KSU_VERSION_TAG := v3.0.0
$(info -- KernelSU-Next version: $(KSU_VERSION))
$(info -- KernelSU-Next tag: $(KSU_VERSION_TAG))
ccflags-y += -DKSU_VERSION=$(KSU_VERSION)
ccflags-y += -DKSU_VERSION_TAG=\"$(KSU_VERSION_TAG)\"

ifndef KSU_NEXT_MANAGER_SIZE
KSU_NEXT_MANAGER_SIZE := 0x3e6
endif

ifndef KSU_NEXT_MANAGER_HASH
KSU_NEXT_MANAGER_HASH := 79e590113c4c4c0c222978e413a5faa801666957b1212a328e46c00c69821bf7
endif

ifdef KSU_MANAGER_PACKAGE
ccflags-y += -DKSU_MANAGER_PACKAGE=\"$(KSU_MANAGER_PACKAGE)\"
$(info -- KernelSU-Next Manager package name: $(KSU_MANAGER_PACKAGE))
endif

$(info -- KernelSU-Next Manager signature size: $(KSU_NEXT_MANAGER_SIZE))
$(info -- KernelSU-Next Manager signature hash: $(KSU_NEXT_MANAGER_HASH))

ccflags-y += -DEXPECTED_MANAGER_SIZE=$(KSU_NEXT_MANAGER_SIZE)
ccflags-y += -DEXPECTED_MANAGER_HASH=\"$(KSU_NEXT_MANAGER_HASH)\"

ccflags-y += -Wno-strict-prototypes -Wno-int-conversion -Wno-gcc-compat -Wno-missing-prototypes
ccflags-y += -Wno-declaration-after-statement -Wno-unused-function

all:
	make -C $(KDIR) M=$(MDIR) modules
compdb:
	python3 $(MDIR)/.vscode/generate_compdb.py -O $(KDIR) $(MDIR)
clean:
	make -C $(KDIR) M=$(MDIR) clean

# Keep a new line here!! Because someone may append config
