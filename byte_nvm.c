//
// Created by ZN on 2022年1月4日.
//
#include <linux/blkdev.h>
#include <linux/fs.h>
#include "f2fs.h"
#include "byte_nvm.h"
#include "node.h"
#include "segment.h"

/* 
	初始化DAX设备，并设置byte_nsb在dax中的映射地址。
	对于可字节访问的nvm，采用dax直接访问内存地址方式
	来访问设备。于是byte_nsb指向内存的一个映射地址，
	对byte nvm 超级块的更新就是对该内存地址的数据进
	行修改。
 */
/******************************************************************************
 * super
 ********************************************************************************/
/**
 * 进行dax所必要的初始化，初始化流程参考nova
 * 同时返回物理超级块的位置
*/
int init_byte_nvm_dax(struct f2fs_sb_info *sbi, struct nvm_super_block **byte_nsb) {
	struct nvm_sb_info *byte_nsbi = sbi->byte_nsbi;
	struct dax_device *dax_dev;
	int ret;
	long dax_blk_nums = 0;
	void *virt_addr = NULL;
	pfn_t __pfn_t;

	*byte_nsb = NULL;
	/* ZN：获取DAX设备信息 */
	ret = bdev_dax_supported(byte_nsbi->nbdev, PAGE_SIZE);
	if (!ret) {
		nvm_debug(NVM_ERR, "device does not support DAX\n");
		return -EINVAL;
	}

	dax_dev = fs_dax_get_by_host(byte_nsbi->nbdev->bd_disk->disk_name);
	if (!dax_dev) {
		nvm_debug(NVM_ERR, "Couldn't retrieve DAX device.\n");
		return -EINVAL;
	}
	/* 分配并填写byte nvm私有数据 */
	byte_nsbi->byte_private = kzalloc(sizeof(struct byte_nvm_private), GFP_KERNEL);
	if(!byte_nsbi->byte_private){
		return -ENOMEM;
	}
		
	byte_nsbi->byte_private->dax_dev = dax_dev;
	/* 获取dax映射虚拟地址 */
	dax_blk_nums = dax_direct_access(byte_nsbi->byte_private->dax_dev, 0, LONG_MAX/PAGE_SIZE, 
									&virt_addr, &__pfn_t);

	if (dax_blk_nums <= 0) {
		nvm_debug(NVM_ERR, "direct_access failed\n");
		return -EINVAL;
	}

	//ZN: 查看是否可访问块数量一致（是一致的
	// printk(KERN_INFO"ZN trap: dax accessable block :%d",dax_blk_nums);
	// printk(KERN_INFO"ZN trap: device accessable block :%d",SECTOR_TO_BLOCK(byte_nsbi->nbdev->bd_part->nr_sects));

	byte_nsbi->byte_private->virt_addr = virt_addr;
	if (!byte_nsbi->byte_private->virt_addr) {
		nvm_debug(NVM_ERR, "ioremap failed\n");
		return -EINVAL;
	}
	/* 如果是第一次挂载， byte_nsb 保存在第一块 */
	if(byte_nsbi->nvm_flag & NVM_FIRST_MOUNR)
		*byte_nsb = (struct nvm_super_block*)virt_addr;
	else if (sbi->ckpt->ckpt_flags & CP_NSB_VER_FLAG)
	{/* 如果不是，则根据checkpoint中的标志进行选择 */
		unsigned char * blk_ptr = (unsigned char *)*byte_nsb;
		blk_ptr += sbi->blocksize;
		*byte_nsb = (struct nvm_super_block *)blk_ptr;
	}
		
	if(!(sbi->byte_nsbi->nvm_flag & NVM_FIRST_MOUNR)){
		/* 如果不是第一次，则需要到checkpoint里查看最新的 byte_nsb应该在哪一块 */
		if(!sbi->ckpt) {
			nvm_debug(NVM_ERR, "cp not ready\n");
			return -EINVAL;
		}
		/* version为1则在第1块 */
		if (sbi->ckpt->ckpt_flags & CP_NSB_VER_FLAG)
			*byte_nsb += 1;
	}
	return 0;
}

/**
 * @brief 初始化 byte nvm 私有参数
 * 
 * @param sbi 文件系统超级快数据
 * @return int 成功时返回0
 */
int init_byte_nvm_private_info(struct f2fs_sb_info *sbi)
{
	struct f2fs_super_block * sb = sbi->raw_super;
	struct byte_nvm_private * byte_nsb_private = F2FS_BYTE_NVM_PRIVATE(sbi);
	if (!sb)
	{
		nvm_debug(NVM_ERR, "raw super block is not ready\n");
		return -EINVAL;
	}
	/**
	 * 物理上nvm第一个segment归属于超级块，segment0从第二块开始数起
	 * 但是block0还是从nvm超级块所在位置开始算起
	 */
	byte_nsb_private->super_blkaddr		= 0;
	byte_nsb_private->segment0_blkaddr 	= 1 * sbi->blocks_per_seg;
	byte_nsb_private->cp_blkaddr		= le32_to_cpu(sb->segment0_blkaddr);
	byte_nsb_private->sit_blkaddr		= le32_to_cpu(sb->sit_blkaddr) 
											- le32_to_cpu(sb->cp_blkaddr) 
											+ byte_nsb_private->cp_blkaddr;
	byte_nsb_private->nat_blkaddr		= le32_to_cpu(sb->nat_blkaddr) 
											- le32_to_cpu(sb->cp_blkaddr) 
											+ byte_nsb_private->cp_blkaddr;
	byte_nsb_private->ssa_blkaddr		= le32_to_cpu(sb->ssa_blkaddr) 
											- le32_to_cpu(sb->cp_blkaddr) 
											+ byte_nsb_private->cp_blkaddr;
	// byte_nsb_private->s_flag |= BNVM_PRIVATE_READY;
	sbi->byte_nsbi->nvm_flag |= NVM_BYTE_PRIVATE_READY;
	return 0;
}
//依据byte nsb设置nsbi的相关字段
//与块设备相比添加DAX信息
int init_byte_nvm_sb_info(struct f2fs_sb_info *sbi, struct nvm_super_block *byte_nsb) {
	struct nvm_sb_info *byte_nsbi = sbi->byte_nsbi;
	int i = 0;
	int j = 0;
	int ret = 0;
	unsigned int segment_count_main = le32_to_cpu(sbi->raw_super->segment_count_main);
	byte_nsbi->nsb = byte_nsb;

	/* ZN:内存中保存各种区域的位图，version_map和dirty_map应该是论文MPT里的后面两位标志 */
	/* 1、为MPT版本位图申请空间,单位：字节 */
	byte_nsbi->mpt_ver_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(byte_nsb->mpt_ver_map_bits), GFP_KERNEL);
	/* 2、为main区域segment位图申请空间 */
	byte_nsbi->segment_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(byte_nsb->main_segment_nums), GFP_KERNEL);
	byte_nsbi->ckpt_segment_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(byte_nsb->main_segment_nums), GFP_KERNEL);
	/* 3、为MPT表脏页位图申请空间 */
	/* MPT脏页位图大小 和 MPT版本位图大小 相同 */
	byte_nsbi->mpt_dirty_map_bits = byte_nsb->mpt_ver_map_bits;
	byte_nsbi->mpt_dirty_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(byte_nsbi->mpt_dirty_map_bits), GFP_KERNEL);

	if (!(byte_nsbi->mpt_ver_map && byte_nsbi->segment_map && byte_nsbi->mpt_dirty_map)) {
		return -ENOMEM;
	}

	mutex_init(&byte_nsbi->nvmgc_mutex);

	spin_lock_init(&byte_nsbi->mpt_ver_map_lock);
	spin_lock_init(&byte_nsbi->segment_map_lock);
	spin_lock_init(&byte_nsbi->mpt_lock);
	spin_lock_init(&byte_nsbi->lfu_half_lock);
	spin_lock_init(&byte_nsbi->aqusz_lock);

	// printk(KERN_INFO"ZN trap: byte mpt_entries nmus = %d", byte_nsb->mpt_entries);
	
	/* 不分配mpt cache ，mpt地址直接指向dax映射区域 */
	byte_nsbi->mpt = (unsigned int)byte_nsbi->byte_private->virt_addr 
										+ byte_nsbi->nsb->mpt_blkaddr * PAGE_SIZE / sizeof(unsigned int);
	// byte_nsbi->mpt = f2fs_kvzalloc(sbi, ((nsb->mpt_entries * sizeof(unsigned int) - 1) / PAGE_SIZE + 1) * PAGE_SIZE,
	// 						  GFP_KERNEL);
	
	/* 4.为lfu_count cache分配内存空间，为了对NVM段进行访问计数，方便对NVM段进行垃圾回收[最后多一个空间用于记录访问总数] */
	for (i = 0; i < LFU_LEVELS; i++) {
		byte_nsbi->lfu_count[i] = f2fs_kvzalloc(sbi,
										   (((segment_count_main + 1) * sizeof(atomic_t) - 1) /
											PAGE_SIZE + 1) * PAGE_SIZE, GFP_KERNEL);
		//初始化值为-1
		for (j = 0; j < segment_count_main; j++) {
			atomic_set(&byte_nsbi->lfu_count[i][j], -1);
		}
		//初始化总计数为0
		atomic_set(&byte_nsbi->lfu_count[i][segment_count_main], 0);
	}
	byte_nsbi->now_lfu_counter = 0;//初始化当前计数器为第一个计数器
	byte_nsbi->mh = init_max_heap(sbi, NVM_TO_SSD);
	byte_nsbi->minh = init_min_heap(sbi, SSD_TO_NVM);
	byte_nsbi->nvm_gc_start_page = alloc_pages(GFP_KERNEL, sbi->log_blocks_per_seg);// 分配用于NVM-GC段迁移的连续物理内存页
	byte_nsbi->ssd_to_nvm_start_page = alloc_pages(GFP_KERNEL, sbi->log_blocks_per_seg);// 分配用于NVM-GC段迁移的连续物理内存页
	nvm_assert(byte_nsbi->nvm_gc_start_page);
	nvm_assert(byte_nsbi->ssd_to_nvm_start_page);

	if (!byte_nsbi->lfu_count[0] || !byte_nsbi->mpt)
		return -ENOMEM;

	ret = init_byte_nvm_private_info(sbi);
	return ret;
}
/**
 * 将mpt cache拷贝回byte nvm mapping区域
 * TODO：真的要按页刷回吗，直接在nvm中更新更好一些，但是dax如何确保数据刷回到nvm中呢
 * 暂时保留这个函数
 * @param sbi
 */
void byte_nvm_flush_mpt_pages(struct f2fs_sb_info *sbi, int flush_all){
	struct nvm_sb_info *byte_nsbi = sbi->byte_nsbi;
	int mpt_pgoff = 0;//找到的脏页偏移
	int dirty = 0;//是否有脏位
	void *dst_addr;//byte nvm 在内存中的dax地址
	void *src_addr;//mpt cache中开始拷贝的地址
	spin_lock(&byte_nsbi->mpt_lock);
	if(flush_all){
		//ZN:如果设置flush_all，无条件刷回所有MPT page，从第一块开始刷回
		mpt_pgoff = 0;
	} else {/* ZN：从0位置开始找比特位为1的位置，如果没有，则返回map的比特数（mpt_dirty_map_bits） */
		mpt_pgoff = find_next_bit(byte_nsbi->mpt_dirty_map, byte_nsbi->mpt_dirty_map_bits, 0);
	}

	while(mpt_pgoff != byte_nsbi->mpt_dirty_map_bits) {
		///设置NVM是否为脏标志位
		if (!dirty) {
			nvm_debug(NVM_DEBUG, "flush mpt");
			dirty = 1;
			//设置标志位：nvm映射表为脏
			byte_nsbi->nvm_flag |= NVM_NSB_DIRTY;
		}	
		//计算mpt cache中开始拷贝的地址
		src_addr = byte_nsbi->mpt + (PAGE_SIZE * mpt_pgoff / sizeof(unsigned int));
		dst_addr = byte_nsbi->byte_private->virt_addr + byte_nsbi->nsb->ra_blkaddr + mpt_pgoff;
		memcpy(dst_addr, src_addr, PAGE_SIZE);
		//处理完清除脏位
		clear_bit(mpt_pgoff, byte_nsbi->mpt_dirty_map);
		//处理下一个
		if (flush_all) {
			++mpt_pgoff;
		} else {
			mpt_pgoff = find_next_bit(byte_nsbi->mpt_dirty_map, byte_nsbi->mpt_dirty_map_bits, ++mpt_pgoff);
		}		

	}

}
/******************************************************************************
 * checkpoint
 ********************************************************************************/
/**
 * 将内存中的sbi->ckpt转移至bnvm上，调用之前确保ckpt已经在内存中
*/
int f2fs_move_cp_super_to_bnvm(struct f2fs_sb_info *sbi)
{
	unsigned int cp_blks = 1+ __cp_payload(sbi);
	struct nvm_sb_info *byte_nsbi  = F2FS_BYTE_NSB_I(sbi);
	struct f2fs_checkpoint *pre_ckpt = sbi->ckpt;
	struct f2fs_checkpoint *new_ckpt;
	unsigned char *src, *dst;
	if (!pre_ckpt)
		return -EINVAL;
	if (sbi->byte_nsbi->nvm_flag & NVM_BYTE_PRIVATE_READY)
		new_ckpt = f2fs_bnvm_get_cp_ptr(sbi, sbi->cur_cp_pack);
	else
		return -EINVAL;
	dst = (unsigned char *)new_ckpt;
	src = (unsigned char *)pre_ckpt;
	memcpy(dst, src, cp_blks * sbi->blocksize);
	kfree(src);
	sbi->ckpt = new_ckpt;
	byte_nsbi->nvm_flag |= NVM_BYTE_CP_SUPER_READY;
	return 0;
}

/**
 * 从SSD上把cp除超级块剩下的部分读取到byte nvm中
 * 
*/
int f2fs_move_cp_content_to_bvnm(struct f2fs_sb_info *sbi){
	struct nvm_sb_info *byte_nsbi  = F2FS_BYTE_NSB_I(sbi);
	unsigned char *dst = (unsigned char *)sbi->ckpt;
	unsigned char *src;
	unsigned int cp_blks = 1+ __cp_payload(sbi);
	unsigned int to_read_blks;
	unsigned int blocksize = sbi->blocksize;
	struct page *page;
	block_t start = start_sum_block(sbi);
	unsigned int i;
	if (!(sbi->byte_nsbi->nvm_flag & NVM_BYTE_CP_SUPER_READY))
	{
		printk(KERN_INFO"ZN trap: cp super is not in byte nvm");
		return -EINVAL;
	}
	dst += cp_blks * blocksize;
	to_read_blks = __le32_to_cpu(sbi->ckpt->cp_pack_total_block_count) - cp_blks;
	/* 把剩下的块读到nvm（orphen node + data summary + node summary + tail super） */
	for ( i = 0; i < to_read_blks; i++)
	{
		page = f2fs_get_meta_page(sbi, start++);
		if (!page)
			return -EINVAL;
		src = (unsigned char*)page_address(page);
		memcpy(dst, src, blocksize);
		f2fs_put_page(page, 1);
		dst += blocksize;
	}
	byte_nsbi->nvm_flag |= NVM_BYTE_CP_CONTENT_READY;
	return 0;
}
/******************************************************************************
 * do_checkpoint()
 ********************************************************************************/
/**
 * @param dst，nvm上orphan node区域起始地址
*/
void bnvm_write_orphan_inodes(struct f2fs_sb_info *sbi, unsigned char *dst)
{
	struct list_head *head;
	struct f2fs_orphan_block *orphan_blk = NULL;
	unsigned int nentries = 0;
	unsigned short index = 1;
	unsigned short orphan_blocks;
	struct ino_entry *orphan = NULL;
	struct inode_management *im = &sbi->im[ORPHAN_INO];
	orphan_blocks = GET_ORPHAN_BLOCKS(im->ino_num);

	head = &im->ino_list;
	orphan_blk = (struct f2fs_orphan_block *)dst;
	list_for_each_entry(orphan, head, list) {

		orphan_blk->ino[nentries++] = cpu_to_le32(orphan->ino);
		if (nentries == F2FS_ORPHANS_PER_BLOCK) {
			/*
				* an orphan block is full of 1020 entries,
				* then we need to flush current orphan blocks
				* and bring another one in memory
				*/
			orphan_blk->blk_addr = cpu_to_le16(index);
			orphan_blk->blk_count = cpu_to_le16(orphan_blocks);
			orphan_blk->entry_count = cpu_to_le32(nentries);
			dst += sbi->blocksize;
			orphan_blk = (struct f2fs_orphan_block *)dst;
			index++;
			nentries = 0;
		}
	}

	if (nentries != 0)
	{
		orphan_blk->blk_addr = cpu_to_le16(index);
		orphan_blk->blk_count = cpu_to_le16(orphan_blocks);
		orphan_blk->entry_count = cpu_to_le32(nentries);
	}
	
}
/**
 * 写入compacted summary
  */
void bnvm_write_compacted_summaries(struct f2fs_sb_info *sbi, unsigned char * dst)
{
	unsigned char *kaddr = dst;
	struct f2fs_summary *summary;
	struct curseg_info *seg_i;
	int written_size = 0;
	int i, j;

	/* Step 1: write nat cache */
	seg_i = CURSEG_I(sbi, CURSEG_HOT_DATA);
	memcpy(kaddr, seg_i->journal, SUM_JOURNAL_SIZE);
	written_size += SUM_JOURNAL_SIZE;

	/* Step 2: write sit cache */
	seg_i = CURSEG_I(sbi, CURSEG_COLD_DATA);
	memcpy(kaddr + written_size, seg_i->journal, SUM_JOURNAL_SIZE);
	written_size += SUM_JOURNAL_SIZE;

	/* Step 3: write summary entries */
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		unsigned short blkoff;
		seg_i = CURSEG_I(sbi, i);
		if (sbi->ckpt->alloc_type[i] == SSR)/* ZN：SSR是乱序分配，所以全部都要写入 */
			blkoff = sbi->blocks_per_seg;
		else
			blkoff = curseg_blkoff(sbi, i);

		for (j = 0; j < blkoff; j++) {
			summary = (struct f2fs_summary *)(kaddr + written_size);
			*summary = seg_i->sum_blk->entries[j];
			written_size += SUMMARY_SIZE;

			if (written_size + SUMMARY_SIZE <= PAGE_SIZE -
							SUM_FOOTER_SIZE)
				continue;/* ZN：仍没写满就跳过换页操作 */

			kaddr += sbi->blocksize;
			written_size = 0;
		}
	}
}
/**
 * 写入normal summary到byte nvm中
 * @param type，数据类型，从CURSEG_HOT_DATA，到CURSEG_COLD_NODE
 * @param dst_sum，summray block起始地址
  */
void bnvm_write_current_sum_page(struct f2fs_sb_info *sbi,
						int type, unsigned char dst_sum)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	struct f2fs_summary_block *src = curseg->sum_blk;
	struct f2fs_summary_block *dst = (struct f2fs_summary_block *)dst;
	
	memset(dst, 0, PAGE_SIZE);
		mutex_lock(&curseg->curseg_mutex);

	down_read(&curseg->journal_rwsem);
	memcpy(&dst->journal, curseg->journal, SUM_JOURNAL_SIZE);
	up_read(&curseg->journal_rwsem);

	memcpy(dst->entries, src->entries, SUM_ENTRY_SIZE);
	memcpy(&dst->footer, &src->footer, SUM_FOOTER_SIZE);

	mutex_unlock(&curseg->curseg_mutex);
}						

void bnvm_write_normal_summaries(struct f2fs_sb_info *sbi, unsigned char * dst, int type)
{
	int i, end;
	if (IS_DATASEG(type))
		end = type + NR_CURSEG_DATA_TYPE;
	else
		end = type + NR_CURSEG_NODE_TYPE;

	for (i = type; i < end; i++)
		bnvm_write_current_sum_page(sbi, i, dst + (i - type)*sbi->blocksize);
}

/**
 * byte nvm checkpoint写入data summary
 * @param dst，当前data summary首地址
*/
void bnvm_write_data_summaries(struct f2fs_sb_info *sbi, unsigned char * dst)
{
	if (is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG))
		bnvm_write_compacted_summaries(sbi, dst);
	else
		bnvm_write_normal_summaries(sbi, dst, CURSEG_HOT_DATA);
}

static void bnvm_commit_checkpoint(struct f2fs_sb_info *sbi,
	unsigned char *src, unsigned char *dst)
{
	memcpy(dst, src, PAGE_SIZE);
}

/**
 * 在非卸载的情况下，写入checkpoint只用写入到nvm
 * 如果要卸载f2fs，则需要落盘
 */
int bnvm_do_checkpoint(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	struct f2fs_checkpoint *cur_ckpt = F2FS_CKPT(sbi);
	struct f2fs_checkpoint *ckpt;/* 当前将要写入的ckpt */
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	unsigned long orphan_num = sbi->im[ORPHAN_INO].ino_num, flags;
	unsigned char * src , *dst;
	unsigned int data_sum_blocks, orphan_blocks;
	__u32 crc32 = 0;
	int i;
	int cp_payload_blks = __cp_payload(sbi);
	struct super_block *sb = sbi->sb;
	struct curseg_info *seg_i = CURSEG_I(sbi, CURSEG_HOT_NODE);
	struct page *page;
	unsigned int blocksize = sbi->blocksize;
	u64 kbytes_written;
	
	int err;	
	if (sbi->cur_cp_pack == 1) 
		ckpt = f2fs_bnvm_get_cp_ptr(sbi, 2);
	else
		ckpt = f2fs_bnvm_get_cp_ptr(sbi,1);
	
	/* ZN：如果META区存在脏page，则等待回写 */
	while (get_pages(sbi, F2FS_DIRTY_META)) {
		f2fs_sync_meta_pages(sbi, META, LONG_MAX, FS_CP_META_IO);
		if (unlikely(f2fs_cp_error(sbi)))
			return -EIO;
	}

	/* ZN：记录六个curseg的segno，blk_off和分配方式 */
	ckpt->elapsed_time = cpu_to_le64(get_mtime(sbi, true));
	ckpt->free_segment_count = cpu_to_le32(free_segments(sbi));
	for (i = 0; i < NR_CURSEG_NODE_TYPE; i++) {
		ckpt->cur_node_segno[i] =
			cpu_to_le32(curseg_segno(sbi, i + CURSEG_HOT_NODE));
		ckpt->cur_node_blkoff[i] =
			cpu_to_le16(curseg_blkoff(sbi, i + CURSEG_HOT_NODE));
		ckpt->alloc_type[i + CURSEG_HOT_NODE] =
				curseg_alloc_type(sbi, i + CURSEG_HOT_NODE);
	}
	for (i = 0; i < NR_CURSEG_DATA_TYPE; i++) {
		ckpt->cur_data_segno[i] =
			cpu_to_le32(curseg_segno(sbi, i + CURSEG_HOT_DATA));
		ckpt->cur_data_blkoff[i] =
			cpu_to_le16(curseg_blkoff(sbi, i + CURSEG_HOT_DATA));
		ckpt->alloc_type[i + CURSEG_HOT_DATA] =
				curseg_alloc_type(sbi, i + CURSEG_HOT_DATA);
	}

	/* 2 cp  + n data seg summary + orphan inode blocks */
	/* ZN：计算checkpoint各个部分需要写回的块数和起始位置 */
	data_sum_blocks = f2fs_npages_for_summary_flush(sbi, false);
	spin_lock_irqsave(&sbi->cp_lock, flags);
	if (data_sum_blocks < NR_CURSEG_DATA_TYPE)
		__set_ckpt_flags(ckpt, CP_COMPACT_SUM_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_COMPACT_SUM_FLAG);
	spin_unlock_irqrestore(&sbi->cp_lock, flags);

	/* 获取orphen block的数量，以计算ckpt中sum 的起始位置 */
	orphan_blocks = GET_ORPHAN_BLOCKS(orphan_num);
	ckpt->cp_pack_start_sum = cpu_to_le32(1 + cp_payload_blks +
			orphan_blocks);	

	update_ckpt_flags(sbi, cpc);
	//COMPLETED:在此切换nsb版本号：如果超级块为脏，切换版本号
	/*start*/
	if(sbi->nsbi->nvm_flag & NVM_NSB_DIRTY){
		nvm_switch_nsb_version(ckpt);
		//更新版本之后清除NVM_NSB_DIRTY标志
		sbi->nsbi->nvm_flag ^= NVM_NSB_DIRTY;
	}
	/*end*/

	/* update SIT/NAT bitmap */
	/* ZN：将sm_i和nm_i中的bitmap复制到内存中的cp super block */
	get_sit_bitmap(sbi, __bitmap_ptr(sbi, SIT_BITMAP));
	get_nat_bitmap(sbi, __bitmap_ptr(sbi, NAT_BITMAP));

	crc32 = f2fs_crc32(sbi, ckpt, le32_to_cpu(ckpt->checksum_offset));
	*((__le32 *)((unsigned char *)ckpt +
				le32_to_cpu(ckpt->checksum_offset)))
				= cpu_to_le32(crc32);

	/* write nat bits */
	if (enabled_nat_bits(sbi, cpc)) {
		__u64 cp_ver = cur_cp_version(cur_ckpt);
		unsigned char * nat_bitmap_ptr;
		cp_ver |= ((__u64)crc32 << 32);
		*(__le64 *)nm_i->nat_bits = cpu_to_le64(cp_ver);
		nat_bitmap_ptr = (unsigned char *)ckpt 
						+ (sbi->blocks_per_seg - nm_i->nat_bits_blocks) 
						* blocksize;
		for ( i = 0; i < nm_i->nat_bits_blocks; i++)
			memcpy(nat_bitmap_ptr + (i << F2FS_BLKSIZE_BITS), 
						nm_i->nat_bits + (i << F2FS_BLKSIZE_BITS), sbi->blocksize);
	}

	/* ZN：复制cp super block */
	src = (unsigned char*)cur_ckpt;
	dst = (unsigned char*)ckpt;
	memcpy(dst, src, blocksize);
	src += blocksize;
	dst += blocksize;
	
	/* 把附加块也写入（如果sit version bitmap需要额外块来保存的话） */
	for (i = 0; i < cp_payload_blks; i++)
	{
		memcpy(dst, src, blocksize);
		src += blocksize;
		dst += blocksize;		
	}

	if (orphan_num)
	{
		bnvm_write_orphan_inodes(sbi, dst);
		dst += orphan_blocks * blocksize;
	}
	
	bnvm_write_data_summaries(sbi, dst);
	dst += data_sum_blocks * blocksize;

	/* Record write statistics in the hot node summary */
	kbytes_written = sbi->kbytes_written;
	if (sb->s_bdev->bd_part)
		kbytes_written += BD_PART_WRITTEN(sbi);
	seg_i->journal->info.kbytes_written = cpu_to_le64(kbytes_written);

	//TODO：按设计来说将cp写入byte nvm不会产生这一步
	if (__remain_node_summaries(cpc->reason)) {
		f2fs_write_node_summaries(sbi, dst);// 将node summary以及里面的journal写入磁盘
		dst += NR_CURSEG_NODE_TYPE * blocksize;
	}

	/* update user_block_counts */
	sbi->last_valid_block_count = sbi->total_valid_block_count;
	percpu_counter_set(&sbi->alloc_valid_block_count, 0);

	//TODO:bnvm的硬件刷回要怎么做？
	err = f2fs_flush_device_cache(sbi);
	if (err)
		return err;
	
	bnvm_commit_checkpoint(sbi, ckpt, dst);

	f2fs_release_ino_entry(sbi, false);

	if (unlikely(f2fs_cp_error(sbi)))
		return -EIO;

	clear_sbi_flag(sbi, SBI_IS_DIRTY);
	clear_sbi_flag(sbi, SBI_NEED_CP);
	__set_cp_next_pack(sbi);

	if (get_pages(sbi, F2FS_DIRTY_NODES) ||
			get_pages(sbi, F2FS_DIRTY_IMETA))
		set_sbi_flag(sbi, SBI_IS_DIRTY);
	
	return 0;
}
/******************************************************************************
 * build_curseg()
 ********************************************************************************/
/**
 * 获取bnvm上的compacted_summaries，
 * 参考 read_compacted_summaries
*/
//TODO：是否需要加锁？
void bnvm_read_compacted_summaries(struct f2fs_sb_info *sbi)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct curseg_info *seg_i;
	unsigned char *kaddr;
	block_t start;
	int i, j, offset;

	start = f2fs_bnvm_get_cp_sum(sbi);
	// printk(KERN_INFO"ZN trap: nvm start_cp_addr %d", f2fs_bnvm_get_valid_cp_addr(sbi));
	// printk(KERN_INFO"ZN trap: nvm start sum blk %d", start);
	/* 已经读取整个cp pack到nvm中，就直接按字节访问 */
	kaddr = F2FS_BYTE_NVM_ADDR(sbi) + start * sbi->blocksize;
	/* 复制 nat journal */
	seg_i = CURSEG_I(sbi, CURSEG_HOT_DATA);
	memcpy(seg_i->journal, kaddr, SUM_JOURNAL_SIZE);

	/* 复制 sit journal，和nat journal 都在一个block上 */
	seg_i = CURSEG_I(sbi, CURSEG_COLD_DATA);
	memcpy(seg_i->journal, kaddr + SUM_JOURNAL_SIZE, SUM_JOURNAL_SIZE);
	offset = 2 * SUM_JOURNAL_SIZE;

	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		unsigned short blk_off;
		unsigned int segno;

		seg_i = CURSEG_I(sbi, i);
		segno = le32_to_cpu(ckpt->cur_data_segno[i]);
		blk_off = le16_to_cpu(ckpt->cur_data_blkoff[i]);
		seg_i->next_segno = segno;	

		reset_curseg(sbi, i, 0);
		seg_i->alloc_type = ckpt->alloc_type[i];
		seg_i->next_blkoff = blk_off;
		if (seg_i->alloc_type == SSR)
			blk_off = sbi->blocks_per_seg;

		for ( j = 0; j < blk_off; j++)
		{
			struct f2fs_summary *s;
			s = (struct f2fs_summary *)(kaddr + offset);
			seg_i->sum_blk->entries[j] = *s;
			offset += SUMMARY_SIZE;
			if (offset + SUMMARY_SIZE <= PAGE_SIZE -
						SUM_FOOTER_SIZE)
				continue;
			kaddr += sbi->blocksize;
			offset = 0;
		}
	}
}
/**
 * 获取bnvm上的 normal summaries
 * 参考 read_normal_summaries
*/
int bnvm_read_normal_summaries(struct f2fs_sb_info *sbi, int type)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_summary_block *sum;
	struct curseg_info *curseg;
	unsigned short blk_off;
	unsigned int segno = 0;
	block_t blk_addr = 0;
	if (IS_DATASEG(type)) {
		segno = le32_to_cpu(ckpt->cur_data_segno[type]);
		blk_off = le16_to_cpu(ckpt->cur_data_blkoff[type -
							CURSEG_HOT_DATA]);
		if (__exist_node_summaries(sbi))// 如果没有宕机的情况下，从checkpoint中恢复
			blk_addr = f2fs_bnvm_get_sum_blk_addr(sbi, NR_CURSEG_TYPE, type);
		else// 出现了宕机，则从SSA中恢复
			blk_addr = f2fs_bnvm_get_sum_blk_addr(sbi, NR_CURSEG_DATA_TYPE, type);
	} else {
		segno = le32_to_cpu(ckpt->cur_node_segno[type -
							CURSEG_HOT_NODE]);
		blk_off = le16_to_cpu(ckpt->cur_node_blkoff[type -
							CURSEG_HOT_NODE]);
		if (__exist_node_summaries(sbi))
			blk_addr = f2fs_bnvm_get_sum_blk_addr(sbi, NR_CURSEG_NODE_TYPE,
							type - CURSEG_HOT_NODE);
		else
			//TODO：2022年3月5日 SSA还没搬运上来，这里假设搬运上来了
			/* blk_addr = 0，表示直接从ssa区域获取数据 */
			blk_addr = 0;		
	}
	if (blk_addr)/* 从CP中获取 */
		sum = (struct f2fs_summary_block *)F2FS_BYTE_NVM_BLK_TO_ADDR(sbi, blk_addr);
	else/* 从SSA获取 */
		sum = f2fs_bnvm_get_sum_blk(sbi, segno);
	
	if (IS_NODESEG(type)){
		if (__exist_node_summaries(sbi)) {// 如果没有宕机的情况下，将每一个ns重新置为0
			struct f2fs_summary *ns = &sum->entries[0];
			int i;
			for (i = 0; i < sbi->blocks_per_seg; i++, ns++) {
				ns->version = 0;
				ns->ofs_in_node = 0;
			}
		} else {
			f2fs_restore_node_summary(sbi, segno, sum);
		}
	}
	/* 剩下的步骤就是复制summary block数据到curseg */
	/* set uncompleted segment to curseg */
	curseg = CURSEG_I(sbi, type);
	mutex_lock(&curseg->curseg_mutex);

	/* update journal info */
	down_write(&curseg->journal_rwsem);
	memcpy(curseg->journal, &sum->journal, SUM_JOURNAL_SIZE);
	up_write(&curseg->journal_rwsem);

	memcpy(curseg->sum_blk->entries, sum->entries, SUM_ENTRY_SIZE);
	memcpy(&curseg->sum_blk->footer, &sum->footer, SUM_FOOTER_SIZE);
	curseg->next_segno = segno;
	reset_curseg(sbi, type, 0);
	curseg->alloc_type = ckpt->alloc_type[type];/* LFS或SSR */
	curseg->next_blkoff = blk_off;
	mutex_unlock(&curseg->curseg_mutex);

	return 0;
}

/******************************************************************************
 * 搬运元数据函数
 ********************************************************************************/
int f2fs_bnvm_move_nat(struct f2fs_sb_info *sbi)
{
	return 0;
}
/******************************************************************************
 * 访问元数据区域函数
 ********************************************************************************/
inline struct nvm_super_block * f2fs_bnvm_get_raw_super(struct f2fs_sb_info *sbi)
{
	return (struct nvm_super_block *)(F2FS_BYTE_NVM_ADDR(sbi) 
									+ F2FS_BYTE_NVM_PRIVATE(sbi)->super_blkaddr 
									* F2FS_BLKSIZE);
}

inline struct f2fs_nat_entry * f2fs_bnvm_get_nat_entry(struct f2fs_sb_info *sbi, 
                                        nid_t nid)
{	/* ZN：原理看 current_nat_addr ()函数，另外nid是全局的node号 */
    struct f2fs_nm_info *nm_i = NM_I(sbi);
	pgoff_t block_off= NAT_BLOCK_OFFSET(nid);
	pgoff_t block_addr;
    struct f2fs_nat_block *nat_blk;

	block_addr = (pgoff_t)(F2FS_BYTE_NVM_PRIVATE(sbi)->nat_blkaddr +
		(block_off << 1) -	//ZN：注意NAT中segment副本成对出现，偏移要乘以2
		(block_off & (sbi->blocks_per_seg - 1)));//ZN：减去多乘的段内偏移
    
	if (f2fs_test_bit(block_off, nm_i->nat_bitmap))
		block_addr += sbi->blocks_per_seg;//ZN：检查bit_map如果在副本段上，则又要加上一段的偏移
	nat_blk = (struct f2fs_nat_block *)(F2FS_BYTE_NVM_ADDR(sbi)
									+ block_addr * F2FS_BLKSIZE);
	return &nat_blk->entries[nid % NAT_ENTRY_PER_BLOCK];
    
}

inline struct f2fs_sit_entry * f2fs_bnvm_get_sit_entry(struct f2fs_sb_info *sbi, 
                                        unsigned int segno)
{   /* ZN：原理看 current_sit_addr ()函数，另外start是全局的segment号 */
    struct sit_info *sit_i = SIT_I(sbi);
    unsigned int block_off = SIT_BLOCK_OFFSET(segno);
    unsigned int block_addr = F2FS_BYTE_NVM_PRIVATE(sbi)->sit_blkaddr 
                                + block_off;
    struct f2fs_sit_block *sit_blk;
	if (f2fs_test_bit(block_off, sit_i->sit_bitmap))
		block_addr += sit_i->sit_blocks;   
    sit_blk = (struct f2fs_sit_block *)(F2FS_BYTE_NVM_ADDR(sbi)
                                    + block_addr * F2FS_BLKSIZE);
    return &sit_blk->entries[segno % SIT_ENTRY_PER_BLOCK];
}

inline struct f2fs_summary_block * f2fs_bnvm_get_sum_blk(struct f2fs_sb_info *sbi, 
										unsigned int segno)
{
	unsigned int block_addr = F2FS_BYTE_NVM_PRIVATE(sbi)->ssa_blkaddr + segno;
	return (struct f2fs_summary_block *)(F2FS_BYTE_NVM_ADDR(sbi)
									+ block_addr * F2FS_BLKSIZE);
}

/* 
 * 测试函数
 */
void print_byte_nvm_mount_parameter(struct f2fs_sb_info *sbi){
	struct nvm_sb_info *byte_nsbi = F2FS_BYTE_NSB_I(sbi);
	struct nvm_super_block *byte_nsb = byte_nsbi->nsb;
	printk(KERN_INFO"ZN trap: =================================");
	printk(KERN_INFO"ZN trap:      byte nvm mount parameter    ");
	printk(KERN_INFO"ZN trap: mpt_blkaddr 		%d",byte_nsb->mpt_blkaddr);
	printk(KERN_INFO"ZN trap: ra_blkaddr 		%d",byte_nsb->ra_blkaddr);
	printk(KERN_INFO"ZN trap: main_blkaddr		%d",byte_nsb->main_blkaddr);
	printk(KERN_INFO"ZN trap: ra_blk_nums		%d",byte_nsb->ra_blk_nums);
	printk(KERN_INFO"ZN trap: main_first_segno	%d",byte_nsb->main_first_segno);
	printk(KERN_INFO"ZN trap: main_segment_nums	%d",byte_nsb->main_segment_nums);
	printk(KERN_INFO"ZN trap: mpt_ver_map_bits	%d",byte_nsb->mpt_ver_map_bits);
	printk(KERN_INFO"ZN trap: mpt_entries		%d",byte_nsb->mpt_entries);
	printk(KERN_INFO"ZN trap: =================================");
}

void print_raw_info(struct f2fs_sb_info *sbi) {
	printk(KERN_INFO"ZN trap: =================================");
	printk(KERN_INFO"ZN trap:      meta area parameter    ");
	printk(KERN_INFO"ZN trap: segment0_blkaddr		%d",sbi->raw_super->segment0_blkaddr);
	printk(KERN_INFO"ZN trap: cp_blkaddr			%d",sbi->raw_super->cp_blkaddr);
	printk(KERN_INFO"ZN trap: segment_count_ckpt	%d",sbi->raw_super->segment_count_ckpt);
	printk(KERN_INFO"ZN trap: nat_blkaddr			%d",sbi->raw_super->nat_blkaddr);
	printk(KERN_INFO"ZN trap: segment_count_nat		%d",sbi->raw_super->segment_count_nat);
	printk(KERN_INFO"ZN trap: sit_blkaddr			%d",sbi->raw_super->sit_blkaddr);
	printk(KERN_INFO"ZN trap: segment_count_sit		%d",sbi->raw_super->segment_count_sit);
	printk(KERN_INFO"ZN trap: ssa_blkaddr			%d",sbi->raw_super->ssa_blkaddr);
	printk(KERN_INFO"ZN trap: segment_count_ssa		%d",sbi->raw_super->segment_count_ssa);
	printk(KERN_INFO"ZN trap: =================================");
}

void print_ckpt_info(struct f2fs_sb_info *sbi) {
	struct f2fs_checkpoint *cp = F2FS_CKPT(sbi);
	if(!sbi->ckpt){
		printk(KERN_INFO"ZN trap: checkpoint is not ready");
		return ;
	}
	printk(KERN_INFO"ZN trap: =================================");
	printk(KERN_INFO"ZN trap:      checkpoint information    ");
	printk(KERN_INFO"ZN trap: cp_blks						%d",1 + __cp_payload(sbi));
	printk(KERN_INFO"ZN trap: cp_pay_load 					%d",__cp_payload(sbi));/* ZN：cp super block的附加块数量 */
	printk(KERN_INFO"ZN trap: cur_cp_pack					%d",sbi->cur_cp_pack);
	printk(KERN_INFO"ZN trap: user_block_count				%d",cp->user_block_count);
	printk(KERN_INFO"ZN trap: valid_block_count				%d",cp->valid_block_count);
	printk(KERN_INFO"ZN trap: valid_node_count				%d",cp->valid_node_count);
	printk(KERN_INFO"ZN trap: free_segment_count			%d",cp->free_segment_count);
	printk(KERN_INFO"ZN trap: cp_pack_total_block_count		%d",cp->cp_pack_total_block_count);
	printk(KERN_INFO"ZN trap: cp_pack_start_sum				%d",cp->cp_pack_start_sum);
	printk(KERN_INFO"ZN trap: alloc_type[CURSEG_HOT_DATA]	%d",cp->alloc_type[CURSEG_HOT_DATA]);
	printk(KERN_INFO"ZN trap: =================================");		
}

