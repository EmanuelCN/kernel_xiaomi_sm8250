// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual block swap device based on vnswap
 *
 * Copyright (C) 2020 Park Ju Hyung
 * Copyright (C) 2013 SungHwan Yun
 */

#define pr_fmt(fmt) "vbswap: " fmt

// #define DEBUG

#include <linux/module.h>
#include <linux/blkdev.h>

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1 << SECTOR_SHIFT)
#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define VBSWAP_LOGICAL_BLOCK_SHIFT 12
#define VBSWAP_LOGICAL_BLOCK_SIZE	(1 << VBSWAP_LOGICAL_BLOCK_SHIFT)
#define VBSWAP_SECTOR_PER_LOGICAL_BLOCK	(1 << \
	(VBSWAP_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))

struct vbswap {
	struct request_queue *queue;
	struct gendisk *disk;
	u64 disksize;	/* bytes */
	int init_success;
};

/* Globals */
static int vbswap_major;
static struct vbswap *vbswap_device;
static struct page *swap_header_page;

static void vbswap_init_disksize(u64 disksize)
{
	if (vbswap_device->init_success) {
		pr_err("%s %d: disksize is already initialized (disksize = %llu)\n",
				__func__, __LINE__, vbswap_device->disksize);
		return;
	}

	vbswap_device->disksize = PAGE_ALIGN(disksize);
	if (!vbswap_device->disksize) {
		pr_err("%s %d: disksize is invalid (disksize = %llu)\n",
		       __func__, __LINE__, vbswap_device->disksize);
		vbswap_device->disksize = 0;
		vbswap_device->init_success = 0;
		return;
	}
	set_capacity(vbswap_device->disk,
		     vbswap_device->disksize >> SECTOR_SHIFT);

	vbswap_device->init_success = 1;
}

static int vbswap_bvec_read(struct vbswap *vbswap, struct bio_vec bvec,
			    u32 index, struct bio *bio)
{
	struct page *page;
	unsigned char *user_mem, *swap_header_page_mem;

	if (unlikely(index != 0)) {
		pr_err("tried to read outside of swap header\n");
		return -EIO;
	}

	page = bvec.bv_page;

	user_mem = kmap_atomic(page);
	swap_header_page_mem = kmap_atomic(swap_header_page);
	memcpy(user_mem, swap_header_page_mem, bvec.bv_len);
	kunmap_atomic(swap_header_page_mem);
	kunmap_atomic(user_mem);
	flush_dcache_page(page);

	return 0;
}

static int vbswap_bvec_write(struct vbswap *vbswap, struct bio_vec bvec,
			     u32 index, struct bio *bio)
{
	struct page *page;
	unsigned char *user_mem, *swap_header_page_mem;

	if (unlikely(index != 0)) {
		pr_err("tried to write outside of swap header\n");
		return -EIO;
	}

	page = bvec.bv_page;

	user_mem = kmap_atomic(page);
	swap_header_page_mem = kmap_atomic(swap_header_page);
	memcpy(swap_header_page_mem, user_mem, PAGE_SIZE);
	kunmap_atomic(swap_header_page_mem);
	kunmap_atomic(user_mem);

	return 0;
}

static int vbswap_bvec_rw(struct vbswap *vbswap, struct bio_vec bvec,
			  u32 index, struct bio *bio, int rw)
{
	int ret;

	if (rw == READ) {
		pr_debug("%s %d: (rw,index) = (%d, %d)\n",
			 __func__, __LINE__, rw, index);
		ret = vbswap_bvec_read(vbswap, bvec, index, bio);
	} else {
		pr_debug("%s %d: (rw,index) = (%d, %d)\n",
			 __func__, __LINE__, rw, index);
		ret = vbswap_bvec_write(vbswap, bvec, index, bio);
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

	pr_debug("%s %d: (rw, index, offset, bi_size) = "
		 "(%d, %d, %d, %d)\n",
		 __func__, __LINE__, rw, index, offset, bio->bi_iter.bi_size);

	if (offset) {
		pr_err("%s %d: invalid offset. "
		       "(bio->bi_iter.bi_sector, index, offset) = (%llu, %d, %d)\n",
		       __func__, __LINE__,
		       (unsigned long long)bio->bi_iter.bi_sector,
		       index, offset);
		goto out_error;
	}

	if (bio->bi_iter.bi_size > PAGE_SIZE) {
		goto out_error;
	}

	if (bio->bi_vcnt > 1) {
		goto out_error;
	}

	bio_for_each_segment(bvec, bio, iter) {
		if (bvec.bv_len != PAGE_SIZE || bvec.bv_offset != 0) {
			pr_err("%s %d: bvec is misaligned. "
			       "(bv_len, bv_offset) = (%d, %d)\n",
			       __func__, __LINE__, bvec.bv_len, bvec.bv_offset);
			goto out_error;
		}

		pr_debug("%s %d: (rw, index, bvec.bv_len) = "
			 "(%d, %d, %d)\n",
			 __func__, __LINE__, rw, index, bvec.bv_len);

		ret = vbswap_bvec_rw(vbswap, bvec, index, bio, rw);
		if (ret < 0) {
			if (ret != -ENOSPC)
				pr_err("%s %d: vbswap_bvec_rw failed."
				       "(ret) = (%d)\n",
				       __func__, __LINE__, ret);
			else
				pr_debug("%s %d: vbswap_bvec_rw failed. "
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
static blk_qc_t vbswap_make_request(struct request_queue *queue,
				    struct bio *bio)
{
	struct vbswap *vbswap;

	if (likely(bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT)) {
		bio_io_error(bio);
		return BLK_QC_T_NONE;
	}

	vbswap = queue->queuedata;

	if (!vbswap_valid_io_request(vbswap, bio)) {
		pr_err("%s %d: invalid io request. "
		       "(bio->bi_iter.bi_sector, bio->bi_iter.bi_size,"
		       "vbswap->disksize) = "
		       "(%llu, %d, %llu)\n",
		       __func__, __LINE__,
		       (unsigned long long)bio->bi_iter.bi_sector,
		       bio->bi_iter.bi_size, vbswap->disksize);

		bio_io_error(bio);
		return BLK_QC_T_NONE;
	}

	__vbswap_make_request(vbswap, bio, bio_data_dir(bio));
	return BLK_QC_T_NONE;
}

static const struct block_device_operations vbswap_fops = {
	.owner = THIS_MODULE
};

static ssize_t disksize_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", vbswap_device->disksize);
}

static ssize_t disksize_store(struct device *dev,
			      struct device_attribute *attr, const char *buf,
			      size_t len)
{
	int ret;
	u64 disksize;

	ret = kstrtoull(buf, 10, &disksize);
	if (ret)
		return ret;

	vbswap_init_disksize(disksize);
	return len;
}

static DEVICE_ATTR(disksize, S_IRUGO | S_IWUSR, disksize_show, disksize_store);

static struct attribute *vbswap_disk_attrs[] = {
	&dev_attr_disksize.attr,
	NULL,
};

static struct attribute_group vbswap_disk_attr_group = {
	.attrs = vbswap_disk_attrs,
};

static int create_device(struct vbswap *vbswap)
{
	int ret;

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
	vbswap->disk->fops = &vbswap_fops;
	vbswap->disk->queue = vbswap->queue;
	vbswap->disk->private_data = vbswap;
	snprintf(vbswap->disk->disk_name, 16, "vbswap%d", 0);

	/* Actual capacity set using sysfs (/sys/block/vbswap<id>/disksize) */
	set_capacity(vbswap->disk, 0);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(vbswap->disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(vbswap->disk->queue,
				     VBSWAP_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(vbswap->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(vbswap->disk->queue, PAGE_SIZE);
	blk_queue_max_hw_sectors(vbswap->disk->queue, PAGE_SIZE / SECTOR_SIZE);

	add_disk(vbswap->disk);

	vbswap->disksize = 0;
	vbswap->init_success = 0;

	ret = sysfs_create_group(&disk_to_dev(vbswap->disk)->kobj,
				 &vbswap_disk_attr_group);
	if (ret < 0) {
		pr_err("%s %d: Error creating sysfs group\n",
		       __func__, __LINE__);
		goto out_put_disk;
	}

	/* vbswap devices sort of resembles non-rotational disks */
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, vbswap->disk->queue);

	swap_header_page = alloc_page(__GFP_HIGHMEM);

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
	int ret;

	vbswap_major = register_blkdev(0, "vbswap");
	if (vbswap_major <= 0) {
		pr_err("%s %d: Unable to get major number\n",
		       __func__, __LINE__);
		ret = -EBUSY;
		goto out;
	}

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

	if (swap_header_page)
		__free_page(swap_header_page);
	kfree(vbswap_device);

	pr_debug("%s %d: Cleanup done!\n", __func__, __LINE__);
}

module_init(vbswap_init);
module_exit(vbswap_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Park Ju Hyung <qkrwngud825@gmail.com>");
MODULE_DESCRIPTION("Virtual block swap device based on vnswap");
