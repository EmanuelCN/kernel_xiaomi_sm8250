// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual block swap device based on vnswap
 *
 * Copyright (C) 2020 Park Ju Hyung
 * Copyright (C) 2013 SungHwan Yun
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/mempool.h>
#include <linux/pagemap.h>
#include <linux/time.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include "vbswap.h"

/* Globals */
static int vbswap_major;
static struct vbswap *vbswap_device;
static struct page *swap_header_page;

/*
 * Description : virtual swp_offset -> backing storage block Mapping table
 * Value : 1 ~ Max swp_entry (used) , 0 (free)
 * For example,
 * vbswap_table [1] = 0, vbswap_table [3] = 1, vbswap_table [6] = 2,
 * vbswap_table [7] = 3,
 * vbswap_table [10] = 4, vbswap_table [Others] = -1
 */
static DEFINE_SPINLOCK(vbswap_table_lock);
static int *vbswap_table;

/* Backing Storage bitmap information */
static unsigned long *backing_storage_bitmap;
static unsigned int backing_storage_bitmap_last_allocated_index = -1;

/* Backing Storage bmap and bdev information */
static sector_t *backing_storage_bmap;
static struct block_device *backing_storage_bdev;
static struct file *backing_storage_file;

static DEFINE_SPINLOCK(vbswap_original_bio_lock);

static void vbswap_init_disksize(u64 disksize)
{
	if ((vbswap_device->init_success & VBSWAP_INIT_DISKSIZE_SUCCESS) != 0x0) {
		pr_err("%s %d: disksize is already initialized (disksize = %llu)\n",
				__func__, __LINE__, vbswap_device->disksize);
		return;
	}
	vbswap_device->disksize = PAGE_ALIGN(disksize);
	if ((vbswap_device->disksize/PAGE_SIZE > MAX_SWAP_AREA_SIZE_PAGES) ||
		!vbswap_device->disksize) {
		pr_err("%s %d: disksize is invalid (disksize = %llu)\n",
				__func__, __LINE__, vbswap_device->disksize);
		vbswap_device->disksize = 0;
		vbswap_device->init_success = VBSWAP_INIT_DISKSIZE_FAIL;
		return;
	}
	set_capacity(vbswap_device->disk,
		vbswap_device->disksize >> SECTOR_SHIFT);

	vbswap_device->init_success = VBSWAP_INIT_DISKSIZE_SUCCESS;
}

static int vbswap_init_backing_storage(void)
{
	struct address_space *mapping;
	struct inode *inode = NULL;
	unsigned blkbits, blocks_per_page;
	sector_t probe_block, last_block, first_block;
	sector_t discard_start_block = 0, discard_last_block = 0;
	int ret = 0, i;
	mm_segment_t oldfs;
	struct timeval discard_start, discard_end;
	int discard_time;

	if (!vbswap_device ||
		vbswap_device->init_success != VBSWAP_INIT_DISKSIZE_SUCCESS) {
		ret = -EINVAL;
		pr_err("%s %d: init disksize is failed." \
				"So we can not go ahead anymore.(init_success = %d)\n",
				__func__, __LINE__,
				!vbswap_device ? -1 :
				vbswap_device->init_success);
		goto error;
	}

	oldfs = get_fs();
	set_fs(get_ds());

	backing_storage_file =
		filp_open(vbswap_device->backing_storage_filename,
					O_RDWR | O_LARGEFILE, 0);

	if (IS_ERR(backing_storage_file)) {
		ret = PTR_ERR(backing_storage_file);
		vbswap_device->stats.vbswap_backing_storage_open_fail =
			PTR_ERR(backing_storage_file);
		backing_storage_file = NULL;
		set_fs(oldfs);
		pr_err("%s %d: filp_open failed" \
				"(backing_storage_file, error, " \
				"backing_storage_filename)" \
				" = (0x%08x, %s)\n",
				__func__, __LINE__,
				ret, vbswap_device->backing_storage_filename);
		goto error;
	} else {
		set_fs(oldfs);
		vbswap_device->stats.vbswap_backing_storage_open_fail = 0;
		dprintk("%s %d: filp_open success" \
				"(backing_storage_file, error, backing_storage_filename)"
				"= (0x%08x, %s)\n",
				__func__, __LINE__,
				ret, vbswap_device->backing_storage_filename);
	}

	/* initialize vbswap_table */
	vbswap_table = vmalloc((vbswap_device->disksize / PAGE_SIZE) *
				sizeof(int));
	if (vbswap_table == NULL) {
		pr_err("%s %d: alloc vbswap_table failed\n",
			__func__, __LINE__);
		ret = -ENOMEM;
		goto error;
	}
	for (i = 0; i < vbswap_device->disksize / PAGE_SIZE; i++)
		vbswap_table[i] = -1;

	mapping = backing_storage_file->f_mapping;
	inode = mapping->host;
	backing_storage_bdev = inode->i_sb->s_bdev;

	if (!S_ISREG(inode->i_mode)) {
		ret = -EINVAL;
		pr_err("%s %d: backing storage file is not regular file" \
				"(inode->i_mode = %d)\n",
				__func__, __LINE__, inode->i_mode);
		goto close_file;
	}

	inode->i_flags |= S_IMMUTABLE;

	blkbits = inode->i_blkbits;
	blocks_per_page = PAGE_SIZE >> blkbits;

	if (blocks_per_page != 1) {
		ret = -EINVAL;
		pr_err("%s %d: blocks_per_page is not 1. " \
				"(blocks_per_page, inode->i_blkbits) = (%d, %d)\n",
				__func__, __LINE__,
				blocks_per_page, inode->i_blkbits);
		goto close_file;
	}

	last_block = i_size_read(inode) >> blkbits;
	vbswap_device->bs_size = last_block;
	vbswap_device->stats.vbswap_total_slot_num = last_block;

	if ((vbswap_device->bs_size > MAX_BACKING_STORAGE_SIZE_PAGES) ||
		!vbswap_device->bs_size) {
		ret = -EINVAL;
		pr_err("%s %d: backing storage size is invalid." \
				"(backing storage size = %llu)\n",
				__func__, __LINE__,
				vbswap_device->bs_size);
		goto close_file;
	}

	/*
	* Align sizeof(unsigned long) * 8 page
	*  - This alignment is for Integer (sizeof(unsigned long) Bytes ->
	*    sizeof(unsigned long) * 8 Bit -> sizeof(unsigned long) * 8 page)
	*    bitmap operation
	*/
	if (vbswap_device->bs_size % (sizeof(unsigned long)*8) != 0) {
		dprintk("%s %d: backing storage size is misaligned " \
				"(%d page align)." \
				"So, it is truncated from %llu pages to %llu pages\n",
				__func__, __LINE__,
				sizeof(unsigned long) * 8,
				vbswap_device->bs_size,
				vbswap_device->bs_size / ((sizeof(unsigned long) * 8) *
					(sizeof(unsigned long) * 8)));
		vbswap_device->bs_size = vbswap_device->bs_size /
		((sizeof(unsigned long) * 8) * (sizeof(unsigned long) * 8));
	}

	backing_storage_bitmap = vmalloc(vbswap_device->bs_size / 8);
	if (backing_storage_bitmap == NULL) {
		ret = -ENOMEM;
		goto close_file;
	}

	for (i = 0; i < vbswap_device->bs_size / (8 * sizeof(unsigned long)); i++)
		backing_storage_bitmap[i] = 0;
	backing_storage_bitmap_last_allocated_index = -1;

	backing_storage_bmap = vmalloc(vbswap_device->bs_size *
							sizeof(sector_t));
	if (backing_storage_bmap == NULL) {
		ret = -ENOMEM;
		goto free_bitmap;
	}

	for (probe_block = 0; probe_block < last_block; probe_block++) {
		first_block = bmap(inode, probe_block);
		if (first_block == 0) {
			pr_err("%s %d: backing_storage file has holes." \
					"(probe_block, first_block) = (%llu,%llu)\n",
					__func__, __LINE__,
					(unsigned long long) probe_block,
					(unsigned long long) first_block);
			ret = -EINVAL;
			goto free_bmap;
		}
		backing_storage_bmap[probe_block] = first_block;

		/* new extent */
		if (discard_start_block == 0) {
			discard_start_block = discard_last_block = first_block;
			continue;
		}

		/* first block is a member of extent */
		if (discard_last_block+1 == first_block) {
			discard_last_block++;
			continue;
		}

		/*
		* first block is not a member of extent
		* discard current extent.
		*/
		do_gettimeofday(&discard_start);
		ret = blkdev_issue_discard(backing_storage_bdev,
				discard_start_block << (PAGE_SHIFT - 9),
				(discard_last_block - discard_start_block + 1)
				<< (PAGE_SHIFT - 9), GFP_KERNEL, 0);
		do_gettimeofday(&discard_end);

		discard_time = (discard_end.tv_sec - discard_start.tv_sec) *
			USEC_PER_SEC +
			(discard_end.tv_usec - discard_start.tv_usec);

		if (ret) {
			pr_err("%s %d: blkdev_issue_discard failed. (ret) = (%d)\n",
					__func__, __LINE__, ret);
			goto free_bmap;
		}
		dprintk("%s %d: blkdev_issue_discard success" \
				"(start, size, discard_time) = (%llu, %llu, %d)\n",
				__func__, __LINE__, discard_start_block,
				discard_last_block - discard_start_block + 1,
				discard_time);
		discard_start_block = discard_last_block = first_block;
	}

	/* last extent */
	if (discard_start_block) {
		do_gettimeofday(&discard_start);
		ret = blkdev_issue_discard(backing_storage_bdev,
				discard_start_block << (PAGE_SHIFT - 9),
				(discard_last_block - discard_start_block + 1)
				<< (PAGE_SHIFT - 9), GFP_KERNEL, 0);
		do_gettimeofday(&discard_end);
		discard_time = (discard_end.tv_sec - discard_start.tv_sec) *
			USEC_PER_SEC +
			(discard_end.tv_usec - discard_start.tv_usec);
		if (ret) {
			pr_err("%s %d: blkdev_issue_discard failed. (ret) = (%d)\n",
					__func__, __LINE__, ret);
			goto free_bmap;
		}
		dprintk("%s %d: blkdev_issue_discard success" \
				"(start, size, discard_time) = (%llu, %llu, %d)\n",
				__func__, __LINE__, discard_start_block,
				discard_last_block - discard_start_block + 1,
				discard_time);
		discard_start_block = discard_last_block = 0;
	}

	vbswap_device->init_success |= VBSWAP_INIT_BACKING_STORAGE_SUCCESS;
	return ret;

free_bmap:
	vfree(backing_storage_bmap);

free_bitmap:
	vfree(backing_storage_bitmap);

close_file:
	filp_close(backing_storage_file, NULL);

error:
	if (vbswap_device)
		vbswap_device->init_success |= VBSWAP_INIT_BACKING_STORAGE_FAIL;
	return ret;
}

static int vbswap_deinit_backing_storage(void)
{
	struct address_space *mapping;
	int stored_pages = atomic_read(&vbswap_device->stats.vbswap_stored_pages);

	if (backing_storage_file && !stored_pages) {
		vbswap_device->init_success &= ~VBSWAP_INIT_BACKING_STORAGE_SUCCESS;
		vbswap_device->bs_size = 0;
		vbswap_device->stats.vbswap_total_slot_num = 0;

		mapping = backing_storage_file->f_mapping;
		if (mapping && mapping->host && mapping->host->i_flags)
			mapping->host->i_flags &= ~S_IMMUTABLE;

		filp_close(backing_storage_file, NULL);
		backing_storage_file = NULL;

		if (backing_storage_bitmap) {
			vfree(backing_storage_bitmap);
			backing_storage_bitmap = NULL;
		}

		if (backing_storage_bmap) {
			vfree(backing_storage_bmap);
			backing_storage_bmap = NULL;
		}
	}
	return 0;
}

/* find free area (nand_offset, page_offset) in backing storage */
static int vbswap_find_free_area_in_backing_storage(int *nand_offset)
{
	int i, found = 0;

	/* Backing Storage is full */
	if (backing_storage_bitmap_last_allocated_index ==
		vbswap_device->bs_size) {
		atomic_inc(&vbswap_device->stats.
			vbswap_backing_storage_full_num);
		return -ENOSPC;
	}

	for (i = backing_storage_bitmap_last_allocated_index + 1;
		i < vbswap_device->bs_size; i++)
		if (!test_bit(i, backing_storage_bitmap)) {
			found = 1;
			break;
		}

	if (!found) {
		for (i = 0;
			i < backing_storage_bitmap_last_allocated_index;
			i++)
			if (!test_bit(i, backing_storage_bitmap)) {
				found = 1;
				break;
			}
	}

	/* Backing Storage is full */
	if (!found) {
		backing_storage_bitmap_last_allocated_index =
			vbswap_device->bs_size;
		atomic_inc(&vbswap_device->stats.
			vbswap_backing_storage_full_num);
		return -ENOSPC;
	}
	*nand_offset =
		backing_storage_bitmap_last_allocated_index = i;
	return i;
}

/* refer req_bio_endio() */
static void vbswap_bio_end_read(struct bio *bio)
{
	const blk_status_t err = bio->bi_status;
	struct bio *original_bio = (struct bio *) bio->bi_private;
	unsigned long flags;

	dprintk("%s %d: (uptodate,error,bi_size) = (%d, %d, %d)\n",
			__func__, __LINE__, uptodate, err, bio->bi_iter.bi_size);

	if (err) {
		atomic_inc(&vbswap_device->stats.vbswap_bio_end_fail_r1_num);
		pr_err("%s %d: (error, bio->bi_iter.bi_size, original_bio->bi_iter.bi_size," \
				"bio->bi_vcnt, original_bio->bi_vcnt, " \
				"bio->bi_iter.bi_idx," \
				"original_bio->bi_iter.bi_idx, " \
				"vbswap_bio_end_fail_r1_num ~" \
				"vbswap_bio_end_fail_r3_num) =" \
				"(%d, %d, %d, %d, %d, %d, %d, %d, %d, %d)\n",
				__func__, __LINE__, err, bio->bi_iter.bi_size,
				original_bio->bi_iter.bi_size,
				bio->bi_vcnt, original_bio->bi_vcnt,
				bio->bi_iter.bi_idx,
				original_bio->bi_iter.bi_idx,
				vbswap_device->stats.
					vbswap_bio_end_fail_r1_num.counter,
				vbswap_device->stats.
					vbswap_bio_end_fail_r2_num.counter,
				vbswap_device->stats.
					vbswap_bio_end_fail_r3_num.counter);
		bio_io_error(original_bio);
		goto out_bio_put;
	} else {
		/*
		* There are bytes yet to be transferred.
		* blk_end_request() -> blk_end_bidi_request() ->
		* blk_update_bidi_request() ->
		* blk_update_request() -> req_bio_endio() ->
		* bio->bi_iter.bi_size -= nbytes;
		*/
		spin_lock_irqsave(&vbswap_original_bio_lock, flags);
		original_bio->bi_iter.bi_size -= ((unsigned int)PAGE_SIZE - bio->bi_iter.bi_size);

		if (bio->bi_iter.bi_size == PAGE_SIZE) {
			atomic_inc(&vbswap_device->stats.
				vbswap_bio_end_fail_r2_num);
			pr_err("%s %d: (error, bio->bi_iter.bi_size, " \
					"original_bio->bi_iter.bi_size," \
					"bio->bi_vcnt," \
					"original_bio->bi_vcnt, bio->bi_iter.bi_idx, " \
					"original_bio->bi_iter.bi_idx," \
					"vbswap_bio_end_fail_r1_num ~ " \
					"vbswap_bio_end_fail_r3_num) = " \
					"(%d, %d, %d, %d, %d, %d, %d, %d, %d, %d)\n",
					__func__, __LINE__, err, bio->bi_iter.bi_size,
					original_bio->bi_iter.bi_size,
					bio->bi_vcnt, original_bio->bi_vcnt,
					bio->bi_iter.bi_idx,
					original_bio->bi_iter.bi_idx,
					vbswap_device->stats.
						vbswap_bio_end_fail_r1_num.
						counter,
					vbswap_device->stats.
						vbswap_bio_end_fail_r2_num.
						counter,
					vbswap_device->stats.
						vbswap_bio_end_fail_r3_num.
						counter);
			spin_unlock_irqrestore(&vbswap_original_bio_lock,
				flags);
			goto out_bio_put;
		}

		if (bio->bi_iter.bi_size || original_bio->bi_iter.bi_size) {
			atomic_inc(&vbswap_device->stats.
				vbswap_bio_end_fail_r3_num);
			pr_err("%s %d: (error, bio->bi_iter.bi_size, " \
					"original_bio->bi_iter.bi_size," \
					"bio->bi_vcnt," \
					"original_bio->bi_vcnt, bio->bi_iter.bi_idx, " \
					"original_bio->bi_iter.bi_idx," \
					"vbswap_bio_end_fail_r1_num ~ " \
					"vbswap_bio_end_fail_r3_num) = " \
					"(%d, %d, %d, %d, %d, %d, %d, %d, %d, %d)\n",
					__func__, __LINE__, err, bio->bi_iter.bi_size,
					original_bio->bi_iter.bi_size,
					bio->bi_vcnt, original_bio->bi_vcnt,
					bio->bi_iter.bi_idx, original_bio->bi_iter.bi_idx,
					vbswap_device->stats.
						vbswap_bio_end_fail_r1_num.
						counter,
					vbswap_device->stats.
						vbswap_bio_end_fail_r2_num.
						counter,
					vbswap_device->stats.
						vbswap_bio_end_fail_r3_num.
						counter);
			spin_unlock_irqrestore(&vbswap_original_bio_lock,
				flags);
			goto out_bio_put;
		}

		original_bio->bi_status = BLK_STS_OK;
		spin_unlock_irqrestore(&vbswap_original_bio_lock, flags);
		bio_endio(original_bio);
	}

out_bio_put:
	bio_put(bio);
}

/* refer req_bio_endio() */
static void vbswap_bio_end_write(struct bio *bio)
{
	const blk_status_t err = bio->bi_status;
	struct bio *original_bio = (struct bio *) bio->bi_private;
	unsigned long flags;

	dprintk("%s %d: (error, bi_size) = (%d, %d)\n",
			__func__, __LINE__, err, bio->bi_iter.bi_size);

	if (err) {
		atomic_inc(&vbswap_device->stats.vbswap_bio_end_fail_w1_num);
		pr_err("%s %d: (error, bio->bi_iter.bi_size, original_bio->bi_iter.bi_size, " \
				"bio->bi_vcnt," \
				"original_bio->bi_vcnt, bio->bi_iter.bi_idx, " \
				"original_bio->bi_iter.bi_idx," \
				"vbswap_bio_end_fail_w1_num ~ " \
				"vbswap_bio_end_fail_w3_num) = " \
				"(%d, %d, %d, %d, %d, %d, %d, %d, %d, %d)\n",
				__func__, __LINE__, err, bio->bi_iter.bi_size,
				original_bio->bi_iter.bi_size,
				bio->bi_vcnt, original_bio->bi_vcnt,
				bio->bi_iter.bi_idx,
				original_bio->bi_iter.bi_idx,
				vbswap_device->stats.
					vbswap_bio_end_fail_w1_num.counter,
				vbswap_device->stats.
					vbswap_bio_end_fail_w2_num.counter,
				vbswap_device->stats.
					vbswap_bio_end_fail_w3_num.counter);
		bio_io_error(original_bio);
		goto out_bio_put;
	} else {
		/*
		* There are bytes yet to be transferred.
		* blk_end_request() -> blk_end_bidi_request() ->
		* blk_update_bidi_request() ->
		* blk_update_request() -> req_bio_endio() ->
		* bio->bi_iter.bi_size -= nbytes;
		*/
		spin_lock_irqsave(&vbswap_original_bio_lock, flags);
		original_bio->bi_iter.bi_size -= ((unsigned int)PAGE_SIZE - bio->bi_iter.bi_size);

		if (bio->bi_iter.bi_size == PAGE_SIZE) {
			atomic_inc(&vbswap_device->stats.
				vbswap_bio_end_fail_w2_num);
			pr_err("%s %d: (error, bio->bi_iter.bi_size, " \
					"original_bio->bi_iter.bi_size, " \
					"bio->bi_vcnt," \
					"original_bio->bi_vcnt, bio->bi_iter.bi_idx, " \
					"original_bio->bi_iter.bi_idx," \
					"vbswap_bio_end_fail_w1_num ~ " \
					"vbswap_bio_end_fail_w3_num) = " \
					"(%d, %d, %d, %d, %d, %d, %d, %d, %d, %d)\n",
					__func__, __LINE__, err, bio->bi_iter.bi_size,
					original_bio->bi_iter.bi_size,
					bio->bi_vcnt, original_bio->bi_vcnt,
					bio->bi_iter.bi_idx,
					original_bio->bi_iter.bi_idx,
					vbswap_device->stats.
						vbswap_bio_end_fail_w1_num.
						counter,
					vbswap_device->stats.
						vbswap_bio_end_fail_w2_num.
						counter,
					vbswap_device->stats.
						vbswap_bio_end_fail_w3_num.
						counter);
			spin_unlock_irqrestore(&vbswap_original_bio_lock,
				flags);
			goto out_bio_put;
		}

		if (bio->bi_iter.bi_size || original_bio->bi_iter.bi_size) {
			atomic_inc(&vbswap_device->stats.
				vbswap_bio_end_fail_w3_num);
			pr_err("%s %d: (error, bio->bi_iter.bi_size, " \
					"original_bio->bi_iter.bi_size, " \
					"bio->bi_vcnt," \
					"original_bio->bi_vcnt, bio->bi_iter.bi_idx, " \
					"original_bio->bi_iter.bi_idx," \
					"vbswap_bio_end_fail_w1_num ~ " \
					"vbswap_bio_end_fail_w3_num) = " \
					"(%d, %d, %d, %d, %d, %d, %d, %d, %d, %d)\n",
					__func__, __LINE__, err, bio->bi_iter.bi_size,
					original_bio->bi_iter.bi_size,
					bio->bi_vcnt, original_bio->bi_vcnt,
					bio->bi_iter.bi_idx,
					original_bio->bi_iter.bi_idx,
					vbswap_device->stats.
						vbswap_bio_end_fail_w1_num.
						counter,
					vbswap_device->stats.
						vbswap_bio_end_fail_w2_num.
						counter,
					vbswap_device->stats.
						vbswap_bio_end_fail_w3_num.
						counter);
			spin_unlock_irqrestore(&vbswap_original_bio_lock,
				flags);
			goto out_bio_put;
		}

		original_bio->bi_status = BLK_STS_OK;
		spin_unlock_irqrestore(&vbswap_original_bio_lock,
			flags);
		bio_endio(original_bio);
	}

out_bio_put:
	bio_put(bio);
}

/* Insert entry into VBSWAP_IO sub system */
static int vbswap_submit_bio(int rw, int nand_offset,
	struct page *page, struct bio *original_bio)
{
	struct bio *bio;
	int ret = 0;

	if (!rw) {
		VM_BUG_ON(!PageLocked(page));
		VM_BUG_ON(PageUptodate(page));
	}

	bio = bio_alloc(GFP_NOIO, 1);

	if (!bio) {
		atomic_inc(&vbswap_device->stats.vbswap_bio_no_mem_num);
		ret = -ENOMEM;
		goto out;
	}

	bio->bi_iter.bi_sector = (backing_storage_bmap[nand_offset] <<
					(PAGE_SHIFT - 9));
	bio_set_dev(bio, backing_storage_bdev);
	bio->bi_io_vec[0].bv_page = page;
	bio->bi_io_vec[0].bv_len = PAGE_SIZE;
	bio->bi_io_vec[0].bv_offset = 0;
	bio->bi_vcnt = 1;
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_size = PAGE_SIZE;
	bio->bi_private = (void *) original_bio;
	if (rw)
		bio->bi_end_io = vbswap_bio_end_write;
	else
		bio->bi_end_io = vbswap_bio_end_read;

	dprintk("%s %d: (rw, nand_offset) = (%d,%d)\n",
			__func__, __LINE__, rw, nand_offset);

	submit_bio(bio);

	if (rw) {
		atomic_inc(&vbswap_device->stats.
			vbswap_stored_pages);
		atomic_inc(&vbswap_device->stats.
			vbswap_write_pages);
	} else {
		atomic_inc(&vbswap_device->stats.
			vbswap_read_pages);
	}
	/* TODO: check bio->bi_flags */

out:
	return ret;
}

static int vbswap_bvec_read(struct vbswap *vbswap, struct bio_vec bvec,
	u32 index, struct bio *bio)
{
	struct page *page;
	unsigned char *user_mem, *swap_header_page_mem;
	int nand_offset = 0, ret = 0;

	page = bvec.bv_page;

	/* swap header */
	if (index == 0) {
		user_mem = kmap_atomic(page);
		swap_header_page_mem = kmap_atomic(swap_header_page);
		memcpy(user_mem, swap_header_page_mem, bvec.bv_len);
		kunmap_atomic(swap_header_page_mem);
		kunmap_atomic(user_mem);
		flush_dcache_page(page);
		return 0;
	}

	spin_lock_irq(&vbswap_table_lock);
	nand_offset = vbswap_table[index];
	if (nand_offset == -1) {
		pr_err("%s %d: vbswap_table is not mapped. " \
				"(index, nand_offset)" \
				"= (%d, %d)\n", __func__, __LINE__,
				index, nand_offset);
		ret = -EIO;
		atomic_inc(&vbswap_device->stats.vbswap_not_mapped_read_pages);
		spin_unlock_irq(&vbswap_table_lock);
		goto out;
	}
	spin_unlock_irq(&vbswap_table_lock);

	dprintk("%s %d: (index, nand_offset) = (%d, %d)\n",
			__func__, __LINE__, index, nand_offset);

	/* Read nand_offset position backing storage into page */
	ret = vbswap_submit_bio(0, nand_offset, page, bio);

out:
	return ret;
}

static int vbswap_bvec_write(struct vbswap *vbswap, struct bio_vec bvec,
	u32 index, struct bio *bio)
{
	struct page *page;
	unsigned char *user_mem, *swap_header_page_mem;
	int nand_offset = 0, ret;

	page = bvec.bv_page;

	/* swap header */
	if (index == 0) {
		user_mem = kmap_atomic(page);
		swap_header_page_mem = kmap_atomic(swap_header_page);
		memcpy(swap_header_page_mem, user_mem, PAGE_SIZE);
		kunmap_atomic(swap_header_page_mem);
		kunmap_atomic(user_mem);
		return 0;
	}

	spin_lock_irq(&vbswap_table_lock);
	nand_offset = vbswap_table[index];

	/* duplicate write - remove existing mapping */
	if (nand_offset != -1) {
		atomic_inc(&vbswap_device->stats.
			vbswap_double_mapped_slot_num);
		clear_bit(nand_offset, backing_storage_bitmap);
		vbswap_table[index] = -1;
		atomic_dec(&vbswap_device->stats.
			vbswap_used_slot_num);
		atomic_dec(&vbswap_device->stats.
			vbswap_stored_pages);
	}

	ret = vbswap_find_free_area_in_backing_storage(&nand_offset);
	if (ret < 0) {
		spin_unlock_irq(&vbswap_table_lock);
		return ret;
	}
	set_bit(nand_offset, backing_storage_bitmap);
	vbswap_table[index] = nand_offset;
	atomic_inc(&vbswap_device->stats.vbswap_used_slot_num);
	spin_unlock_irq(&vbswap_table_lock);

	dprintk("%s %d: (index, nand_offset) = (%d, %d)\n",
			__func__, __LINE__, index, nand_offset);
	ret = vbswap_submit_bio(1, nand_offset, page, bio);

	if (ret) {
		spin_lock_irq(&vbswap_table_lock);
		clear_bit(nand_offset, backing_storage_bitmap);
		vbswap_table[index] = -1;
		spin_unlock_irq(&vbswap_table_lock);
	}

	return ret;
}

static int vbswap_bvec_rw(struct vbswap *vbswap, struct bio_vec bvec,
	u32 index, struct bio *bio, int rw)
{
	int ret;

	if (rw == READ) {
		down_read(&vbswap->lock);
		dprintk("%s %d: (rw,index) = (%d, %d)\n",
			__func__, __LINE__, rw, index);
		ret = vbswap_bvec_read(vbswap, bvec, index, bio);
		up_read(&vbswap->lock);
	} else {
		down_write(&vbswap->lock);
		dprintk("%s %d: (rw,index) = (%d, %d)\n",
			__func__, __LINE__, rw, index);
		ret = vbswap_bvec_write(vbswap, bvec, index, bio);
		up_write(&vbswap->lock);
	}

	return ret;
}

static void __vbswap_make_request(struct vbswap *vbswap,
	struct bio *bio, int rw)
{
	int offset, ret;
	u32 index, is_swap_header_page;
	struct bio_vec bvec;
	struct bvec_iter iter;

	index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	offset = (bio->bi_iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;

	if (index == 0)
		is_swap_header_page = 1;
	else
		is_swap_header_page = 0;

	dprintk("%s %d: (rw, index, offset, bi_size) = " \
			"(%d, %d, %d, %d)\n",
			__func__, __LINE__,
			rw, index, offset, bio->bi_iter.bi_size);

	if (offset) {
		atomic_inc(&vbswap_device->stats.
			vbswap_bio_invalid_num);
		pr_err("%s %d: invalid offset. " \
				"(bio->bi_iter.bi_sector, index, offset," \
				"vbswap_bio_invalid_num) = (%llu, %d, %d, %d)\n",
				__func__, __LINE__,
				(unsigned long long) bio->bi_iter.bi_sector,
				index, offset,
				vbswap_device->stats.
					vbswap_bio_invalid_num.counter);
		goto out_error;
	}

	if (bio->bi_iter.bi_size > PAGE_SIZE) {
		atomic_inc(&vbswap_device->stats.
			vbswap_bio_large_bi_size_num);
		goto out_error;
	}

	if (bio->bi_vcnt > 1) {
		atomic_inc(&vbswap_device->stats.
			vbswap_bio_large_bi_vcnt_num);
		goto out_error;
	}

	bio_for_each_segment(bvec, bio, iter) {
		if (bvec.bv_len != PAGE_SIZE || bvec.bv_offset != 0) {
			atomic_inc(&vbswap_device->stats.
				vbswap_bio_invalid_num);
			pr_err("%s %d: bvec is misaligned. " \
					"(bv_len, bv_offset," \
					"vbswap_bio_invalid_num) = (%d, %d, %d)\n",
					__func__, __LINE__,
					bvec.bv_len, bvec.bv_offset,
					vbswap_device->stats.
						vbswap_bio_invalid_num.counter);
			goto out_error;
		}

		dprintk("%s %d: (rw, index, bvec.bv_len) = " \
				"(%d, %d, %d)\n",
				__func__, __LINE__, rw, index, bvec.bv_len);

		ret = vbswap_bvec_rw(vbswap, bvec, index, bio, rw);
		if (ret < 0) {
			if (ret != -ENOSPC)
				pr_err("%s %d: vbswap_bvec_rw failed." \
						"(ret) = (%d)\n",
						__func__, __LINE__, ret);
			else
				dprintk("%s %d: vbswap_bvec_rw failed. " \
				"(ret) = (%d)\n",
						__func__, __LINE__, ret);
			goto out_error;
		}

		index++;
	}

	if (is_swap_header_page) {
		bio->bi_status = BLK_STS_OK;
		bio_endio(bio);
	}

	return;

out_error:
	bio_io_error(bio);
}

/*
 * Check if request is within bounds and aligned on vbswap logical blocks.
 */
static inline int vbswap_valid_io_request(struct vbswap *vbswap,
	struct bio *bio)
{
	if (unlikely(
		(bio->bi_iter.bi_sector >= (vbswap->disksize >> SECTOR_SHIFT)) ||
		(bio->bi_iter.bi_sector & (VBSWAP_SECTOR_PER_LOGICAL_BLOCK - 1)) ||
		(bio->bi_iter.bi_size & (VBSWAP_LOGICAL_BLOCK_SIZE - 1)))) {

		return 0;
	}

	/* I/O request is valid */
	return 1;
}

/*
 * Handler function for all vbswap I/O requests.
 */
static blk_qc_t vbswap_make_request(struct request_queue *queue, struct bio *bio)
{
	struct vbswap *vbswap = queue->queuedata;

	/* disable NAND I/O when DiskSize is not initialized */
	if (!(vbswap_device->init_success & VBSWAP_INIT_DISKSIZE_SUCCESS))
		goto error;

	/*
	 * disable NAND I/O when Backing Storage is not initialized and
	 * is not swap_header_page
	 */
	if (!(vbswap_device->init_success &
		VBSWAP_INIT_BACKING_STORAGE_SUCCESS)
		&& (bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT))
		goto error;

	if (!vbswap_valid_io_request(vbswap, bio)) {
		atomic_inc(&vbswap_device->stats.
			vbswap_bio_invalid_num);
		pr_err("%s %d: invalid io request. " \
				"(bio->bi_iter.bi_sector, bio->bi_iter.bi_size," \
				"vbswap->disksize, vbswap_bio_invalid_num) = " \
				"(%llu, %d, %llu, %d)\n",
				__func__, __LINE__,
				(unsigned long long) bio->bi_iter.bi_sector,
				bio->bi_iter.bi_size, vbswap->disksize,
				vbswap_device->stats.
					vbswap_bio_invalid_num.counter);
		goto error;
	}

	__vbswap_make_request(vbswap, bio, bio_data_dir(bio));
	return BLK_QC_T_NONE;

error:
	bio_io_error(bio);
	return BLK_QC_T_NONE;
}

static void vbswap_slot_free_notify(struct block_device *bdev, unsigned long index)
{
	struct vbswap *vbswap;
	int nand_offset = 0;

	vbswap = bdev->bd_disk->private_data;

	spin_lock_irq(&vbswap_table_lock);
	nand_offset = vbswap_table ? vbswap_table[index] : -1;

	/* This index is not mapped to vbswap and is mapped to zswap */
	if (nand_offset == -1) {
		atomic_inc(&vbswap_device->stats.
			vbswap_not_mapped_slot_free_num);
		spin_unlock_irq(&vbswap_table_lock);
		return;
	}

	atomic_inc(&vbswap_device->stats.
		vbswap_mapped_slot_free_num);
	atomic_dec(&vbswap_device->stats.
		vbswap_stored_pages);
	atomic_dec(&vbswap_device->stats.
		vbswap_used_slot_num);
	clear_bit(nand_offset, backing_storage_bitmap);
	vbswap_table[index] = -1;

	/* When Backing Storage is full, set Backing Storage is not full */
	if (backing_storage_bitmap_last_allocated_index ==
		vbswap_device->bs_size) {
		backing_storage_bitmap_last_allocated_index = nand_offset;
	}
	spin_unlock_irq(&vbswap_table_lock);

	/*
	 * disable blkdev_issue_discard
	 * - BUG: scheduling while atomic: rild/4248/0x00000003
	*   blkdev_issue_discard() -> wait_for_completion() ->
	 *	wait_for_common() -> schedule_timeout() -> schedule()
	 */
#if 0
	/* discard nand_offset position Backing Storage for security */
	ret = blkdev_issue_discard(backing_storage_bdev,
			(backing_storage_bmap[nand_offset] << (PAGE_SHIFT - 9)),
			1 << (PAGE_SHIFT - 9), GFP_KERNEL, 0);
	if (ret)
		dprintk("vbswap_slot_free_notify: " \
		"blkdev_issue_discard is failed\n");
#endif
}

static const struct block_device_operations vbswap_devops = {
	.swap_slot_free_notify = vbswap_slot_free_notify,
	.owner = THIS_MODULE
};

static ssize_t disksize_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", vbswap_device->disksize);
}

static ssize_t disksize_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	u64 disksize;

	ret = kstrtoull(buf, 10, &disksize);
	if (ret)
		return ret;

	vbswap_init_disksize(disksize);
	return len;
}

static ssize_t swap_filename_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	if (!vbswap_device)
		return 0;
	dprintk("%s %d: backing_storage_filename = %s\n",
			__func__, __LINE__,
			vbswap_device->backing_storage_filename);
	return sprintf(buf, "%s\n", vbswap_device->backing_storage_filename);
}

static ssize_t swap_filename_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	if (!vbswap_device) {
		pr_err("%s %d: vbswap_device is null\n", __func__, __LINE__);
		return len;
	}
	memcpy((void *)vbswap_device->backing_storage_filename,
			(void *)buf, len);
	dprintk("%s %d: (buf, len, backing_storage_filename) = " \
			"(%s, %d, %s)\n",
			__func__, __LINE__,
			buf, len, vbswap_device->backing_storage_filename);
	return len;
}

static ssize_t init_backing_storage_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "(disksize, bs_size) = (%llu, %llu)\n",
		vbswap_device ? vbswap_device->disksize : 0,
		vbswap_device ? vbswap_device->bs_size : 0);
}

static ssize_t init_backing_storage_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	vbswap_init_backing_storage();
	return len;
}

static ssize_t deinit_backing_storage_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	vbswap_deinit_backing_storage();
	return len;
}

static ssize_t vbswap_init_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "(vbswap_is_init, init_success) = (%llu, %d)\n",
			vbswap_device->stats.vbswap_is_init,
			(vbswap_device) ? vbswap_device->init_success : 0);
}

static ssize_t vbswap_swap_info_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "(%d, %d, %d) (%llu, %d, %d, %d, %d, %d) " \
						"(%d, %d, %d, %d, %d, %d, " \
						"%d, %d, %d, %d, %d, %d)\n",
		vbswap_device->stats.vbswap_stored_pages.counter,
		vbswap_device->stats.vbswap_write_pages.counter,
		vbswap_device->stats.vbswap_read_pages.counter,
		vbswap_device->stats.vbswap_total_slot_num,
		vbswap_device->stats.vbswap_used_slot_num.counter,
		vbswap_device->stats.vbswap_backing_storage_full_num.counter,
		vbswap_device->stats.vbswap_mapped_slot_free_num.counter,
		vbswap_device->stats.vbswap_not_mapped_slot_free_num.counter,
		vbswap_device->stats.vbswap_double_mapped_slot_num.counter,
		vbswap_device->stats.vbswap_bio_end_fail_r1_num.counter,
		vbswap_device->stats.vbswap_bio_end_fail_r2_num.counter,
		vbswap_device->stats.vbswap_bio_end_fail_r3_num.counter,
		vbswap_device->stats.vbswap_bio_end_fail_w1_num.counter,
		vbswap_device->stats.vbswap_bio_end_fail_w2_num.counter,
		vbswap_device->stats.vbswap_bio_end_fail_w3_num.counter,
		vbswap_device->stats.vbswap_bio_large_bi_size_num.counter,
		vbswap_device->stats.vbswap_bio_large_bi_vcnt_num.counter,
		vbswap_device->stats.vbswap_bio_invalid_num.counter,
		vbswap_device->stats.vbswap_bio_no_mem_num.counter,
		vbswap_device->stats.vbswap_not_mapped_read_pages.counter,
		vbswap_device->stats.vbswap_backing_storage_open_fail
	);
}

static DEVICE_ATTR(disksize, S_IRUGO | S_IWUSR, disksize_show,
	disksize_store);
static DEVICE_ATTR(swap_filename, S_IRUGO | S_IWUSR, swap_filename_show,
	swap_filename_store);
static DEVICE_ATTR(init_backing_storage, S_IRUGO | S_IWUSR,
	init_backing_storage_show, init_backing_storage_store);
static DEVICE_ATTR(deinit_backing_storage, S_IWUSR, NULL,
		deinit_backing_storage_store);
static DEVICE_ATTR(vbswap_init, S_IRUGO | S_IWUSR,
	vbswap_init_show, NULL);
static DEVICE_ATTR(vbswap_swap_info, S_IRUGO | S_IWUSR,
	vbswap_swap_info_show, NULL);

static struct attribute *vbswap_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_swap_filename.attr,
	&dev_attr_init_backing_storage.attr,
	&dev_attr_deinit_backing_storage.attr,
	&dev_attr_vbswap_init.attr,
	&dev_attr_vbswap_swap_info.attr,
	NULL,
};

static struct attribute_group vbswap_disk_attr_group = {
	.attrs = vbswap_disk_attrs,
};

static int create_device(struct vbswap *vbswap)
{
	int ret = 0;

	init_rwsem(&vbswap->lock);

	vbswap->queue = blk_alloc_queue(GFP_KERNEL);
	if (!vbswap->queue) {
		pr_err("%s %d: Error allocating disk queue for device\n",
				__func__, __LINE__);
		ret = -ENOMEM;
		goto out;
	}

	blk_queue_make_request(vbswap->queue, vbswap_make_request);
	vbswap->queue->queuedata = vbswap;

	 /* gendisk structure */
	vbswap->disk = alloc_disk(1);
	if (!vbswap->disk) {
		blk_cleanup_queue(vbswap->queue);
		pr_err("%s %d: Error allocating disk structure for device\n",
				__func__, __LINE__);
		ret = -ENOMEM;
		goto out_free_queue;
	}

	vbswap->disk->major = vbswap_major;
	vbswap->disk->first_minor = 0;
	vbswap->disk->fops = &vbswap_devops;
	vbswap->disk->queue = vbswap->queue;
	vbswap->disk->private_data = vbswap;
	snprintf(vbswap->disk->disk_name, 16, "vbswap%d", 0);

	/* Actual capacity set using sysfs (/sys/block/vbswap<id>/disksize) */
	set_capacity(vbswap->disk, 0);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(vbswap->disk->queue,
		PAGE_SIZE);
	blk_queue_logical_block_size(vbswap->disk->queue,
		VBSWAP_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(vbswap->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(vbswap->disk->queue, PAGE_SIZE);
	blk_queue_max_hw_sectors(vbswap->disk->queue,
		PAGE_SIZE / SECTOR_SIZE);

	add_disk(vbswap->disk);

	vbswap->disksize = 0;
	vbswap->bs_size = 0;
	vbswap->init_success = 0;

	ret = sysfs_create_group(&disk_to_dev(vbswap->disk)->kobj,
			&vbswap_disk_attr_group);
	if (ret < 0) {
		pr_err("%s %d: Error creating sysfs group\n",
			__func__, __LINE__);
		goto out_put_disk;
	}

	/* vbswap devices sort of resembles non-rotational disks */
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT,
		vbswap->disk->queue);

	swap_header_page =  alloc_page(__GFP_HIGHMEM);

	if (!swap_header_page) {
		pr_err("%s %d: Error creating swap_header_page\n",
			__func__, __LINE__);
		ret = -ENOMEM;
		goto remove_vbswap_group;
	}

out:
	return ret;

remove_vbswap_group:
	sysfs_remove_group(&disk_to_dev(vbswap->disk)->kobj,
		&vbswap_disk_attr_group);

out_put_disk:
	put_disk(vbswap->disk);

out_free_queue:
	blk_cleanup_queue(vbswap->queue);

	return ret;
}

static void destroy_device(struct vbswap *vbswap)
{
	if (vbswap->disk)
		sysfs_remove_group(&disk_to_dev(vbswap->disk)->kobj,
			&vbswap_disk_attr_group);

	if (vbswap->disk) {
		del_gendisk(vbswap->disk);
		put_disk(vbswap->disk);
	}

	if (vbswap->queue)
		blk_cleanup_queue(vbswap->queue);
}

static int __init vbswap_init(void)
{
	int ret = 0;

	vbswap_major = register_blkdev(0, "vbswap");
	if (vbswap_major <= 0) {
		pr_err("%s %d: Unable to get major number\n",
			__func__, __LINE__);
		ret = -EBUSY;
		goto out;
	}

	/* Initialize global variables */
	vbswap_table = NULL;
	backing_storage_bitmap = NULL;
	backing_storage_bmap = NULL;
	backing_storage_bdev = NULL;
	backing_storage_file = NULL;

	/* Allocate and initialize the device */
	vbswap_device = kzalloc(sizeof(struct vbswap), GFP_KERNEL);
	if (!vbswap_device) {
		ret = -ENOMEM;
		pr_err("%s %d: Unable to allocate vbswap_device\n",
			__func__, __LINE__);
		goto unregister;
	}

	ret = create_device(vbswap_device);
	if (ret) {
		pr_err("%s %d: Unable to create vbswap_device\n",
			__func__, __LINE__);
		goto free_devices;
	}

	vbswap_device->stats.vbswap_is_init = 1;

	return 0;

free_devices:
	kfree(vbswap_device);

unregister:
	unregister_blkdev(vbswap_major, "vbswap");

out:
	return ret;
}

static void __exit vbswap_exit(void)
{
	destroy_device(vbswap_device);

	unregister_blkdev(vbswap_major, "vbswap");

	if (backing_storage_file)
		filp_close(backing_storage_file, NULL);
	if (swap_header_page)
		__free_page(swap_header_page);
	kfree(vbswap_device);
	if (backing_storage_bmap)
		vfree(backing_storage_bmap);
	if (backing_storage_bitmap)
		vfree(backing_storage_bitmap);
	if (vbswap_table)
		vfree(vbswap_table);

	dprintk("%s %d: Cleanup done!\n", __func__, __LINE__);
}

module_init(vbswap_init);
module_exit(vbswap_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Park Ju Hyung <qkrwngud825@gmail.com>");
MODULE_DESCRIPTION("Virtual block swap device based on vnswap");
