debugfs-objs	:= inode.o file.o

ifeq ($(CONFIG_DEBUG_KMEMLEAK),y)
ccflags-y	:= -DCONFIG_DEBUG_FS
endif

obj-y		+= debugfs.o
