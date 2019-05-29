/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2016 Intel Corporation. All rights reserved.
 */
#ifndef __DAX_PRIVATE_H__
#define __DAX_PRIVATE_H__

#include <linux/device.h>
#include <linux/cdev.h>

/**
 * struct dax_region - mapping infrastructure for dax devices
 * @id: kernel-wide unique region for a memory range
 * @base: linear address corresponding to @res
 * @kref: to pin while other agents have a need to do lookups
 * @dev: parent device backing this region
 * @align: allocation and mapping alignment for child dax devices
 * @res: physical address range of the region
 * @pfn_flags: identify whether the pfns are paged back or not
 */
struct dax_region {
	int id;
	struct ida ida;
	void *base;
	struct kref kref;
	struct device *dev;
	unsigned int align;
	struct resource res;
	unsigned long pfn_flags;
};

/**
 * struct dev_dax - instance data for a subdivision of a dax region
 * @region - parent region
 * @dax_dev - core dax functionality
 * @dev - device core
 * @id - child id in the region
 * @num_resources - number of physical address extents in this device
 * @res - array of physical address ranges
 */
struct dev_dax {
	struct dax_region *region;
	struct dax_device *dax_dev;
	struct device dev;
	int id;
	int num_resources;
	struct resource res[0];
};
#endif
