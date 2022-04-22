/*
 * Virtual Nand Swap Device which simulates Swap Area
 *
 * Copyright (C) 2013 SungHwan Yun
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 */

#ifndef _VBSWAP_DRV_H_
#define _VBSWAP_DRV_H_

#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/blkdev.h>

#define VBSWAP_DEBUG    0

#if VBSWAP_DEBUG > 0
#define dprintk        printk
#else
#define dprintk(x...)  do { ; } while (0)
#endif

/*
 * Max Swap Area Size (4GB)
 *  - 1024*1024 page = 4KB*1024*1024 = 4GB
 */
#define MAX_SWAP_AREA_SIZE_PAGES	(_AC(1, UL) << 20)

/*
 * Max Backing Storage Size (1GB)
 *  - 256*1024 page = 4KB*256*1024 = 1GB
 */
#define MAX_BACKING_STORAGE_SIZE_PAGES	(_AC(1, UL) << 18)

#define VBSWAP_INIT_DISKSIZE_SUCCESS 0x1
#define VBSWAP_INIT_DISKSIZE_FAIL 0x2
#define VBSWAP_INIT_BACKING_STORAGE_SUCCESS 0x10
#define VBSWAP_INIT_BACKING_STORAGE_FAIL 0x20

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1 << SECTOR_SHIFT)
#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define VBSWAP_LOGICAL_BLOCK_SHIFT 12
#define VBSWAP_LOGICAL_BLOCK_SIZE	(1 << VBSWAP_LOGICAL_BLOCK_SHIFT)
#define VBSWAP_SECTOR_PER_LOGICAL_BLOCK	(1 << \
	(VBSWAP_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))

#define MAX_BACKING_STORAGE_FILENAME_LEN	127

struct vbswap_stats {
	u64 vbswap_is_init;	/* vbswap_init success or fail */
	u64 vbswap_total_slot_num;	/* total  slot number */
	atomic_t vbswap_stored_pages;
		/* The number of pages currently stored in backing storagel */
	atomic_t vbswap_used_slot_num;
		/* currently used slot number */
	atomic_t vbswap_mapped_slot_free_num;
		/* total mapped slot free number */
	atomic_t vbswap_double_mapped_slot_num;
		/* total double mapped slot number */
	atomic_t vbswap_read_pages;	/* total read pages */
	atomic_t vbswap_write_pages;	/* total write pages */
	atomic_t vbswap_bio_end_fail_r1_num;
		/* total bio_end fail pages */
	atomic_t vbswap_bio_end_fail_r2_num;
	atomic_t vbswap_bio_end_fail_r3_num;
	atomic_t vbswap_bio_end_fail_w1_num;
	atomic_t vbswap_bio_end_fail_w2_num;
	atomic_t vbswap_bio_end_fail_w3_num;
	atomic_t vbswap_bio_large_bi_size_num;
		/* total large bio bi_size (>4kb) number */
	atomic_t vbswap_bio_large_bi_vcnt_num;
		/* total large bio bi_vcnt (>1) number */
	atomic_t vbswap_bio_invalid_num;
		/* total invalid (not aligned 4kb) bio number */
	atomic_t vbswap_bio_no_mem_num;
		/* total bio alloc fail number */
	atomic_t vbswap_not_mapped_read_pages;
		/* total not-mapped read pages */
	atomic_t vbswap_not_mapped_slot_free_num;
		/* total not-mapped-slot free number */
	atomic_t vbswap_backing_storage_full_num;
		/* total write_fail_because_of_backing_storage_full number */
	int vbswap_backing_storage_open_fail;
		/* backing storage file open fail */
};

struct vbswap {
	struct rw_semaphore lock;
		/* protect buffers against concurrent read and writes */
	struct request_queue *queue;
	struct gendisk *disk;
	u64 disksize;	/* bytes */
	char backing_storage_filename[MAX_BACKING_STORAGE_FILENAME_LEN+1];
	u64 bs_size;	/* backing storage size (pages) */
	int init_success;
		/* vbswap init success: VBSWAP_INIT_DISKSIZE_SUCCESS |
		* VBSWAP_INIT_BACKING_STORAGE_SUCCESS ,
		* others: vbswap init fail*/
	struct vbswap_stats stats;
};

#endif
