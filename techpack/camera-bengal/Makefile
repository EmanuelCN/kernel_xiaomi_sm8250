# SPDX-License-Identifier: GPL-2.0-only

# auto-detect subdirs
ifeq ($(CONFIG_ARCH_BENGAL), y)
include $(srctree)/techpack/camera-bengal/config/bengalcamera.conf
endif

ifeq ($(CONFIG_ARCH_KHAJE), y)
include $(srctree)/techpack/camera-bengal/config/khajecamera.conf
endif

ifeq ($(CONFIG_ARCH_BENGAL), y)
LINUXINCLUDE    += \
		-include $(srctree)/techpack/camera-bengal/config/bengalcameraconf.h
endif

ifeq ($(CONFIG_ARCH_KHAJE), y)
LINUXINCLUDE    += \
		-include $(srctree)/techpack/camera-bengal/config/khajecameraconf.h
endif

ifdef CONFIG_SPECTRA_CAMERA_BENGAL
# Use USERINCLUDE when you must reference the UAPI directories only.
USERINCLUDE     += \
                -I$(srctree)/techpack/camera-bengal/include/uapi

# Use LINUXINCLUDE when you must reference the include/ directory.
# Needed to be compatible with the O= option
LINUXINCLUDE    += \
                -I$(srctree)/techpack/camera-bengal/include/uapi \
                -I$(srctree)/techpack/camera-bengal/include
obj-y += drivers/
endif
