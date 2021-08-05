// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 * Copyright (C) 2021, Alibaba Cloud
 */
#include "internal.h"
#include <linux/prefetch.h>
#include <linux/iomap.h>

#include <linux/blkdev.h>
#include <linux/uio.h>

#include <trace/events/erofs.h>

struct page *erofs_get_meta_page(struct super_block *sb, erofs_blk_t blkaddr)
{
	struct address_space *const mapping = sb->s_bdev->bd_inode->i_mapping;
	struct page *page;

	page = read_cache_page_gfp(mapping, blkaddr,
				   mapping_gfp_constraint(mapping, ~__GFP_FS));
	/* should already be PageUptodate */
	if (!IS_ERR(page))
		lock_page(page);
	return page;
}

static int erofs_map_blocks_flatmode(struct inode *inode,
				     struct erofs_map_blocks *map,
				     int flags)
{
	erofs_blk_t nblocks, lastblk;
	u64 offset = map->m_la;
	struct erofs_inode *vi = EROFS_I(inode);
	bool tailendpacking = (vi->datalayout == EROFS_INODE_FLAT_INLINE);

	nblocks = DIV_ROUND_UP(inode->i_size, PAGE_SIZE);
	lastblk = nblocks - tailendpacking;

	/* there is no hole in flatmode */
	map->m_flags = EROFS_MAP_MAPPED;
	if (offset < blknr_to_addr(lastblk)) {
		map->m_pa = blknr_to_addr(vi->raw_blkaddr) + map->m_la;
		map->m_plen = blknr_to_addr(lastblk) - offset;
	} else if (tailendpacking) {
		/* 2 - inode inline B: inode, [xattrs], inline last blk... */
		struct erofs_sb_info *sbi = EROFS_SB(inode->i_sb);

		map->m_pa = iloc(sbi, vi->nid) + vi->inode_isize +
			vi->xattr_isize + erofs_blkoff(map->m_la);
		map->m_plen = inode->i_size - offset;

		/* inline data should be located in the same meta block */
		if (erofs_blkoff(map->m_pa) + map->m_plen > EROFS_BLKSIZ) {
			erofs_err(inode->i_sb,
				  "inline data cross block boundary @ nid %llu",
				  vi->nid);
			DBG_BUGON(1);
			return -EFSCORRUPTED;
		}
		map->m_flags |= EROFS_MAP_META;
	} else {
		erofs_err(inode->i_sb,
			  "internal error @ nid: %llu (size %llu), m_la 0x%llx",
			  vi->nid, inode->i_size, map->m_la);
		DBG_BUGON(1);
		return -EIO;
	}
	return 0;
}

static int erofs_map_blocks(struct inode *inode,
			    struct erofs_map_blocks *map, int flags)
{
	struct super_block *sb = inode->i_sb;
	struct erofs_inode *vi = EROFS_I(inode);
	struct erofs_inode_chunk_index *idx;
	struct page *page;
	u64 chunknr;
	unsigned int unit;
	erofs_off_t pos;
	int err = 0;
	
	trace_erofs_map_blocks_enter(inode, map, flags);
	if (map->m_la >= inode->i_size) {
		/* leave out-of-bound access unmapped */
		map->m_flags = 0;
		map->m_plen = 0;
		goto out;
	}

	if (vi->datalayout != EROFS_INODE_CHUNK_BASED) {
		err = erofs_map_blocks_flatmode(inode, map, flags);
		goto out;
	}

	if (vi->chunkformat & EROFS_CHUNK_FORMAT_INDEXES)
		unit = sizeof(*idx);			/* chunk index */
	else
		unit = EROFS_BLOCK_MAP_ENTRY_SIZE;	/* block map */

	chunknr = map->m_la >> vi->chunkbits;
	pos = ALIGN(iloc(EROFS_SB(sb), vi->nid) + vi->inode_isize +
		    vi->xattr_isize, unit) + unit * chunknr;

	page = erofs_get_meta_page(inode->i_sb, erofs_blknr(pos));
	if (IS_ERR(page)) {
		err = PTR_ERR(page);
		goto out;
	}
	map->m_la = chunknr << vi->chunkbits;
	map->m_plen = min_t(erofs_off_t, 1UL << vi->chunkbits,
			    roundup(inode->i_size - map->m_la, EROFS_BLKSIZ));

	/* handle block map */
	if (!(vi->chunkformat & EROFS_CHUNK_FORMAT_INDEXES)) {
		__le32 *blkaddr = page_address(page) + erofs_blkoff(pos);

		if (le32_to_cpu(*blkaddr) == EROFS_NULL_ADDR) {
			map->m_flags = 0;
		} else {
			map->m_pa = blknr_to_addr(le32_to_cpu(*blkaddr));
			map->m_flags = EROFS_MAP_MAPPED;
		}
		goto out_unlock;
	}
	/* parse chunk indexes */
	idx = page_address(page) + erofs_blkoff(pos);
	switch (le32_to_cpu(idx->blkaddr)) {
	case EROFS_NULL_ADDR:
		map->m_flags = 0;
		break;
	default:
		/* only one device is supported for now */
		if (idx->device_id) {
			erofs_err(sb, "invalid device id %u @ %llu for nid %llu",
				  le16_to_cpu(idx->device_id),
				  chunknr, vi->nid);
			err = -EFSCORRUPTED;
			goto out_unlock;
		}
		map->m_pa = blknr_to_addr(le32_to_cpu(idx->blkaddr));
		map->m_flags = EROFS_MAP_MAPPED;
		break;
	}
out_unlock:
	unlock_page(page);
	put_page(page);
out:
	if (!err)
		map->m_llen = map->m_plen;
	trace_erofs_map_blocks_exit(inode, map, flags, 0);
	return err;
}

static int erofs_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		unsigned int flags, struct iomap *iomap)
{
	int ret;
	struct erofs_map_blocks map;

	map.m_la = offset;
	map.m_llen = length;

	ret = erofs_map_blocks(inode, &map, EROFS_GET_BLOCKS_RAW);
	if (ret < 0)
		return ret;

	iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = map.m_la;
	iomap->length = map.m_llen;
	iomap->flags = 0;
	iomap->private = NULL;

	if (!(map.m_flags & EROFS_MAP_MAPPED)) {
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		if (!iomap->length)
			iomap->length = length;
		return 0;
	}

	if (map.m_flags & EROFS_MAP_META) {
		struct page *ipage;

		iomap->type = IOMAP_INLINE;
		ipage = erofs_get_meta_page(inode->i_sb,
					    erofs_blknr(map.m_pa));
		if (IS_ERR(ipage))
			return PTR_ERR(ipage);
		iomap->inline_data = page_address(ipage) +
					erofs_blkoff(map.m_pa);
		iomap->private = ipage;
	} else {
		iomap->type = IOMAP_MAPPED;
		iomap->addr = map.m_pa;
	}
	return 0;
}

static int erofs_iomap_end(struct inode *inode, loff_t pos, loff_t length,
		ssize_t written, unsigned int flags, struct iomap *iomap)
{
	struct page *ipage = iomap->private;

	if (ipage) {
		DBG_BUGON(iomap->type != IOMAP_INLINE);
		unlock_page(ipage);
		put_page(ipage);
	} else {
		DBG_BUGON(iomap->type == IOMAP_INLINE);
	}
	return written;
}

static const struct iomap_ops erofs_iomap_ops = {
	.iomap_begin = erofs_iomap_begin,
	.iomap_end = erofs_iomap_end,
};

/*
 * since we dont have write or truncate flows, so no inode
 * locking needs to be held at the moment.
 */
static int erofs_readpage(struct file *file, struct page *page)
{
	return iomap_readpage(page, &erofs_iomap_ops);
}

static int erofs_readpages(struct file *file,
			struct address_space *mapping,
			struct list_head *pages, unsigned nr_pages)
{
	return iomap_readpages(mapping, pages, nr_pages, &erofs_iomap_ops);
}

static sector_t erofs_bmap(struct address_space *mapping, sector_t block)
{
	return iomap_bmap(mapping, block, &erofs_iomap_ops);
}

static int erofs_prepare_dio(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t align = iocb->ki_pos | iov_iter_count(to) |
		iov_iter_alignment(to);
	struct block_device *bdev = inode->i_sb->s_bdev;
	unsigned int blksize_mask;

	if (bdev)
		blksize_mask = (1 << ilog2(bdev_logical_block_size(bdev))) - 1;
	else
		blksize_mask = (1 << inode->i_blkbits) - 1;

	if (align & blksize_mask)
		return -EINVAL;
	return 0;
}

static ssize_t erofs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	/* no need taking (shared) inode lock since it's a ro filesystem */
	if (!iov_iter_count(to))
		return 0;

	if (iocb->ki_flags & IOCB_DIRECT) {
		int err = erofs_prepare_dio(iocb, to);

		if (!err)
			return iomap_dio_rw(iocb, to, &erofs_iomap_ops, NULL);
		if (err < 0)
			return err;
	}
	return generic_file_read_iter(iocb, to);
}

/* for uncompressed (aligned) files and raw access for other files */
const struct address_space_operations erofs_raw_access_aops = {
	.readpage = erofs_readpage,
	.readpages = erofs_readpages,
	.bmap = erofs_bmap,
	.direct_IO = noop_direct_IO,
};

const struct file_operations erofs_file_fops = {
	.llseek		= generic_file_llseek,
	.read_iter	= erofs_file_read_iter,
	.mmap		= generic_file_readonly_mmap,
	.splice_read	= generic_file_splice_read,
};
