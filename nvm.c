//
// Created by ypf on 18-11-29.
//
//TODO：2022年1月6日 ZN 添加新的文件导致编译失败？

#include <linux/blkdev.h>
#include <linux/fs.h>
#include "nvm.h"
#include "segment.h"
#include "f2fs.h"
/**
 * 将mpt cache拷贝到mapping区域
 * ZN：拷贝到nsbi的地址空间，也就是高速缓存
 * @param sbi
 */
void nvm_flush_mpt_pages(struct f2fs_sb_info *sbi, int flush_all) {
	struct nvm_sb_info *nsbi = sbi->nsbi;

	int mpt_pgoff = 0;//找到的脏页偏移
	int dirty = 0;//是否有脏位
	void *dst_addr;//dst_page映射的虚拟地址，拷贝时候的地址
	void *src_addr;//mpt cache中开始拷贝的地址
	struct page *mpt_page;

	spin_lock(&nsbi->mpt_lock);
	//如果设置flush_all，无条件刷回所有MPT page
	if (flush_all) {
		mpt_pgoff = 0;
	} else {/* ZN：从0位置开始找比特位为1的位置，如果没有，则返回map的比特数（mpt_dirty_map_bits） */
		mpt_pgoff = find_next_bit(nsbi->mpt_dirty_map, nsbi->mpt_dirty_map_bits, 0);
	}

	while (mpt_pgoff != nsbi->mpt_dirty_map_bits) {
		///设置NVM是否为脏标志位
		if (!dirty) {
			nvm_debug(NVM_DEBUG, "flush mpt");
			dirty = 1;
			//设置标志位：nvm映射表为脏
			nsbi->nvm_flag |= NVM_NSB_DIRTY;
		}
		//计算mpt cache中开始拷贝的地址
		src_addr = nsbi->mpt + (PAGE_SIZE * mpt_pgoff / sizeof(unsigned int));

		//根据偏移得到要写的MPT page:有效的那一个，返回锁定的page
		mpt_page = get_next_mpt_page(sbi, mpt_pgoff);
		dst_addr = page_address(mpt_page);
		memcpy(dst_addr, src_addr, PAGE_SIZE);
		if (!PageDirty(mpt_page))
			set_page_dirty(mpt_page);
		f2fs_put_page(mpt_page, 1);

		//处理完清除脏位
		clear_bit(mpt_pgoff, nsbi->mpt_dirty_map);
		//处理下一个
		if (flush_all) {
			++mpt_pgoff;
		} else {
			mpt_pgoff = find_next_bit(nsbi->mpt_dirty_map, nsbi->mpt_dirty_map_bits, ++mpt_pgoff);
		}
	}
	spin_unlock(&nsbi->mpt_lock);

}

//获取下一个要使用的有效mpt page（函数主要选定双份seg中有效的那个）,返回锁定的page
//ZN：双份segment？论文里是用了两个segment段做备份。
struct page *get_next_mpt_page(struct f2fs_sb_info *sbi, int mpt_pgoff) {
	struct nvm_sb_info *nsbi = sbi->nsbi;

	struct page *dst_page;//实际要写的mapping中page地址
	pgoff_t block_addr;//实际写的page对应硬盘的块号，也是META区域的偏移

	block_addr = nsbi->nsb->mpt_blkaddr + mpt_pgoff;/* ZN：这是在NVM上的物理地址 */
	///根据版本位图决定返回双份seg哪一个seg中的地址
	if (!f2fs_test_bit(mpt_pgoff, nsbi->mpt_ver_map))
		block_addr += sbi->blocks_per_seg;
	//翻转位，切换版本（设置为当前使用的版本）
	f2fs_change_bit(mpt_pgoff, sbi->nsbi->mpt_ver_map);
	//获取要写的page，这里只获取内存page，因为我们要拷贝进去的是最新数据
	dst_page = f2fs_grab_meta_page(sbi, block_addr);
	return dst_page;
}

/**
 * 将cache中nsb刷回mapping page
 * @param sbi
 */
void nvm_flush_nsb_pages(struct f2fs_sb_info *sbi, pgoff_t block_addr) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	struct page *dst_page;//实际要写的mapping中page地址
	struct nvm_super_block *dst_addr;
	int mpt_ver_map_bytes = (nsbi->nsb->mpt_ver_map_bits - 1) / BITS_PER_BYTE + 1;//mpt版本位图占用字节数
	int segment_map_bytes = (nsbi->nsb->main_segment_nums - 1) / BITS_PER_BYTE + 1;//segment位图占用字节数

	//读取page
	dst_page = f2fs_grab_meta_page(sbi, block_addr);
	dst_addr = page_address(dst_page);
	//拷贝nsb固定长度
	memcpy(dst_addr, nsbi->nsb, sizeof(struct nvm_super_block));
	//拷贝mpt版本位图
	memcpy(&dst_addr->map[0], nsbi->mpt_ver_map, mpt_ver_map_bytes);
	//拷贝segment位图
	memcpy(&dst_addr->map[0] + mpt_ver_map_bytes, nsbi->segment_map, segment_map_bytes);

	if (!PageDirty(dst_page))
		set_page_dirty(dst_page);
	f2fs_put_page(dst_page, 1);
}

void nvm_move_lfu_count(struct f2fs_sb_info *sbi) {
	int i, clear_index, pre_index = sbi->nsbi->now_lfu_counter;
	if (pre_index == 0) {
		clear_index = LFU_LEVELS - 1;
	} else {
		clear_index = pre_index - 1;
	}
	//清空最后一级计数器
	for (i = 0; i < sbi->raw_super->segment_count_main; i++) {
		if (atomic_read(&sbi->nsbi->lfu_count[pre_index][i]) == -1) {
			//之前该段没有使用，直接跳过
			continue;
		} else {
			//之前该段有使用，则计数归零
			atomic_set(&sbi->nsbi->lfu_count[clear_index][i], 0);
		}
	}
	//切换计数器
	sbi->nsbi->now_lfu_counter = clear_index;
}

/**
 * 将cache中lfu刷回mapping page
 * @param sbi：
 * @param flag: 表示lfu的在双份中的哪一个中,0表示在第一份中，1表示在第二份中
 */
void nvm_flush_lfu_pages(struct f2fs_sb_info *sbi, int flag) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	int i;
	int lfu_blkaddr;// lfu区域的起始和结束块地址
	void *dst_addr;//dst_page映射的虚拟地址，拷贝时候的地址
	void *src_addr;//mpt cache中开始拷贝的地址
	struct page *lfu_page;

	int lfu_blk_num = ((sbi->raw_super->segment_count_main + 1) * sizeof(atomic_t) - 1) / PAGE_SIZE + 1;

	if (flag & CP_LFU_VER_FLAG) {
		lfu_blkaddr = nsbi->nsb->lfu_blkaddr1;
	} else {
		lfu_blkaddr = nsbi->nsb->lfu_blkaddr0;
	}

	nvm_debug(NVM_DEBUG, "nvm_flush_lfu_pages,lfu_blk_num=%d", lfu_blk_num);
	for (i = 0; i < lfu_blk_num; ++i) {
		//计算lfu cache中开始拷贝的地址
		src_addr = nsbi->lfu_count + (PAGE_SIZE * i / sizeof(atomic_t));
		//根据物理块地址，得到要写的lfu page
		lfu_page = f2fs_grab_meta_page(sbi, lfu_blkaddr);
		dst_addr = page_address(lfu_page);
		memcpy(dst_addr, src_addr, PAGE_SIZE);

		if (!PageDirty(lfu_page))
			set_page_dirty(lfu_page);
		f2fs_put_page(lfu_page, 1);
		lfu_blkaddr++;
	}
}

//
static bool use_ssd(struct f2fs_sb_info *sbi, unsigned int index) {
	//判断条件：1、写负载足够低；2、nvm io队列足够长；3、有double标志
	unsigned int ratio;
	unsigned int local_ssd_aqu_sz = 0;
	unsigned int local_nvm_aqu_sz = 0;

	if (write_ratio < write_threshold && get_double_flag(sbi->nsbi, index)) {

		local_ssd_aqu_sz = ssd_aqu_sz;
		local_nvm_aqu_sz = nvm_aqu_sz;
		//将等于0的值设置为1，便于后面计算（不会影响算法）
		if (local_ssd_aqu_sz == 0) {
			local_ssd_aqu_sz = 1;
		}
		if (local_nvm_aqu_sz == 0) {
			local_nvm_aqu_sz = 1;
		}
		//计算nvm平均队列长度所占比例
		ratio = 100 * local_nvm_aqu_sz / (local_ssd_aqu_sz + local_nvm_aqu_sz);
		if (ratio >= nvm_aqu_threshold) {
			nvm_debug(NVM_ERR, "use ssd,write_ratio=%d,ratio=%d,double_flag=%d", write_ratio, ratio,
					  get_double_flag(sbi->nsbi, index));
			return true;
		}
//		nvm_debug(NVM_ERR, "use nvm,write_ratio=%d,ratio=%d,double_flag=%d", write_ratio, ratio,
//				  get_double_flag(sbi->nsbi, index));

		return false;
	} else {
//		nvm_debug(NVM_ERR, "use nvm,write_ratio=%d,double_flag=%d", write_ratio, get_double_flag(sbi->nsbi, index));
		return false;
	}
}

//size是一次io中包含的块数目
static int get_size_count(int size) {
	if (size < 4)return size1;
	else if (size < 8)return size2;
	else if (size < 16)return size3;
	else return size4;
}

/**
 * submit_bio之前，进行bio重定向
 * @param sbi
 * @param bio
 * @return
 */
struct bio *nvm_redirect_bio(struct f2fs_sb_info *sbi, struct bio *bio, enum page_type type) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	int nr_nvm_main_segs;
	block_t ssd_blk_addr;
	unsigned int ssd_segno;
	block_t nvm_blk_addr;

	unsigned int ssd_main_segoff, nvm_main_segoff;//查找的段号在ssd和nvm MAIN区域的偏移量
	unsigned int offset;//块在所在seg中的偏移
	int index;
	int redirect_flag;
	int size_count;
//	unsigned int write_count = bio->bi_vcnt;
//	static int meta_read_count;
//	static int meta_write_count;
//	static int other_read_count;
//	static int other_write_count;

//    nvm_debug(NVM_INFO,"nvm_redirect_bio");
	//根据页面类型判断是否是读写META区域数据
	if (type == META || type == META_FLUSH) {
		//非第一次挂载，重定向到nvm设备中
		//如果是第一次挂载，读不重定向，写重定向；
		if (!(nsbi->nvm_flag & NVM_FIRST_MOUNR) || !is_read_io(bio_op(bio))) {
			//设置读写设备为NVM
//			if (is_read_io(bio_op(bio))) {
//				meta_read_count += bio->bi_vcnt;
//				nvm_debug(NVM_INFO, "meta_read_count:%d,all:%d", bio->bi_vcnt, meta_read_count);
//			} else {
//				meta_write_count += bio->bi_vcnt;
//				nvm_debug(NVM_INFO, "meta_write_count:%d,all:%d", bio->bi_vcnt, meta_write_count);
//			}
			bio_set_dev(bio, nsbi->nbdev);
		}
		goto out;
	}
//	if (is_read_io(bio_op(bio))) {
//		other_read_count += bio->bi_vcnt;
//		nvm_debug(NVM_INFO, "other_read_count:%d,all:%d", bio->bi_vcnt, other_read_count);
//	} else {
//		other_write_count += bio->bi_vcnt;
//		nvm_debug(NVM_INFO, "other_write_count:%d,all:%d", bio->bi_vcnt, other_write_count);
//	}
	ssd_blk_addr = SECTOR_TO_BLOCK(bio->bi_iter.bi_sector);
	ssd_segno = GET_SEGNO_FROM_SEG0(sbi, ssd_blk_addr);
	//segment在ssd MAIN区域的偏移量
	ssd_main_segoff = ssd_segno - GET_SEGNO_FROM_SEG0(sbi, sbi->raw_super->main_blkaddr);
	nr_nvm_main_segs = nsbi->nsb->main_segment_nums;

	/* ZN：
		index：segment在ssd MAIN区域的偏移量在NVM中对应偏移
		offset：段中的块偏移
	 */
	index = nr_nvm_main_segs + ssd_main_segoff;
	offset = GET_BLKOFF_FROM_SEG0(sbi, ssd_blk_addr);

	//重定向标志
	redirect_flag = get_redirect_flag(sbi->nsbi, index);
	//增加SSD段访问计数:只记录读计数，写计数没有意义
	if (is_read_io(bio_op(bio))) {
		//根据IO size增加不同的计数
		size_count = get_size_count(bio->bi_vcnt);
		atomic_add(size_count,&nsbi->lfu_count[nsbi->now_lfu_counter][ssd_main_segoff]);
//		atomic_inc(&nsbi->lfu_count[nsbi->now_lfu_counter][sbi->raw_super->segment_count_main]);
//		nvm_debug(NVM_INFO, "nvm_redirect_bio,redirect_flag=%d,ssd_main_segoff=%d,lfu_count=%d", redirect_flag,
//				  ssd_main_segoff, atomic_read(&nsbi->lfu_count[ssd_main_segoff]));
	}
//	else {
//		while (write_count--) {
//			nvm_debug(NVM_INFO, "nvm_redirect_bio,write,redirect_flag=%d,ssd_blk_no=%d#", redirect_flag,
//					  ssd_blk_addr++);
//		}
//	}

	//是否需要重定向
	if (!redirect_flag) {
		goto out;
	}

	///到此处说明有映射标志
	//对于读io，需要根据负载情况来决定是否从nvm读
//	if (is_read_io(bio_op(bio)) && use_ssd(sbi, index)) {
//		goto out;
//	}

	//segment在nvm MAIN区域的偏移量
	nvm_main_segoff = get_mpt_value(sbi->nsbi, index);
	//nvm MAIN首地址+nvm中seg偏移地址+当前seg中偏移
	nvm_blk_addr = nsbi->nsb->main_blkaddr + nvm_main_segoff * sbi->blocks_per_seg + offset;
	//设置bio读写地址
	bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK(nvm_blk_addr);
	//设置读写设备为NVM
	bio_set_dev(bio, nsbi->nbdev);

	out:
	return bio;
}

int nvm_redirect_dio(struct f2fs_sb_info *sbi, struct buffer_head *bh, unsigned int blk_nums, int write) {
	printk(KERN_INFO"ZN trap: sbi->nsbi %p",sbi->nsbi);
	struct nvm_sb_info *nsbi = sbi->nsbi;
	block_t ssd_blk_addr, nvm_blk_addr;
	unsigned int ssd_segno, ssd_main_segoff, nvm_main_segoff;//查找的段号在ssd和nvm MAIN区域的偏移量
//	unsigned int ssd_segoff_det;
	unsigned int offset;//块在所在seg中的偏移
	int index;
	int redirect_flag;// 重定向标志
	int nr_nvm_main_segs;// NVM设备MAIN区域段数目
	int size_count;

	ssd_blk_addr = bh->b_blocknr;
	nr_nvm_main_segs = nsbi->nsb->main_segment_nums;
	ssd_segno = GET_SEGNO_FROM_SEG0(sbi, ssd_blk_addr);
	//segment在ssd MAIN区域的偏移量
	ssd_main_segoff = ssd_segno - GET_SEGNO_FROM_SEG0(sbi, sbi->raw_super->main_blkaddr);
//	ssd_segoff_det = ssd_segno - GET_SEGNO_FROM_SEG0(sbi, ssd_blk_addr + blk_nums - 1);//ssd_segoff_det为0才正常，否则跨段
//	if (ssd_segoff_det < 0) {
//		nvm_debug(NVM_ERR, "directIO jump segment,ssd_segoff_det=%d", ssd_segoff_det);
//	}

	index = nr_nvm_main_segs + ssd_main_segoff;
	offset = GET_BLKOFF_FROM_SEG0(sbi, ssd_blk_addr);
//	nvm_debug(NVM_INFO, "start directIO_redirect_flag");
	//重定向标志,如果读写都不需要重定向，那么这个mpt项没有映射
	redirect_flag = get_redirect_flag(sbi->nsbi, index);
	//增加SSD段访问计数
	//TODO:写不需要计数，只对读计数即可
	if (!write) {
		//根据IO size增加不同的计数
		size_count = get_size_count(blk_nums);
		atomic_add(size_count,&nsbi->lfu_count[nsbi->now_lfu_counter][ssd_main_segoff]);
//		atomic_add(1, &nsbi->lfu_count[nsbi->now_lfu_counter][sbi->raw_super->segment_count_main]);
//		nvm_debug(NVM_INFO, "nvm_redirect_bio,redirect_flag=%d,ssd_main_segoff=%d,lfu_count=%d", redirect_flag,
//				  ssd_main_segoff, atomic_read(&nsbi->lfu_count[ssd_main_segoff]));
	}
	if (!redirect_flag) {
		goto out;
	}

	///到此处说明有映射标志
	//对于读io，需要根据负载情况来决定是否从nvm读
//	if (!write && use_ssd(sbi, index)) {
//		goto out;
//	}

	//segment在nvm MAIN区域的偏移量
	nvm_main_segoff = get_mpt_value(sbi->nsbi, index);
//	nvm_debug(NVM_DEBUG, "directIO_redirect_flag:%d, ssd_segoff=%d, ssd_segoff_det=%d,nvm_segoff=%d", redirect_flag,
//			  ssd_main_segoff,
//			  ssd_segoff_det, nvm_main_segoff);
	//nvm MAIN首地址+nvm中seg偏移地址+当前seg中偏移
	nvm_blk_addr = nsbi->nsb->main_blkaddr + nvm_main_segoff * sbi->blocks_per_seg + offset;
	//设置bh读写地址
	bh->b_blocknr = nvm_blk_addr;
	//设置bh的读写设备为NVM
	bh->b_bdev = nsbi->nbdev;

	out:
	return 0;
}

/**
 * 从nvm分配块并建立映射关系
 * @param sbi
 * @param ssd_segno
 * @return
 */
int nvm_allocate_segment(struct f2fs_sb_info *sbi, unsigned int ssd_segoff, int is_create_mapping) {

	struct nvm_sb_info *nsbi = sbi->nsbi;
	unsigned int nvm_segoff = nsbi->cur_alloc_nvm_segoff;

	/**查找并设置segment位图，新建segment*/
	spin_lock(&nsbi->segment_map_lock);
	if (nsbi->nsb->main_segment_free_nums == 0) {
		nvm_debug(NVM_ERR, "ERROR:nvm no space!");
		spin_unlock(&nsbi->segment_map_lock);
		return -ENVMNOSPACE;
	}
//    nvm_debug(1, "begin alloc nvm seg");
	nvm_assert(nsbi->nsb->main_segment_free_nums <= nsbi->nsb->main_segment_nums);

	// nvm_debug(NVM_DEBUG,"nvm free:%d",nsbi->nsb->main_segment_free_nums);
	//得到空闲位：得到的是segment号相对于MAIN区域首部的偏移
	next:
	nvm_segoff = find_next_zero_bit(nsbi->segment_map, nsbi->nsb->main_segment_nums, nvm_segoff);
	if (nvm_segoff >= nsbi->nsb->main_segment_nums) {
		nvm_assert(nsbi->nsb->main_segment_free_nums > 0);
		nvm_segoff = 0;
		goto next;
		// nvm_debug(1, "ERROR:nvm no space!");
		// spin_unlock(&nsbi->segment_map_lock);
		// return 1;//TODO:状态标志
	}
	//FIXME:崩溃时位图一致性保证
//    if (f2fs_test_bit(nvm_segoff, (char *) nsbi->ckpt_segment_map)) {
//        ++nvm_segoff;
//        goto next;
//    }
	//设置位为1，并将空闲数减一
	if (!test_and_set_bit(nvm_segoff, nsbi->segment_map)) {
		--nsbi->nsb->main_segment_free_nums;
		sbi->nsbi->nvm_flag |= NVM_NSB_DIRTY;// 设置nsb为脏
	}
	//更新当前NVM段分配的位置
	nsbi->cur_alloc_nvm_segoff = nvm_segoff + 1;
	spin_unlock(&nsbi->segment_map_lock);


	if (is_create_mapping) {
		create_mapping(sbi, ssd_segoff, nvm_segoff, rw_map | rw_redirect);
	}

	return nvm_segoff;
}

void create_mapping(struct f2fs_sb_info *sbi, unsigned int ssd_segoff, unsigned int nvm_segoff, int rw_flag) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	unsigned int entries_per_blk = F2FS_BLKSIZE / sizeof(unsigned int);//mpt区域每个块能保存多少项
	int dirty_bit[2];//表示设置的两个脏的映射项在mpt区域的页偏移
	/**建立映射关系到mpt*/
	spin_lock(&nsbi->mpt_lock);

	//建立nvm映射项，映射项建立时默认进行重定向
	set_mpt_entry(sbi->nsbi, nvm_segoff, ssd_segoff, rw_flag);
	//建立ssd映射项
	set_mpt_entry(sbi->nsbi, nsbi->nsb->main_segment_nums + ssd_segoff, nvm_segoff, rw_flag);
	/**设置脏的映射页偏移，也就是mpt_dirty_map下标*/
	dirty_bit[0] = nvm_segoff / entries_per_blk;
	test_and_set_bit(dirty_bit[0], nsbi->mpt_dirty_map);
	dirty_bit[1] = (nsbi->nsb->main_segment_nums + ssd_segoff) / entries_per_blk;
	test_and_set_bit(dirty_bit[1], nsbi->mpt_dirty_map);
	spin_unlock(&nsbi->mpt_lock);
	nvm_debug(NVM_DEBUG, "Build mapping[nvm(%d)-ssd(%d)-rem_segs(%d)]", nvm_segoff, ssd_segoff,
			  nsbi->nsb->main_segment_free_nums);
}

/**
 * 如果该SSD段存在映射，则取消映射
 * 这里的segno同义于ssd的segoff
 * @segno:ssd中要回收的段相对于ssd main区域偏移
 */
void unset_mapping(struct f2fs_sb_info *sbi, unsigned int ssd_segno) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	unsigned int ssd_offset = ssd_segno + nsbi->nsb->main_segment_nums;
	unsigned int nvm_offset;
	//mpt区域每个块能保存多少项
	unsigned int entries_per_blk = F2FS_BLKSIZE / sizeof(unsigned int);
	spin_lock(&nsbi->mpt_lock);
	if (get_map_flag(nsbi, ssd_offset)) {
//        nvm_debug(NVM_DEBUG, "unset mapping,ssd blkoff:%d", segno);
		/*取消映射关系，设置脏页信息*/
		nvm_offset = get_mpt_value(nsbi, ssd_offset);
//		atomic_set(&nsbi->lfu_count[nvm_offset], 0x0);
		clear_map(nsbi, ssd_offset);
		clear_map(nsbi, nvm_offset);
		set_bit(ssd_offset / entries_per_blk, nsbi->mpt_dirty_map);
		set_bit(nvm_offset / entries_per_blk, nsbi->mpt_dirty_map);
		//修改segment位图，增加空闲segment数目
		spin_lock(&nsbi->segment_map_lock);
		clear_bit(nvm_offset, nsbi->segment_map);
		nsbi->nsb->main_segment_free_nums++;
		spin_unlock(&nsbi->segment_map_lock);
	}
	spin_unlock(&nsbi->mpt_lock);
}


//cp结束时设置ckpt_segment_map
void nvm_set_ckpt_segment_map(struct f2fs_sb_info *sbi) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	memcpy(nsbi->ckpt_segment_map, nsbi->segment_map, (nsbi->nsb->main_segment_nums - 1) / BITS_PER_BYTE + 1);
}

/**
 * 调试函数，不直接调用，通过nvm_debug函数调用
 * @param level：调试等级
 * @param file
 * @param func
 * @param line
 * @param fmt：输出信息
 * @param ...
 */
void __nvm_debug(int level, const char *func,
				 unsigned int line, const char *fmt, ...) {
	struct va_format vaf;
	va_list args;

	if (level >= __NVM_DEBUG__)
		return;
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk(KERN_ERR "(%s, %u): %pV", func, line, &vaf);
	va_end(args);
}

int read_meta_page_from_SSD(struct f2fs_sb_info *sbi, bool is_byte_nvm) {
	struct nvm_sb_info *nsbi = is_byte_nvm? sbi->byte_nsbi : sbi->nsbi;
//	struct nvm_super_block *nsb = nsbi->nsb;

	/* 起始位置的逻辑块号，结束位置的逻辑块号
		范围：从CP区域到main区域开始
	*/
	unsigned long start = sbi->raw_super->cp_blkaddr;
	unsigned long end = sbi->raw_super->main_blkaddr;

	/* 将META区域读取到mapping中 */
	/* ZN:这块代码把META区的数据都读到内存中 */
	for (; start < end; start++) {
		struct page *page;

		/* 从磁盘读取索引块号为start的块，读到内存的page中 */
		page = f2fs_get_meta_page(sbi, start);
		//增加引用计数
//        get_page(page);
		//设置脏页，用于回写
		if (!PageDirty(page))
			set_page_dirty(page);
		f2fs_put_page(page, 1);
	}
	//取消设置nsbi->nvm_flag标志位
	nvm_assert(nsbi->nvm_flag & NVM_FIRST_MOUNR);
	nsbi->nvm_flag ^= NVM_FIRST_MOUNR;
	return 0;
}

int flush_meta_page_to_NVM(struct f2fs_sb_info *sbi) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	struct nvm_super_block *nsb = nsbi->nsb;

	/* 起始位置的逻辑块号，结束位置的逻辑块号 */
	unsigned long start = 0;
	unsigned long end = nsb->main_blkaddr;

	for (; start < end; start++) {
		struct page *page;
		page = f2fs_get_meta_page(sbi, start);
		put_page(page);  //减少引用计数

		/* 脏页写回NVM */
		f2fs_do_write_meta_page(sbi, page, FS_META_IO);
		/*或者f2fs_write_meta_pages 一次写回多个META page*/

		/* 清除脏页标志 */
		if (PageDirty(page))
			ClearPageDirty(page);
	}
	return 0;
}


/*
 * 读取 NVM 超级块结构
 * Read f2fs raw super block in NVM.
 * Because we have two copies of super block, so read both of them
 * to get the first valid one. If any one of them is broken, we pass
 * them recovery flag back to the caller.
 */
int read_nvm_super_block(struct f2fs_sb_info *sbi, int *recovery) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	int block;
	struct page *page;
	struct nvm_super_block *super;
	struct nvm_super_block *nsb_page;
	int mpt_ver_map_bytes;
	int segment_map_bytes;
	int lfu_len;
	int err = 0;

	super = kzalloc(sizeof(struct nvm_super_block), GFP_KERNEL);
	if (!super)
		return -ENOMEM;
	//TODO:此处要根据cp包中的记录来决定使用哪个nsb，需要修改
	block = sbi->ckpt->ckpt_flags & CP_NSB_VER_FLAG ? 1 : 0;
	//通过buffer_head读NVM超级块
	page = f2fs_get_meta_page(sbi, block);
	if (!page) {
		nvm_debug(NVM_ERR, "Unable to read %dth superblock in NVM", block + 1);
		err = -EIO;
	}
	nsb_page = page_address(page);
	/* sanity checking of raw super in NVM*/
	if (sanity_check_nvm_super(sbi, nsb_page)) {
		nvm_debug(NVM_ERR, "Can't find valid F2FS filesystem in %dth superblock in NVM",
				  block + 1);
		err = -EINVAL;
		/* Fail to read nvm superblock*/
		*recovery = 1;
		f2fs_put_page(page, 1);
		kfree(super);
		return err;
	}

	/* f2fs_super_block：有一个1024个字节的偏移
	   nvm_super_block:没有偏移
	*/
	memcpy(super, nsb_page, sizeof(*super));

	/* 初始化nvm_sb_info的字段 */
	if (init_nvm_sb_info(sbi, super)) {
		nvm_debug(NVM_ERR, "Can't init nvm sb info");
		err = -EINVAL;
		f2fs_put_page(page, 1);
		return err;
	}

	mpt_ver_map_bytes = (nsbi->nsb->mpt_ver_map_bits - 1) / BITS_PER_BYTE + 1;  // mpt版本位图占用字节数
	segment_map_bytes = (nsbi->nsb->main_segment_nums - 1) / BITS_PER_BYTE + 1; // segment位图占用字节数
	lfu_len = le32_to_cpu(nsbi->nsb->main_segment_nums); // NVM的LFU段访问计数列表中元素的个数
	nvm_debug(NVM_DEBUG, "read nsb from disk, blkno:%d", block);
	nvm_debug(NVM_DEBUG, "lfu_len:%d", lfu_len);

	/* 获得MPT版本位图和NVM segment位图 */
	memcpy(nsbi->mpt_ver_map, &nsb_page->map[0], mpt_ver_map_bytes);
	memcpy(nsbi->segment_map, &nsb_page->map[0] + mpt_ver_map_bytes, segment_map_bytes);
	memcpy(nsbi->ckpt_segment_map, nsbi->segment_map, segment_map_bytes);

	f2fs_put_page(page, 1);

	return err;
}

/*
 * 检查NVM超级块
 */
int sanity_check_nvm_super(struct f2fs_sb_info *sbi,
						   struct nvm_super_block *nsb) {
	struct super_block *sb = sbi->sb;

	/* 判断nsb的uuid与fsb的uuid是否一致 */
	if (strncmp(nsb->uuid, F2FS_RAW_SUPER(sbi)->uuid, sizeof(nsb->uuid))) {
		f2fs_msg(sb, KERN_ERR,
				 "uuid Mismatch, nvm(%s) - ssd(%s)",
				 (nsb->uuid), (F2FS_RAW_SUPER(sbi)->uuid));
		return 1;
	}


	return 0;
}

//依据nsb设置nsbi的相关字段
unsigned int init_nvm_sb_info(struct f2fs_sb_info *sbi, struct nvm_super_block *nsb) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	int i = 0;
	int j = 0;
	nsbi->nsb = nsb;
	nsbi->byte_private = NULL;
	/* ZN:内存中保存各种区域的位图，version_map和dirty_map应该是论文MPT里的后面两位标志 */
	/* 1、为MPT版本位图申请空间,单位：字节 */
	nsbi->mpt_ver_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(nsb->mpt_ver_map_bits), GFP_KERNEL);
	/* 2、为main区域segment位图申请空间 */
	nsbi->segment_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(nsb->main_segment_nums), GFP_KERNEL);
	nsbi->ckpt_segment_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(nsb->main_segment_nums), GFP_KERNEL);
	/* 3、为MPT表脏页位图申请空间 */
	/* MPT脏页位图大小 和 MPT版本位图大小 相同 */
	nsbi->mpt_dirty_map_bits = nsb->mpt_ver_map_bits;
	nsbi->mpt_dirty_map = f2fs_kvzalloc(sbi, f2fs_bitmap_size(nsbi->mpt_dirty_map_bits), GFP_KERNEL);

	if (!(nsbi->mpt_ver_map && nsbi->segment_map && nsbi->mpt_dirty_map)) {
		return -ENOMEM;
	}

	mutex_init(&nsbi->nvmgc_mutex);

	spin_lock_init(&nsbi->mpt_ver_map_lock);
	spin_lock_init(&nsbi->segment_map_lock);
	spin_lock_init(&nsbi->mpt_lock);
	spin_lock_init(&nsbi->lfu_half_lock);
	spin_lock_init(&nsbi->aqusz_lock);

	printk(KERN_INFO"ZN trap: mpt_entries nums = %d", nsb->mpt_entries);
	/* 为MPT cache分配内存空间，为了读取和写回的方便，分配整数page大小的MPT cache */
	nsbi->mpt = f2fs_kvzalloc(sbi, ((nsb->mpt_entries * sizeof(unsigned int) - 1) / PAGE_SIZE + 1) * PAGE_SIZE,
							  GFP_KERNEL);
	/* 4.为lfu_count cache分配内存空间，为了对NVM段进行访问计数，方便对NVM段进行垃圾回收[最后多一个空间用于记录访问总数] */
	for (i = 0; i < LFU_LEVELS; i++) {
		nsbi->lfu_count[i] = f2fs_kvzalloc(sbi,
										   (((sbi->raw_super->segment_count_main + 1) * sizeof(atomic_t) - 1) /
											PAGE_SIZE + 1) * PAGE_SIZE, GFP_KERNEL);
		//初始化值为-1
		for (j = 0; j < sbi->raw_super->segment_count_main; j++) {
			atomic_set(&nsbi->lfu_count[i][j], -1);
		}
		//初始化总计数为0
		atomic_set(&nsbi->lfu_count[i][sbi->raw_super->segment_count_main], 0);
	}
	nsbi->now_lfu_counter = 0;//初始化当前计数器为第一个计数器


	nsbi->mh = init_max_heap(sbi, NVM_TO_SSD);
	nsbi->minh = init_min_heap(sbi, SSD_TO_NVM);
	nsbi->nvm_gc_start_page = alloc_pages(GFP_KERNEL, sbi->log_blocks_per_seg);// 分配用于NVM-GC段迁移的连续物理内存页
	nsbi->ssd_to_nvm_start_page = alloc_pages(GFP_KERNEL, sbi->log_blocks_per_seg);// 分配用于NVM-GC段迁移的连续物理内存页
	nvm_assert(nsbi->nvm_gc_start_page);
	nvm_assert(nsbi->ssd_to_nvm_start_page);

	if (!nsbi->lfu_count[0] || !nsbi->mpt)
		return -ENOMEM;

	return 0;
}

//获取当前使用的有效mpt page（函数主要选定双份seg中有效的那个）
struct page *get_current_mpt_page(struct f2fs_sb_info *sbi, int mpt_pgoff) {
	struct nvm_sb_info *nsbi = sbi->nsbi;

	struct page *dst_page;//当前有效的mapping中page地址
	pgoff_t block_addr;//实际读的page对应硬盘的块号，也是META区域的偏移

	block_addr = nsbi->nsb->mpt_blkaddr + mpt_pgoff;
	///根据版本位图决定返回双份seg哪一个seg中的地址
	if (f2fs_test_bit(mpt_pgoff, nsbi->mpt_ver_map))
		block_addr += sbi->blocks_per_seg;

	//获取要写的page，这里获取实际的page内容
	dst_page = f2fs_get_meta_page(sbi, block_addr);
	return dst_page;
}


//读取MPT映射表，将mpt表保存到cache中
void f2fs_get_mpt(struct f2fs_sb_info *sbi) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	struct nvm_super_block *nsb = nsbi->nsb;
	struct page *mpt_page;
	int i;
	void *dst_addr;
	void *src_addr;

	for (i = 0; i < nsb->mpt_ver_map_bits; ++i) {
		///根据MPT版本位图得到当前有效的MPT page
		mpt_page = get_current_mpt_page(sbi, i);

		src_addr = page_address(mpt_page);
		//ZN: 除sizeof(unsigned int)，是因为nsbi->mpt的类型是unsigned int
		dst_addr = nsbi->mpt + (PAGE_SIZE * i) / sizeof(unsigned int);

		/* 将mpt page拷贝到mpt cache中 */
		memcpy(dst_addr, src_addr, PAGE_SIZE);
		//注意使用之后释放page索引！！！
		f2fs_put_page(mpt_page, 1);
	}
}

//读取LFU，将LFU访问计数数组保存到cache中
void f2fs_get_lfu(struct f2fs_sb_info *sbi) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	int i, start_blk, lfu_blk_num;
	struct page *lfu_page;
	void *dst_addr;
	void *src_addr;

	lfu_blk_num = ((sbi->raw_super->segment_count_main + 1) * sizeof(atomic_t) - 1) / PAGE_SIZE + 1;

	//TODO:此处要根据cp包中的记录来决定使用双份LFU中的哪一份
	start_blk = sbi->ckpt->ckpt_flags & CP_LFU_VER_FLAG ? nsbi->nsb->lfu_blkaddr1 : nsbi->nsb->lfu_blkaddr0;
	nvm_debug(NVM_DEBUG, "read from lfu_area: %d ", start_blk);

	for (i = 0; i < lfu_blk_num; ++i) {
		///根据MPT版本位图得到当前有效的MPT page
		lfu_page = f2fs_get_meta_page(sbi, start_blk);
		src_addr = page_address(lfu_page);
		dst_addr = nsbi->lfu_count + (PAGE_SIZE * i) / sizeof(atomic_t);

		/* 将mpt page拷贝到mpt cache中 */
		memcpy(dst_addr, src_addr, PAGE_SIZE);
		//注意使用之后释放page索引！！！
		f2fs_put_page(lfu_page, 1);
		start_blk++;
	}

	// 输出调试信息
	// for(i=0; i<nsbi->nsb->main_segment_nums; i++){
	//     nvm_debug(1, "read from disk, segoff=%d, lfu_count=%d",i,atomic_read(&nsbi->lfu_count[i]));
	// }
}

/**
 * 1.初始化大根堆
 */
struct max_heap *init_max_heap(struct f2fs_sb_info *sbi, int k) {
	struct max_heap *mh = f2fs_kvzalloc(sbi, sizeof(struct max_heap), GFP_KERNEL);
	mh->nodes = f2fs_kvzalloc(sbi, k * sizeof(struct heap_node), GFP_KERNEL);
	mh->cur_num = 0;
	mh->k = k;
	mh->max_count = 0;
	return mh;
}

/**
 * 初始化小根堆
 */
struct min_heap *init_min_heap(struct f2fs_sb_info *sbi, int k) {
	struct min_heap *minh = f2fs_kvzalloc(sbi, sizeof(struct min_heap), GFP_KERNEL);
	minh->nodes = f2fs_kvzalloc(sbi, k * sizeof(struct heap_node), GFP_KERNEL);
	minh->cur_num = 0;
	minh->k = k;
	minh->min_count = 0;
	return minh;
}

/**
 * 2.清除大根堆
 */
void clear_heap(struct max_heap *mh) {
	kvfree(mh->nodes);
	kvfree(mh);
	return;
}

/**
 * 清除小根堆
 */
void clear_min_heap(struct min_heap *mh) {
	kvfree(mh->nodes);
	kvfree(mh);
}

/**
 * 3.判断大根堆是否为空
 */
bool is_empty_heap(struct max_heap *mh) {
	if (mh->cur_num == 0) {
		return true;
	}
	return false;
}

/**
 * 4.获取大根堆堆顶的元素值（堆中的最大元素）
 */
unsigned long get_max_heap_top(struct max_heap *mh) {
	if (is_empty_heap(mh)) {
		nvm_debug(NVM_DEBUG, "top-k max-heap is empty!!");
		return 0;
	}
	return mh->nodes[0].count;
}

/**
 * 判断某个SSD段，是否为上次checkpoint操作的当前段。
 */
bool is_last_ckpt_curseg(struct f2fs_checkpoint *ckpt, int ssd_segoff) {
	int node_h, node_w, node_c, data_h, data_w, data_c;
	node_h = le32_to_cpu(ckpt->cur_node_segno[CURSEG_HOT_NODE]);
	if (ssd_segoff == node_h)
		return true;
	node_w = le32_to_cpu(ckpt->cur_node_segno[CURSEG_WARM_NODE]);
	if (ssd_segoff == node_w)
		return true;
	node_c = le32_to_cpu(ckpt->cur_node_segno[CURSEG_COLD_NODE]);
	if (ssd_segoff == node_c)
		return true;

	data_h = le32_to_cpu(ckpt->cur_data_segno[CURSEG_HOT_DATA]);
	if (ssd_segoff == data_h)
		return true;
	data_w = le32_to_cpu(ckpt->cur_data_segno[CURSEG_WARM_DATA]);
	if (ssd_segoff == data_w)
		return true;
	data_c = le32_to_cpu(ckpt->cur_data_segno[CURSEG_COLD_DATA]);
	if (ssd_segoff == data_c)
		return true;
	return false;
}

int calc_count(struct f2fs_sb_info *sbi, unsigned int ssd_segno) {
	int level[LFU_LEVELS];
	int count[LFU_LEVELS];
	int i, nums = 0;
	struct nvm_sb_info *nsbi = sbi->nsbi;
	int now = nsbi->now_lfu_counter;
	//得到LFU_LEVELS级计数值
	for (i = 0; i < LFU_LEVELS; i++) {
		count[i] = atomic_read(&nsbi->lfu_count[i][ssd_segno]);
		if (count[i] == -1) {
			count[i] = 0;
			nums++;
		}
	}
	//如果没有任何计数，说明该段没有使用，返回-1
	if (nums == LFU_LEVELS)return -1;
	//计算每一级的权重
	if (now == 0) {
		level[0] = LFU_LEVEL1;
		level[1] = LFU_LEVEL2;
		level[2] = LFU_LEVEL3;
		level[3] = LFU_LEVEL4;
	} else if (now == 3) {
		level[0] = LFU_LEVEL2;
		level[1] = LFU_LEVEL3;
		level[2] = LFU_LEVEL4;
		level[3] = LFU_LEVEL1;
	} else if (now == 2) {
		level[0] = LFU_LEVEL3;
		level[1] = LFU_LEVEL4;
		level[2] = LFU_LEVEL1;
		level[3] = LFU_LEVEL2;
	} else if (now == 1) {
		level[0] = LFU_LEVEL4;
		level[1] = LFU_LEVEL1;
		level[2] = LFU_LEVEL2;
		level[3] = LFU_LEVEL3;
	}
	nums = 0;
	for (i = 0; i < LFU_LEVELS; i++) {
		nums += count[i] * level[i];
	}
	return nums;
}

/**
 * 4.获取访问计数列表的前k个最小元素，返回TOP K的大根堆
 * return 1:不需要回收，基本不会发生
 */
int get_max_k(struct f2fs_sb_info *sbi, struct max_heap *mh, int len, bool pre_move) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	int mpt_index = 0;
	unsigned int ssd_segno;
	int count;
	unsigned int secno;
	//当前正在分配段的前500个和后50个不考虑，避免刚刚被分配的段被回收
	unsigned int now = nsbi->cur_alloc_nvm_segoff;
	unsigned int left_segoff;
	unsigned int right_segoff;
	if (now < 500) {
		left_segoff = 0;
	} else {
		left_segoff = now - 500;
	}
	right_segoff = now + 50;
//    int ssd_segoff, secno;
//    struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);


	//1.建立TOP k的大根堆
	// nvm_debug(1,">>>begin to build top-k max-heap");
	for (mpt_index = 0; mpt_index < len && mh->cur_num < mh->k; mpt_index++) {
		// if (!f2fs_test_bit(i, (char *) sbi->nsbi->ckpt_segment_map))
		//     continue;
		// ssd_segoff = get_mpt_value(sbi->nsbi, i);
		// if (is_last_ckpt_curseg(ckpt, ssd_segoff))
		//     continue;
		// secno = GET_SEC_FROM_SEG(sbi, ssd_segoff);
		// if (sec_usage_check(sbi, secno))//是当前在用的seg
		//     continue;
		/*start候选者条件*/
		//跳过刚分配的段
		if (mpt_index >= left_segoff && mpt_index <= right_segoff) {
			mpt_index = right_segoff;
			continue;
		}
		//跳过没有映射的段
		if (!get_map_flag(nsbi, mpt_index)) {
			continue;
		}
		//如果是预回收，跳过有double标志的段（因为这些已经不需要预回收）
		if (pre_move && get_double_flag(nsbi, mpt_index)) {
			continue;
		}
		ssd_segno = get_mpt_value(nsbi, mpt_index);
		//跳过当前在用的段
		secno = GET_SEC_FROM_SEG(sbi, ssd_segno);
		if (sec_usage_check(sbi, secno))
			continue;
		/*end*/

		count = calc_count(sbi, ssd_segno);
//		nvm_debug(NVM_INFO, "mpt_index=%d,count=%d", mpt_index, count);
		if (count >= 0) {//计数为0，说明写入之后就没有读过，依然需要回收
			insert_max_heap(mh, mpt_index, count);
		}
	}
	// nvm_debug(1,"top-k max-heap is initialized，size of max-heap：%d",mh->cur_num);
	//2.遍历arr列表的剩余元素，获取top k个最小元素
	for (; mpt_index < len; mpt_index++) {
		// if (!f2fs_test_bit(i, (char *) sbi->nsbi->ckpt_segment_map))
		//     continue;
		// if (is_last_ckpt_curseg(ckpt, get_mpt_value(sbi->nsbi, i)))
		//     continue;
		// ssd_segoff = get_mpt_value(sbi->nsbi, i);
		// secno = GET_SEC_FROM_SEG(sbi, ssd_segoff);
		// if (sec_usage_check(sbi, secno))//跳过当前段
		//     continue;
		/*start候选者条件*/
		//跳过刚分配的段
		if (mpt_index >= left_segoff && mpt_index <= right_segoff) {
			mpt_index = right_segoff;
			continue;
		}
		//跳过没有映射的段
		if (!get_map_flag(nsbi, mpt_index)) {
			continue;
		}
		//如果是预回收，跳过有double标志的段（因为这些已经不需要预回收）
		if (pre_move && get_double_flag(nsbi, mpt_index)) {
			continue;
		}
		ssd_segno = get_mpt_value(nsbi, mpt_index);
		//跳过当前在用的段
		secno = GET_SEC_FROM_SEG(sbi, ssd_segno);
		if (sec_usage_check(sbi, secno))
			continue;
		/*end*/

		count = calc_count(sbi, ssd_segno);
		// 剩余的元素中，如果有计数比大根堆堆顶元素小的，则加入挤占堆顶位置，并从堆顶开始自上而下调整大根堆
		if (count >= 0 && count < get_max_heap_top(mh)) {
			mh->nodes[0].count = count;
			mh->nodes[0].seg_off = mpt_index;
			max_heap_adjust_down(mh);
		}
	}
	// nvm_debug(1,">>>top-k max-heap is built");
	//3. 如果mh->cur_num < k 则说明arr列表中非0元素个数小于k个，即被访问的段的数量小于k个
	if (mh->cur_num < mh->k) {
		nvm_debug(NVM_INFO, "choose nvm segment fail: nvm segment is almost clear！");
		return -ENOCOLD;
	}
//	print_max_heap(mh, sbi);
//	nvm_debug(NVM_INFO, "gc happened,nvm ready to ssd，mh->max_count=%d", mh->max_count);
	return 0;
}

int get_min_k(struct f2fs_sb_info *sbi, struct min_heap *mh, int len) {
	int segoff = 0;
	int ret = 0;
	unsigned int secno;
	unsigned int nvm_segments = sbi->nsbi->nsb->main_segment_nums;
	int count;
	unsigned int valid_blocks;
//	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
//    int ssd_segoff, secno;
//    struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);

	//1.建立TOP k的大根堆
	// nvm_debug(1,">>>begin to build top-k max-heap");
	for (segoff = 0; segoff < len && mh->cur_num < mh->k; segoff++) {
		// if (!f2fs_test_bit(i, (char *) sbi->nsbi->ckpt_segment_map))
		//     continue;
		// ssd_segoff = get_mpt_value(sbi->nsbi, i);
		// if (is_last_ckpt_curseg(ckpt, ssd_segoff))
		//     continue;
		// secno = GET_SEC_FROM_SEG(sbi, ssd_segoff);
		// if (sec_usage_check(sbi, secno))//是当前在用的seg
		//     continue;

		//start:判断是否满足小根堆候选者条件
		if (get_map_flag(sbi->nsbi, nvm_segments + segoff)) {
			//如果有映射关系,该ssd段可以跳过
			continue;
		}
		count = calc_count(sbi, segoff);
		//改段未被使用(计数是-1)，该ssd段可以跳过
		if (count == -1) {
			continue;
		}
		//对于有效块少的段，不做热数据迁移,跳过
//		valid_blocks = get_valid_blocks(sbi, segoff, false);
//		if (valid_blocks < sbi->blocks_per_seg * UP_HOT_SEG_VALID_BLOCKS / 100) {
//			continue;
//		}
		//如果是当前在用的seg，则跳过
		secno = GET_SEC_FROM_SEG(sbi, segoff);
		if (sec_usage_check(sbi, secno))
			continue;
		//end
		if (count > 0) {
//			nvm_debug(NVM_INFO, "segoff=%d,count=%d", segoff, count);
			insert_min_heap(mh, segoff, count);
		}
	}
	// nvm_debug(1,"top-k max-heap is initialized，size of max-heap：%d",mh->cur_num);
	//2.遍历arr列表的剩余元素，获取top k个最小元素
	for (; segoff < len; segoff++) {
		// if (!f2fs_test_bit(i, (char *) sbi->nsbi->ckpt_segment_map))
		//     continue;
		// if (is_last_ckpt_curseg(ckpt, get_mpt_value(sbi->nsbi, i)))
		//     continue;
		// ssd_segoff = get_mpt_value(sbi->nsbi, i);
		// secno = GET_SEC_FROM_SEG(sbi, ssd_segoff);
		// if (sec_usage_check(sbi, secno))//跳过当前段
		//     continue;

		//start:判断是否满足小根堆候选者条件
		if (get_map_flag(sbi->nsbi, nvm_segments + segoff)) {
			//如果有映射关系,该ssd段可以跳过
			continue;
		}
		count = calc_count(sbi, segoff);
		//改段未被使用(计数是-1)，该ssd段可以跳过
		if (count == -1) {
			continue;
		}
		//对于有效块少的段，不做热数据迁移,跳过
//		valid_blocks = get_valid_blocks(sbi, segoff, false);
//		if (valid_blocks < sbi->blocks_per_seg * UP_HOT_SEG_VALID_BLOCKS / 100) {
//			continue;
//		}
		//如果是当前在用的seg，则跳过
		secno = GET_SEC_FROM_SEG(sbi, segoff);
		if (sec_usage_check(sbi, secno))
			continue;
		//end
		// 剩余的元素中，如果有计数比小根堆堆顶元素大的，则加入挤占堆顶位置，并从堆顶开始自上而下调整小根堆
		if (count > 0 && count > mh->nodes[0].count) {
			mh->nodes[0].count = count;
			mh->nodes[0].seg_off = segoff;
			min_heap_adjust_down(mh);
//			nvm_debug(NVM_INFO, "segoff=%d,count=%d", segoff, count);
		}
	}
	// nvm_debug(1,">>>top-k max-heap is built");
	//3. 如果mh->cur_num < k 则说明arr列表中非0元素个数小于k个，即被访问的段的数量小于k个
	if (mh->cur_num < mh->k) {
//		nvm_debug(NVM_ERR, "choose nvm segment fail: nvm segment is almost clear！");
		ret = -ENOHOT;
		goto stop;
	}
	stop:
	return ret;
}

/**
 * 4.向大根堆插入结点
 */
void insert_max_heap(struct max_heap *mh, int seg_off, int count) {
	int i, j;
	//1. 将新加入的元素添加到堆数组的末尾
	i = mh->cur_num;
//    mh->nodes[i].count = count;
//    mh->nodes[i].seg_off = seg_off;
	mh->cur_num++;

	// nvm_debug(1,"insert into max heap:%dth, segoff:%d, count:%d",mh->cur_num, mh->nodes[i].seg_off, mh->nodes[i].count);

	//2.自下而上，调整大根堆
	while (i > 0) {
		j = (i - 1) >> 1;// j指向i的双亲
		if (count <= mh->nodes[j].count)
			break;
		mh->nodes[i].count = mh->nodes[j].count;
		mh->nodes[i].seg_off = mh->nodes[j].seg_off;
		i = j;
	}
	mh->nodes[i].count = count;
	mh->nodes[i].seg_off = seg_off;
}

/**
 * 向小根堆插入结点
 */
void insert_min_heap(struct min_heap *mh, int seg_off, int count) {
	int i, j;
	//1. 将新加入的元素添加到堆数组的末尾
	i = mh->cur_num;
//    mh->nodes[i].count = count;
//    mh->nodes[i].seg_off = seg_off;
	mh->cur_num++;

	// nvm_debug(1,"insert into max heap:%dth, segoff:%d, count:%d",mh->cur_num, mh->nodes[i].seg_off, mh->nodes[i].count);

	//2.自下而上，调整小根堆
	while (i > 0) {
		j = (i - 1) >> 1;// j指向i的双亲
		if (count >= mh->nodes[j].count)
			break;
		mh->nodes[i].count = mh->nodes[j].count;
		mh->nodes[i].seg_off = mh->nodes[j].seg_off;
		i = j;
	}
	mh->nodes[i].count = count;
	mh->nodes[i].seg_off = seg_off;
}

/**
 * 针对大根堆，自上而下进行调整
 */
void max_heap_adjust_down(struct max_heap *mh) {
	int i = 0, j;
	int tmp_count = mh->nodes[0].count;
	int tmp_segoff = mh->nodes[0].seg_off;
	while (i < mh->cur_num) {
		j = (i + 1) * 2 - 1;// j是i的左孩子
		if (j < mh->cur_num - 1 && mh->nodes[j].count < mh->nodes[j + 1].count)
			j++;
		if (j >= mh->cur_num || tmp_count >= mh->nodes[j].count)
			break;
		mh->nodes[i].count = mh->nodes[j].count;
		mh->nodes[i].seg_off = mh->nodes[j].seg_off;
		i = j;
	}
	mh->nodes[i].count = tmp_count;
	mh->nodes[i].seg_off = tmp_segoff;
}

/**
 * 针对小根堆，自上而下进行调整
 */
void min_heap_adjust_down(struct min_heap *mh) {
	int i = 0, j;
	int tmp_count = mh->nodes[0].count;
	int tmp_segoff = mh->nodes[0].seg_off;
	while (i < mh->cur_num) {
		j = (i + 1) * 2 - 1;// j是i的左孩子
		if (j < mh->cur_num - 1 && mh->nodes[j].count > mh->nodes[j + 1].count)
			j++;
		if (j >= mh->cur_num || tmp_count <= mh->nodes[j].count)
			break;
		mh->nodes[i].count = mh->nodes[j].count;
		mh->nodes[i].seg_off = mh->nodes[j].seg_off;
		i = j;
	}
	mh->nodes[i].count = tmp_count;
	mh->nodes[i].seg_off = tmp_segoff;
}


/**
* 打印大根堆
*/
void print_max_heap(struct max_heap *mh, struct f2fs_sb_info *sbi) {
	int i;
	if (is_empty_heap(mh)) {
		nvm_debug(NVM_INFO, "top-k max-heap is empty, nothing to output");
		return;
	}
	for (i = 0; i < mh->cur_num; i++) {
		nvm_debug(NVM_INFO, "top-k max heap, ssd_segoff=%d, count=%d", get_mpt_value(sbi->nsbi, mh->nodes[i].seg_off),
				  mh->nodes[i].count);
	}
}


void nvm_destory_nsbi_modules(struct f2fs_sb_info *sbi) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	int i = 0;

	/* 释放nsbi中的位图结构 */
	kvfree(nsbi->mpt);
	kvfree(nsbi->mpt_dirty_map);
	kvfree(nsbi->ckpt_segment_map);
	kvfree(nsbi->segment_map);
	kvfree(nsbi->mpt_ver_map);
	for (i = 0; i < LFU_LEVELS; i++) {
		kvfree(nsbi->lfu_count[i]);
	}
	clear_heap(nsbi->mh);// 释放基于LFU访问计数的top-k max-heap
	clear_min_heap(nsbi->minh);
	__free_pages(nsbi->nvm_gc_start_page, sbi->log_blocks_per_seg);// 释放用于NVM-GC段迁移的连续物理内存页
	__free_pages(nsbi->ssd_to_nvm_start_page, sbi->log_blocks_per_seg);

	/* 释放nsb结构 */
	kfree(nsbi->nsb);

	/* 最后释放nsbi结构 */
	kfree(nsbi);

	sbi->nsbi = NULL;
}

//得到一个真实page的内容
void get_real_page(struct f2fs_sb_info *sbi, struct page *real_page, struct block_device *bdev, block_t blkno) {
	struct bio *read_bio;
	read_bio = f2fs_bio_alloc(sbi, 1, true);
//    f2fs_target_device(sbi, ssd_start_blkno, read_bio);//SSD转移到NVM
	bio_set_dev(read_bio, bdev);
	read_bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK(blkno);
	read_bio->bi_private = NULL;

	if (bio_add_page(read_bio, real_page, PAGE_SIZE, 0) < PAGE_SIZE) {
		f2fs_bug_on(sbi, 1);
	}
	bio_set_op_attrs(read_bio, REQ_OP_READ, 0);
	//提交读操作
	submit_bio_wait(read_bio);
	bio_put(read_bio);
}

static char *read_line(char *buf, int buf_len, struct file *fp) {
	int ret;
	int i = 0;
	mm_segment_t fs;

	fs = get_fs();
	set_fs(KERNEL_DS);
	ret = fp->f_op->read(fp, buf, buf_len, &(fp->f_pos));
	set_fs(fs);

	if (ret <= 0)
		return NULL;

	while (buf[i++] != '\n' && i < ret);

	if (i < ret) {
		fp->f_pos += i - ret;
	}

	if (i < buf_len) {
		buf[i] = 0;
	}
	return buf;
}

//存储之前的统计信息
unsigned int pre_ssd_rq_ticks, pre_nvm_rq_ticks;
unsigned int ssd_aqu_sz, nvm_aqu_sz;
unsigned int pre_rd_sec, pre_wr_sec;
unsigned int write_ratio;
unsigned int pre_tot_ticks_nvm, pre_tot_ticks_ssd;
unsigned int util_nvm, util_ssd;

//定期调用
int update_aqu_sz(void *data) {
	struct file *fp;
	char line[256], dev_name[MAX_NAME_LEN];
	unsigned int ios_pgr, tot_ticks, rq_ticks, wr_ticks;
	unsigned long rd_ios, rd_merges_or_rd_sec, rd_ticks_or_wr_sec, wr_ios;
	unsigned long wr_merges, rd_sec_or_wr_ios, wr_sec;
	unsigned long dc_ios, dc_merges, dc_sec, dc_ticks;
	unsigned int major, minor;
	unsigned int ssd_rq_ticks = 0, nvm_rq_ticks = 0;
	struct f2fs_sb_info *sbi = (struct f2fs_sb_info *) data;
	struct block_device *ssd_bdev = sbi->sb->s_bdev;
	struct block_device *nvm_bdev = sbi->nsbi->nbdev;
	//当间隔时间内没有读写数据时，认为读写各占50%
	unsigned int now_wr = 0, now_rd = 0;
	unsigned int delt = 0;
	static int util_count = 0;

	while (!kthread_should_stop()) {
		if (!spin_trylock(&sbi->nsbi->aqusz_lock)) {
			goto next;
		}
		if ((fp = filp_open(DISKSTATS, O_RDONLY, 0644)) == NULL)
			return 1;
		now_wr = 0;
		now_rd = 0;
		util_count += 1;
		util_count %= (UTIL_COLLECT_INTERVAL / LOAD_COLLECT_INTERVAL);

		while (read_line(line, sizeof(line), fp) != NULL) {

			/* major minor name rio rmerge rsect ruse wio wmerge wsect wuse running use aveq dcio dcmerge dcsect dcuse*/
			sscanf(line, "%u %u %s %lu %lu %lu %lu %lu %lu %lu %u %u %u %u %lu %lu %lu %lu",
				   &major, &minor, dev_name,
				   &rd_ios, &rd_merges_or_rd_sec, &rd_sec_or_wr_ios, &rd_ticks_or_wr_sec,
				   &wr_ios, &wr_merges, &wr_sec, &wr_ticks, &ios_pgr, &tot_ticks, &rq_ticks,
				   &dc_ios, &dc_merges, &dc_sec, &dc_ticks);

			if (strcmp(ssd_bdev->bd_disk->disk_name, dev_name) == 0) {
				ssd_rq_ticks = rq_ticks;
				now_wr += wr_sec;
				now_rd += rd_sec_or_wr_ios;

				//IO饱和率统计
				if (util_count == 0) {
					util_ssd = 100 * (tot_ticks - pre_tot_ticks_ssd) / UTIL_COLLECT_INTERVAL;
					pre_tot_ticks_ssd = tot_ticks;
				}


			} else if (strcmp(nvm_bdev->bd_disk->disk_name, dev_name) == 0) {
				nvm_rq_ticks = rq_ticks;
				now_wr += wr_sec;
				now_rd += rd_sec_or_wr_ios;

				//IO饱和率统计
				if (util_count == 0) {
					util_nvm = 100 * (tot_ticks - pre_tot_ticks_nvm) / UTIL_COLLECT_INTERVAL;
					pre_tot_ticks_nvm = tot_ticks;
				}

			}

		}
		filp_close(fp, NULL);
		//更新写比例
		delt = now_wr + now_rd - pre_rd_sec - pre_wr_sec;
		if (delt == 0) {
			//此段时间没读没写，则将write_ratio设为50%
			write_ratio = 50;
		} else {
			write_ratio = 100 * (now_wr - pre_wr_sec) / delt;
		}
		pre_wr_sec = now_wr;
		pre_rd_sec = now_rd;
//		if (util_count == 0) {
//			nvm_debug(NVM_INFO, "util_nvm=%d,util_ssd=%d", util_nvm, util_ssd);
//		}
		//更新aqu_sz
		ssd_aqu_sz = (ssd_rq_ticks - pre_ssd_rq_ticks);
		nvm_aqu_sz = (nvm_rq_ticks - pre_nvm_rq_ticks);
		if (ssd_aqu_sz != 0)
			pre_ssd_rq_ticks = ssd_rq_ticks;
		if (nvm_aqu_sz != 0)
			pre_nvm_rq_ticks = nvm_rq_ticks;
		spin_unlock(&sbi->nsbi->aqusz_lock);
		next:
		SLEEP_MILLI_SEC(LOAD_COLLECT_INTERVAL)
	}

	return 0;
}

//即时调用
int update_aqu_sz_now(void *data) {
	struct file *fp;
	char line[256], dev_name[MAX_NAME_LEN];
	unsigned int ios_pgr, tot_ticks, rq_ticks, wr_ticks;
	unsigned long rd_ios, rd_merges_or_rd_sec, rd_ticks_or_wr_sec, wr_ios;
	unsigned long wr_merges, rd_sec_or_wr_ios, wr_sec;
	unsigned long dc_ios, dc_merges, dc_sec, dc_ticks;
	unsigned int major, minor;
	unsigned int ssd_rq_ticks = 0, nvm_rq_ticks = 0;
	struct f2fs_sb_info *sbi = (struct f2fs_sb_info *) data;
	struct block_device *ssd_bdev = sbi->sb->s_bdev;
	struct block_device *nvm_bdev = sbi->nsbi->nbdev;

	if ((fp = filp_open(DISKSTATS, O_RDONLY, 0644)) == NULL)
		return 1;

	while (read_line(line, sizeof(line), fp) != NULL) {

		/* major minor name rio rmerge rsect ruse wio wmerge wsect wuse running use aveq dcio dcmerge dcsect dcuse*/
		sscanf(line, "%u %u %s %lu %lu %lu %lu %lu %lu %lu %u %u %u %u %lu %lu %lu %lu",
			   &major, &minor, dev_name,
			   &rd_ios, &rd_merges_or_rd_sec, &rd_sec_or_wr_ios, &rd_ticks_or_wr_sec,
			   &wr_ios, &wr_merges, &wr_sec, &wr_ticks, &ios_pgr, &tot_ticks, &rq_ticks,
			   &dc_ios, &dc_merges, &dc_sec, &dc_ticks);

		if (strcmp(ssd_bdev->bd_disk->disk_name, dev_name) == 0) {
			ssd_rq_ticks = rq_ticks;

		} else if (strcmp(nvm_bdev->bd_disk->disk_name, dev_name) == 0) {
			nvm_rq_ticks = rq_ticks;
		}

	}
	filp_close(fp, NULL);

	//更新aqu_sz
	ssd_aqu_sz = (ssd_rq_ticks - pre_ssd_rq_ticks);
	nvm_aqu_sz = (nvm_rq_ticks - pre_nvm_rq_ticks);
	if (ssd_aqu_sz != 0)
		pre_ssd_rq_ticks = ssd_rq_ticks;
	if (nvm_aqu_sz != 0)
		pre_nvm_rq_ticks = nvm_rq_ticks;

	return 0;
}

/**
 * 定时更新aqu_sz
 */
int init_aqu_sz_thread(struct f2fs_sb_info *sbi) {
	int err;
	struct task_struct *aqu_sz_task;
	aqu_sz_task = kthread_create(update_aqu_sz, sbi, "aqu_sz_task");
	if (IS_ERR(aqu_sz_task)) {
		nvm_debug(NVM_ERR, "Unable to start kernel thread.");
		err = PTR_ERR(aqu_sz_task);
		return err;
	}
	sbi->nsbi->aqu_sz_task = aqu_sz_task;
	wake_up_process(aqu_sz_task);
	return 0;
}

bool nvm_alloc(struct f2fs_sb_info *sbi, int type) {
	bool alloc;
	__le32 all = sbi->nsbi->nsb->main_segment_nums;
	__le32 used = all - sbi->nsbi->nsb->main_segment_free_nums;
	unsigned int ratio = 0;
	unsigned int real_low_boundary = 0, real_high_boundary = 0;
	unsigned int local_ssd_aqu_sz = 0;
	unsigned int local_nvm_aqu_sz = 0;

	//优先考虑根据冷热数据分配
	if (type == CURSEG_HOT_DATA || type == CURSEG_HOT_NODE) {
		return true;
	}
	if (spin_trylock(&sbi->nsbi->aqusz_lock)) {
		update_aqu_sz_now(sbi);
		local_ssd_aqu_sz = ssd_aqu_sz;
		local_nvm_aqu_sz = nvm_aqu_sz;
		spin_unlock(&sbi->nsbi->aqusz_lock);
	} else {
		local_ssd_aqu_sz = ssd_aqu_sz;
		local_nvm_aqu_sz = nvm_aqu_sz;
	}

	//将等于0的值设置为1，便于后面计算（不会影响算法）
	if (local_ssd_aqu_sz == 0) {
		local_ssd_aqu_sz = 1;
	}
	if (local_nvm_aqu_sz == 0) {
		local_nvm_aqu_sz = 1;
	}
	//计算nvm平均队列长度所占比例
	ratio = 100 * local_nvm_aqu_sz / (local_ssd_aqu_sz + local_nvm_aqu_sz);
	//计算nvm剩余空间加权值，降低边界高度：当nvm剩余空间不足时候，应降低分配的速度
	if (2 * used > all) {//使用率超过一半才考虑剩余空间对分配的影响
		real_low_boundary = low_boundary - space_weight * (2 * used - all) / all;
		real_high_boundary = high_boundary - space_weight * (2 * used - all) / all;
	} else {
		real_low_boundary = low_boundary;
		real_high_boundary = high_boundary;
	}

	if (ratio < real_low_boundary) {
		//nvm负载低，分配到nvm
		alloc = true;
	} else {
		alloc = false;
	}
//	nvm_debug(NVM_ERR, "alloc=%d,ratio=%d,ssd_aqu_sz=%d,nvm_aqu_sz=%d,nvm_used_ratio=%d", alloc, ratio, ssd_aqu_sz,
//			  nvm_aqu_sz, 100 * used / all);
	return alloc;
}
