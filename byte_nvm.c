//
// Created by ZN on 2022年1月4日.
//
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/dax.h>
#include "byte_nvm.h"
#include "f2fs.h"
#include "segment.h"
//依据byte nsb设置nsbi的相关字段
//与块设备相比添加DAX信息
unsigned int init_byte_nvm_sb_info(struct f2fs_sb_info *sbi, struct nvm_super_block *nsb) {
	struct nvm_sb_info *byte_nsbi = sbi->byte_nsbi;
	int i = 0;
	int j = 0;
	int ret = 0;
	long dax_blk_nums = 0;
	void *virt_addr = NULL;
	pfn_t __pfn_t;
	struct dax_device *dax_dev;
	
	byte_nsbi->nsb = nsb;
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
	byte_nsbi->byte_private = kzalloc(sizeof(struct byte_private), GFP_KERNEL);
	if(!byte_nsbi->byte_private)
		return -ENOMEM;
	byte_nsbi->byte_private->dax_dev = dax_dev;
	/* 获取dax映射虚拟地址 */
	dax_blk_nums = dax_direct_access(byte_nsbi->byte_private->dax_dev, 0, LONG_MAX/PAGE_SIZE, 
									&virt_addr, &__pfn_t);

	if (dax_blk_nums <= 0) {
		nvm_debug(NVM_ERR, "direct_access failed\n");
		return -EINVAL;
	}

	//ZN: 检查是否可访问块数量一致
	printk(KERN_INFO"ZN trap: dax accessable block :%d",dax_blk_nums);
	printk(KERN_INFO"ZN trap: device accessable block :%d",SECTOR_TO_BLOCK(byte_nsbi->nbdev->bd_part->nr_sects));

	byte_nsbi->byte_private->virt_addr = virt_addr;
	if (!byte_nsbi->byte_private->virt_addr) {
		nvm_debug(NVM_ERR, "ioremap failed\n");
		return -EINVAL;
	}


	/* ZN:内存中保存各种区域的位图，version_map和dirty_map应该是论文MPT里的后面两位标志 */
	/* 1、为MPT版本位图申请空间,单位：字节 */
	byte_nsbi->mpt_ver_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(nsb->mpt_ver_map_bits), GFP_KERNEL);
	/* 2、为main区域segment位图申请空间 */
	byte_nsbi->segment_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(nsb->main_segment_nums), GFP_KERNEL);
	byte_nsbi->ckpt_segment_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(nsb->main_segment_nums), GFP_KERNEL);
	/* 3、为MPT表脏页位图申请空间 */
	/* MPT脏页位图大小 和 MPT版本位图大小 相同 */
	byte_nsbi->mpt_dirty_map_bits = nsb->mpt_ver_map_bits;
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

	printk(KERN_INFO"ZN trap: mpt_entries size = %d", nsb->mpt_entries);
	printk(KERN_INFO"ZN trap: mpt size = %d ",((nsb->mpt_entries * sizeof(unsigned int) - 1) / PAGE_SIZE + 1) * PAGE_SIZE);
	/* 不分配mpt cache ，mpt地址直接指向dax映射区域 */
	byte_nsbi->mpt = (unsigned int)byte_nsbi->byte_private->virt_addr 
										+ byte_nsbi->nsb->mpt_blkaddr * PAGE_SIZE / sizeof(unsigned int);
	// byte_nsbi->mpt = f2fs_kvzalloc(sbi, ((nsb->mpt_entries * sizeof(unsigned int) - 1) / PAGE_SIZE + 1) * PAGE_SIZE,
	// 						  GFP_KERNEL);
	
	/* 4.为lfu_count cache分配内存空间，为了对NVM段进行访问计数，方便对NVM段进行垃圾回收[最后多一个空间用于记录访问总数] */
	for (i = 0; i < LFU_LEVELS; i++) {
		byte_nsbi->lfu_count[i] = f2fs_kvzalloc(sbi,
										   (((sbi->raw_super->segment_count_main + 1) * sizeof(atomic_t) - 1) /
											PAGE_SIZE + 1) * PAGE_SIZE, GFP_KERNEL);
		//初始化值为-1
		for (j = 0; j < sbi->raw_super->segment_count_main; j++) {
			atomic_set(&byte_nsbi->lfu_count[i][j], -1);
		}
		//初始化总计数为0
		atomic_set(&byte_nsbi->lfu_count[i][sbi->raw_super->segment_count_main], 0);
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

	return 0;
}
/**
 * 将mpt cache拷贝回byte nvm mapping区域
 * TODO：真的要按页刷回吗，直接在nvm中更细更好一些，但是dax如何确保数据刷回到nvm中呢
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