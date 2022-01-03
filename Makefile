# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_F2FS_FS) += f2fs.o

f2fs-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o nvm.o
f2fs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o gc_nvm.o
f2fs-y		+= shrinker.o extent_cache.o sysfs.o
f2fs-$(CONFIG_F2FS_STAT_FS) += debug.o
f2fs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
f2fs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
f2fs-$(CONFIG_F2FS_IO_TRACE) += trace.o

KERNELDIR:= /lib/modules/$(shell uname -r)/build
PWD:=$(shell pwd)
EXTRA_CFLAGS = -g
ccflags-y := -std=gnu99 -Wno-declaration-after-statement

default:
	make -C $(KERNELDIR) M=$(PWD) modules -j8
clean:
	rm -rf *.o *.mod.c *.ko *.symvers
