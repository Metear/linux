// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext2/dir.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/dir.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 directory handling functions
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 * All code that works with directory layout had been switched to pagecache
 * and moved here. AV
 */

#include "ext2.h"
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/iversion.h>

typedef struct ext2_dir_entry_2 ext2_dirent;

/*
 * Tests against MAX_REC_LEN etc were put in place for 64k block
 * sizes; if that is not possible on this arch, we can skip
 * those tests and speed things up.
 */
static inline unsigned ext2_rec_len_from_disk(__le16 dlen)
{
	unsigned len = le16_to_cpu(dlen);

#if (PAGE_SIZE >= 65536)
	if (len == EXT2_MAX_REC_LEN)
		return 1 << 16;
#endif
	return len;
}

static inline __le16 ext2_rec_len_to_disk(unsigned len)
{
#if (PAGE_SIZE >= 65536)
	if (len == (1 << 16))
		return cpu_to_le16(EXT2_MAX_REC_LEN);
	else
		BUG_ON(len > (1 << 16));
#endif
	return cpu_to_le16(len);
}

/*
 * ext2 uses block-sized chunks. Arguably, sector-sized ones would be
 * more robust, but we have what we have
 */
static inline unsigned ext2_chunk_size(struct inode *inode)
{
	return inode->i_sb->s_blocksize;
}

/*
 * Return the offset into page `page_nr' of the last valid
 * byte in that page, plus one.
 */
static unsigned
ext2_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = inode->i_size;

	last_byte -= page_nr << PAGE_SHIFT;
	if (last_byte > PAGE_SIZE)
		last_byte = PAGE_SIZE;
	return last_byte;
}

static void ext2_commit_chunk(struct folio *folio, loff_t pos, unsigned len)
{
	struct address_space *mapping = folio->mapping;
	struct inode *dir = mapping->host;

	inode_inc_iversion(dir);
	block_write_end(pos, len, len, folio);

	if (pos+len > dir->i_size) {
		i_size_write(dir, pos+len);
		mark_inode_dirty(dir);
	}
	folio_unlock(folio);
}

static bool ext2_check_folio(struct folio *folio, int quiet, char *kaddr)
{
	struct inode *dir = folio->mapping->host;
	struct super_block *sb = dir->i_sb;
	unsigned chunk_size = ext2_chunk_size(dir);
	u32 max_inumber = le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count);
	unsigned offs, rec_len;
	unsigned limit = folio_size(folio);
	ext2_dirent *p;
	char *error;

	if (dir->i_size < folio_pos(folio) + limit) {
		limit = offset_in_folio(folio, dir->i_size);
		if (limit & (chunk_size - 1))
			goto Ebadsize;
		if (!limit)
			goto out;
	}
	for (offs = 0; offs <= limit - EXT2_DIR_REC_LEN(1); offs += rec_len) {
		p = (ext2_dirent *)(kaddr + offs);
		rec_len = ext2_rec_len_from_disk(p->rec_len);

		if (unlikely(rec_len < EXT2_DIR_REC_LEN(1)))
			goto Eshort;
		if (unlikely(rec_len & 3))
			goto Ealign;
		if (unlikely(rec_len < EXT2_DIR_REC_LEN(p->name_len)))
			goto Enamelen;
		if (unlikely(((offs + rec_len - 1) ^ offs) & ~(chunk_size-1)))
			goto Espan;
		if (unlikely(le32_to_cpu(p->inode) > max_inumber))
			goto Einumber;
	}
	if (offs != limit)
		goto Eend;
out:
	folio_set_checked(folio);
	return true;

	/* Too bad, we had an error */

Ebadsize:
	if (!quiet)
		ext2_error(sb, __func__,
			"size of directory #%lu is not a multiple "
			"of chunk size", dir->i_ino);
	goto fail;
Eshort:
	error = "rec_len is smaller than minimal";
	goto bad_entry;
Ealign:
	error = "unaligned directory entry";
	goto bad_entry;
Enamelen:
	error = "rec_len is too small for name_len";
	goto bad_entry;
Espan:
	error = "directory entry across blocks";
	goto bad_entry;
Einumber:
	error = "inode out of bounds";
bad_entry:
	if (!quiet)
		ext2_error(sb, __func__, "bad entry in directory #%lu: : %s - "
			"offset=%llu, inode=%lu, rec_len=%d, name_len=%d",
			dir->i_ino, error, folio_pos(folio) + offs,
			(unsigned long) le32_to_cpu(p->inode),
			rec_len, p->name_len);
	goto fail;
Eend:
	if (!quiet) {
		p = (ext2_dirent *)(kaddr + offs);
		ext2_error(sb, "ext2_check_folio",
			"entry in directory #%lu spans the page boundary"
			"offset=%llu, inode=%lu",
			dir->i_ino, folio_pos(folio) + offs,
			(unsigned long) le32_to_cpu(p->inode));
	}
fail:
	return false;
}

/*
 * Calls to ext2_get_folio()/folio_release_kmap() must be nested according
 * to the rules documented in kmap_local_folio()/kunmap_local().
 *
 * NOTE: ext2_find_entry() and ext2_dotdot() act as a call
 * to folio_release_kmap() and should be treated as a call to
 * folio_release_kmap() for nesting purposes.
 */
static void *ext2_get_folio(struct inode *dir, unsigned long n,
				   int quiet, struct folio **foliop)
{
	struct address_space *mapping = dir->i_mapping;
	struct folio *folio = read_mapping_folio(mapping, n, NULL);
	void *kaddr;

	if (IS_ERR(folio))
		return ERR_CAST(folio);
	kaddr = kmap_local_folio(folio, 0);
	if (unlikely(!folio_test_checked(folio))) {
		if (!ext2_check_folio(folio, quiet, kaddr))
			goto fail;
	}
	*foliop = folio;
	return kaddr;

fail:
	folio_release_kmap(folio, kaddr);
	return ERR_PTR(-EIO);
}

/*
 * NOTE! unlike strncmp, ext2_match returns 1 for success, 0 for failure.
 *
 * len <= EXT2_NAME_LEN and de != NULL are guaranteed by caller.
 */
static inline int ext2_match (int len, const char * const name,
					struct ext2_dir_entry_2 * de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode)
		return 0;
	return !memcmp(name, de->name, len);
}

/*
 * p is at least 6 bytes before the end of page
 */
static inline ext2_dirent *ext2_next_entry(ext2_dirent *p)
{
	return (ext2_dirent *)((char *)p +
			ext2_rec_len_from_disk(p->rec_len));
}

static inline unsigned 
ext2_validate_entry(char *base, unsigned offset, unsigned mask)
{
	ext2_dirent *de = (ext2_dirent*)(base + offset);
	ext2_dirent *p = (ext2_dirent*)(base + (offset&mask));
	while ((char*)p < (char*)de) {
		if (p->rec_len == 0)
			break;
		p = ext2_next_entry(p);
	}
	return offset_in_page(p);
}

static inline void ext2_set_de_type(ext2_dirent *de, struct inode *inode)
{
	if (EXT2_HAS_INCOMPAT_FEATURE(inode->i_sb, EXT2_FEATURE_INCOMPAT_FILETYPE))
		de->file_type = fs_umode_to_ftype(inode->i_mode);
	else
		de->file_type = 0;
}

static int
ext2_readdir(struct file *file, struct dir_context *ctx)
{
	loff_t pos = ctx->pos;
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	unsigned int offset = pos & ~PAGE_MASK;
	unsigned long n = pos >> PAGE_SHIFT;
	unsigned long npages = dir_pages(inode);
	unsigned chunk_mask = ~(ext2_chunk_size(inode)-1);
	bool need_revalidate = !inode_eq_iversion(inode, *(u64 *)file->private_data);
	bool has_filetype;

	if (pos > inode->i_size - EXT2_DIR_REC_LEN(1))
		return 0;

	has_filetype =
		EXT2_HAS_INCOMPAT_FEATURE(sb, EXT2_FEATURE_INCOMPAT_FILETYPE);

	for ( ; n < npages; n++, offset = 0) {
		ext2_dirent *de;
		struct folio *folio;
		char *kaddr = ext2_get_folio(inode, n, 0, &folio);
		char *limit;

		if (IS_ERR(kaddr)) {
			ext2_error(sb, __func__,
				   "bad page in #%lu",
				   inode->i_ino);
			ctx->pos += PAGE_SIZE - offset;
			return PTR_ERR(kaddr);
		}
		if (unlikely(need_revalidate)) {
			if (offset) {
				offset = ext2_validate_entry(kaddr, offset, chunk_mask);
				ctx->pos = (n<<PAGE_SHIFT) + offset;
			}
			*(u64 *)file->private_data = inode_query_iversion(inode);
			need_revalidate = false;
		}
		de = (ext2_dirent *)(kaddr+offset);
		limit = kaddr + ext2_last_byte(inode, n) - EXT2_DIR_REC_LEN(1);
		for ( ;(char*)de <= limit; de = ext2_next_entry(de)) {
			if (de->rec_len == 0) {
				ext2_error(sb, __func__,
					"zero-length directory entry");
				folio_release_kmap(folio, de);
				return -EIO;
			}
			if (de->inode) {
				unsigned char d_type = DT_UNKNOWN;

				if (has_filetype)
					d_type = fs_ftype_to_dtype(de->file_type);

				if (!dir_emit(ctx, de->name, de->name_len,
						le32_to_cpu(de->inode),
						d_type)) {
					folio_release_kmap(folio, de);
					return 0;
				}
			}
			ctx->pos += ext2_rec_len_from_disk(de->rec_len);
		}
		folio_release_kmap(folio, kaddr);
	}
	return 0;
}

/*
 *	ext2_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the page in which the entry was found (as a parameter - res_page),
 * and the entry itself. Page is returned mapped and unlocked.
 * Entry is guaranteed to be valid.
 *
 * On Success folio_release_kmap() should be called on *foliop.
 *
 * NOTE: Calls to ext2_get_folio()/folio_release_kmap() must be nested
 * according to the rules documented in kmap_local_folio()/kunmap_local().
 *
 * ext2_find_entry() and ext2_dotdot() act as a call to ext2_get_folio()
 * and should be treated as a call to ext2_get_folio() for nesting
 * purposes.
 */
struct ext2_dir_entry_2 *ext2_find_entry (struct inode *dir,
			const struct qstr *child, struct folio **foliop)
{
	const char *name = child->name;
	int namelen = child->len;
	unsigned reclen = EXT2_DIR_REC_LEN(namelen);
	unsigned long start, n;
	unsigned long npages = dir_pages(dir);
	struct ext2_inode_info *ei = EXT2_I(dir);
	ext2_dirent * de;

	if (npages == 0)
		goto out;

	start = ei->i_dir_start_lookup;
	if (start >= npages)
		start = 0;
	n = start;
	do {
		char *kaddr = ext2_get_folio(dir, n, 0, foliop);
		if (IS_ERR(kaddr))
			return ERR_CAST(kaddr);

		de = (ext2_dirent *) kaddr;
		kaddr += ext2_last_byte(dir, n) - reclen;
		while ((char *) de <= kaddr) {
			if (de->rec_len == 0) {
				ext2_error(dir->i_sb, __func__,
					"zero-length directory entry");
				folio_release_kmap(*foliop, de);
				goto out;
			}
			if (ext2_match(namelen, name, de))
				goto found;
			de = ext2_next_entry(de);
		}
		folio_release_kmap(*foliop, kaddr);

		if (++n >= npages)
			n = 0;
		/* next folio is past the blocks we've got */
		if (unlikely(n > (dir->i_blocks >> (PAGE_SHIFT - 9)))) {
			ext2_error(dir->i_sb, __func__,
				"dir %lu size %lld exceeds block count %llu",
				dir->i_ino, dir->i_size,
				(unsigned long long)dir->i_blocks);
			goto out;
		}
	} while (n != start);
out:
	return ERR_PTR(-ENOENT);

found:
	ei->i_dir_start_lookup = n;
	return de;
}

/*
 * Return the '..' directory entry and the page in which the entry was found
 * (as a parameter - p).
 *
 * On Success folio_release_kmap() should be called on *foliop.
 *
 * NOTE: Calls to ext2_get_folio()/folio_release_kmap() must be nested
 * according to the rules documented in kmap_local_folio()/kunmap_local().
 *
 * ext2_find_entry() and ext2_dotdot() act as a call to ext2_get_folio()
 * and should be treated as a call to ext2_get_folio() for nesting
 * purposes.
 */
struct ext2_dir_entry_2 *ext2_dotdot(struct inode *dir, struct folio **foliop)
{
	ext2_dirent *de = ext2_get_folio(dir, 0, 0, foliop);

	if (!IS_ERR(de))
		return ext2_next_entry(de);
	return NULL;
}

int ext2_inode_by_name(struct inode *dir, const struct qstr *child, ino_t *ino)
{
	struct ext2_dir_entry_2 *de;
	struct folio *folio;

	de = ext2_find_entry(dir, child, &folio);
	if (IS_ERR(de))
		return PTR_ERR(de);

	*ino = le32_to_cpu(de->inode);
	folio_release_kmap(folio, de);
	return 0;
}

static int ext2_prepare_chunk(struct folio *folio, loff_t pos, unsigned len)
{
	return __block_write_begin(folio, pos, len, ext2_get_block);
}

static int ext2_handle_dirsync(struct inode *dir)
{
	int err;

	err = filemap_write_and_wait(dir->i_mapping);
	if (!err)
		err = sync_inode_metadata(dir, 1);
	return err;
}

int ext2_set_link(struct inode *dir, struct ext2_dir_entry_2 *de,
		struct folio *folio, struct inode *inode, bool update_times)
{
	loff_t pos = folio_pos(folio) + offset_in_folio(folio, de);
	unsigned len = ext2_rec_len_from_disk(de->rec_len);
	int err;

	folio_lock(folio);
	err = ext2_prepare_chunk(folio, pos, len);
	if (err) {
		folio_unlock(folio);
		return err;
	}
	de->inode = cpu_to_le32(inode->i_ino);
	ext2_set_de_type(de, inode);
	ext2_commit_chunk(folio, pos, len);
	if (update_times)
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	EXT2_I(dir)->i_flags &= ~EXT2_BTREE_FL;
	mark_inode_dirty(dir);
	return ext2_handle_dirsync(dir);
}

/*
 *	Parent is locked.
 */
int ext2_add_link (struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned chunk_size = ext2_chunk_size(dir);
	unsigned reclen = EXT2_DIR_REC_LEN(namelen);
	unsigned short rec_len, name_len;
	struct folio *folio = NULL;
	ext2_dirent * de;
	unsigned long npages = dir_pages(dir);
	unsigned long n;
	loff_t pos;
	int err;

	/*
	 * We take care of directory expansion in the same loop.
	 * This code plays outside i_size, so it locks the folio
	 * to protect that region.
	 */
	for (n = 0; n <= npages; n++) {
		char *kaddr = ext2_get_folio(dir, n, 0, &folio);
		char *dir_end;

		if (IS_ERR(kaddr))
			return PTR_ERR(kaddr);
		folio_lock(folio);
		dir_end = kaddr + ext2_last_byte(dir, n);
		de = (ext2_dirent *)kaddr;
		kaddr += folio_size(folio) - reclen;
		while ((char *)de <= kaddr) {
			if ((char *)de == dir_end) {
				/* We hit i_size */
				name_len = 0;
				rec_len = chunk_size;
				de->rec_len = ext2_rec_len_to_disk(chunk_size);
				de->inode = 0;
				goto got_it;
			}
			if (de->rec_len == 0) {
				ext2_error(dir->i_sb, __func__,
					"zero-length directory entry");
				err = -EIO;
				goto out_unlock;
			}
			err = -EEXIST;
			if (ext2_match (namelen, name, de))
				goto out_unlock;
			name_len = EXT2_DIR_REC_LEN(de->name_len);
			rec_len = ext2_rec_len_from_disk(de->rec_len);
			if (!de->inode && rec_len >= reclen)
				goto got_it;
			if (rec_len >= name_len + reclen)
				goto got_it;
			de = (ext2_dirent *) ((char *) de + rec_len);
		}
		folio_unlock(folio);
		folio_release_kmap(folio, kaddr);
	}
	BUG();
	return -EINVAL;

got_it:
	pos = folio_pos(folio) + offset_in_folio(folio, de);
	err = ext2_prepare_chunk(folio, pos, rec_len);
	if (err)
		goto out_unlock;
	if (de->inode) {
		ext2_dirent *de1 = (ext2_dirent *) ((char *) de + name_len);
		de1->rec_len = ext2_rec_len_to_disk(rec_len - name_len);
		de->rec_len = ext2_rec_len_to_disk(name_len);
		de = de1;
	}
	de->name_len = namelen;
	memcpy(de->name, name, namelen);
	de->inode = cpu_to_le32(inode->i_ino);
	ext2_set_de_type (de, inode);
	ext2_commit_chunk(folio, pos, rec_len);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	EXT2_I(dir)->i_flags &= ~EXT2_BTREE_FL;
	mark_inode_dirty(dir);
	err = ext2_handle_dirsync(dir);
	/* OFFSET_CACHE */
out_put:
	folio_release_kmap(folio, de);
	return err;
out_unlock:
	folio_unlock(folio);
	goto out_put;
}

/*
 * ext2_delete_entry deletes a directory entry by merging it with the
 * previous entry. Page is up-to-date.
 */
int ext2_delete_entry(struct ext2_dir_entry_2 *dir, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	size_t from, to;
	char *kaddr;
	loff_t pos;
	ext2_dirent *de, *pde = NULL;
	int err;

	from = offset_in_folio(folio, dir);
	to = from + ext2_rec_len_from_disk(dir->rec_len);
	kaddr = (char *)dir - from;
	from &= ~(ext2_chunk_size(inode)-1);
	de = (ext2_dirent *)(kaddr + from);

	while ((char*)de < (char*)dir) {
		if (de->rec_len == 0) {
			ext2_error(inode->i_sb, __func__,
				"zero-length directory entry");
			return -EIO;
		}
		pde = de;
		de = ext2_next_entry(de);
	}
	if (pde)
		from = offset_in_folio(folio, pde);
	pos = folio_pos(folio) + from;
	folio_lock(folio);
	err = ext2_prepare_chunk(folio, pos, to - from);
	if (err) {
		folio_unlock(folio);
		return err;
	}
	if (pde)
		pde->rec_len = ext2_rec_len_to_disk(to - from);
	dir->inode = 0;
	ext2_commit_chunk(folio, pos, to - from);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	EXT2_I(inode)->i_flags &= ~EXT2_BTREE_FL;
	mark_inode_dirty(inode);
	return ext2_handle_dirsync(inode);
}

/*
 * Set the first fragment of directory.
 */
int ext2_make_empty(struct inode *inode, struct inode *parent)
{
	struct folio *folio = filemap_grab_folio(inode->i_mapping, 0);
	unsigned chunk_size = ext2_chunk_size(inode);
	struct ext2_dir_entry_2 * de;
	int err;
	void *kaddr;

	if (IS_ERR(folio))
		return PTR_ERR(folio);

	err = ext2_prepare_chunk(folio, 0, chunk_size);
	if (err) {
		folio_unlock(folio);
		goto fail;
	}
	kaddr = kmap_local_folio(folio, 0);
	memset(kaddr, 0, chunk_size);
	de = (struct ext2_dir_entry_2 *)kaddr;
	de->name_len = 1;
	de->rec_len = ext2_rec_len_to_disk(EXT2_DIR_REC_LEN(1));
	memcpy (de->name, ".\0\0", 4);
	de->inode = cpu_to_le32(inode->i_ino);
	ext2_set_de_type (de, inode);

	de = (struct ext2_dir_entry_2 *)(kaddr + EXT2_DIR_REC_LEN(1));
	de->name_len = 2;
	de->rec_len = ext2_rec_len_to_disk(chunk_size - EXT2_DIR_REC_LEN(1));
	de->inode = cpu_to_le32(parent->i_ino);
	memcpy (de->name, "..\0", 4);
	ext2_set_de_type (de, inode);
	kunmap_local(kaddr);
	ext2_commit_chunk(folio, 0, chunk_size);
	err = ext2_handle_dirsync(inode);
fail:
	folio_put(folio);
	return err;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
int ext2_empty_dir(struct inode *inode)
{
	struct folio *folio;
	char *kaddr;
	unsigned long i, npages = dir_pages(inode);

	for (i = 0; i < npages; i++) {
		ext2_dirent *de;

		kaddr = ext2_get_folio(inode, i, 0, &folio);
		if (IS_ERR(kaddr))
			return 0;

		de = (ext2_dirent *)kaddr;
		kaddr += ext2_last_byte(inode, i) - EXT2_DIR_REC_LEN(1);

		while ((char *)de <= kaddr) {
			if (de->rec_len == 0) {
				ext2_error(inode->i_sb, __func__,
					"zero-length directory entry");
				printk("kaddr=%p, de=%p\n", kaddr, de);
				goto not_empty;
			}
			if (de->inode != 0) {
				/* check for . and .. */
				if (de->name[0] != '.')
					goto not_empty;
				if (de->name_len > 2)
					goto not_empty;
				if (de->name_len < 2) {
					if (de->inode !=
					    cpu_to_le32(inode->i_ino))
						goto not_empty;
				} else if (de->name[1] != '.')
					goto not_empty;
			}
			de = ext2_next_entry(de);
		}
		folio_release_kmap(folio, kaddr);
	}
	return 1;

not_empty:
	folio_release_kmap(folio, kaddr);
	return 0;
}

static int ext2_dir_open(struct inode *inode, struct file *file)
{
	file->private_data = kzalloc(sizeof(u64), GFP_KERNEL);
	if (!file->private_data)
		return -ENOMEM;
	return 0;
}

static int ext2_dir_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static loff_t ext2_dir_llseek(struct file *file, loff_t offset, int whence)
{
	return generic_llseek_cookie(file, offset, whence,
				     (u64 *)file->private_data);
}

const struct file_operations ext2_dir_operations = {
	.open		= ext2_dir_open,
	.release	= ext2_dir_release,
	.llseek		= ext2_dir_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= ext2_readdir,
	.unlocked_ioctl = ext2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext2_compat_ioctl,
#endif
	.fsync		= ext2_fsync,
};
