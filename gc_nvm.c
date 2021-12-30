/*
 * fs/f2fs/gc.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>


#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "gc.h"
#include "nvm.h"

/**
 * 在原有的gc函数中，segno与segoff同义
 * 沿用此定义，本文件中所有segno均与segoff同义
 */

static int cold_to_ssd_thread(void *data) {
	struct f2fs_sb_info *sbi = data;
	struct f2fs_gc_kthread *gc_th = sbi->nsbi->gc_thread;
	wait_queue_head_t *wq = &sbi->nsbi->gc_thread->gc_wait_queue_head;
	unsigned int wait_ms;

	wait_ms = gc_th->min_sleep_time;

	set_freezable();
	do {
		wait_event_interruptible_timeout(*wq,
										 kthread_should_stop() || freezing(current) ||
										 gc_th->gc_wake,
										 msecs_to_jiffies(wait_ms));

		/* give it a try one time */
		if (gc_th->gc_wake)
			gc_th->gc_wake = 0;

		if (try_to_freeze())
			continue;
		if (kthread_should_stop())
			break;

		if (sbi->sb->s_writers.frozen >= SB_FREEZE_WRITE) {
			increase_sleep_time(gc_th, &wait_ms);
			continue;
		}

#ifdef CONFIG_F2FS_FAULT_INJECTION
		if (time_to_inject(sbi, FAULT_CHECKPOINT)) {
			f2fs_show_injection_info(FAULT_CHECKPOINT);
			f2fs_stop_checkpoint(sbi, false);
		}
#endif

		if (!sb_start_write_trylock(sbi->sb))
			continue;

		if (!mutex_trylock(&sbi->gc_mutex))
			goto next;

		if (!has_not_enough_free_nvm_secs(sbi->nsbi)) {
			goto next;
		}
		//TODO:nvm gc
		cold_to_ssd(sbi);//test_opt与mount有关，(sbi->mount_opt).opt &0x00004000

		/* balancing f2fs's metadata periodically */
		f2fs_balance_fs_bg(sbi);
		next:
		mutex_unlock(&sbi->gc_mutex);
		sb_end_write(sbi->sb);
	} while (!kthread_should_stop());
	return 0;
}


/*
static int get_nvm_victim(struct f2fs_sb_info *sbi, unsigned int *result)//用result返回NVM的受害者的segno
{
    struct nvm_sb_info *nsbi = sbi->nsbi;
    unsigned int secno;
    unsigned int start_segno = 0;//NVM和SSD的segno都是从main区域的第一个段开始，同segoff
//    unsigned int last_segno = sbi->nsbi->nsb->main_segment_nums;
//    unsigned int nsearched = sbi->nsbi->nsb->main_segment_nums;//每次查找的范围，后期可能会改这个范围
    unsigned int segno = start_segno;//段号查找段空闲位图得到
    unsigned int ssd_segno;
    unsigned long changetime;

    if (*result != NULL_SEGNO && get_redirect_flag(sbi->nsbi, *result)) {
        ssd_segno = get_mpt_value(sbi->nsbi, *result);
        if (!sec_usage_check(sbi, GET_SEC_FROM_SEG(sbi, ssd_segno)))//不是当前在用的seg
            goto out;
    }

    changetime = ((unsigned long) (~0));

    //因为此处只读取位图状态，不需要加锁
    segno = find_next_bit(nsbi->segment_map, nsbi->nsb->main_segment_nums, segno);
    while (segno != nsbi->nsb->main_segment_nums) {

        ssd_segno = get_mpt_value(sbi->nsbi, segno);
        secno = GET_SEC_FROM_SEG(sbi, ssd_segno);
        if (sec_usage_check(sbi, secno))//是当前在用的seg
            continue;

        if (changetime > get_seg_entry(sbi, ssd_segno)->mtime) {//找其中的修改最早的段
            changetime = get_seg_entry(sbi, ssd_segno)->mtime;
            *result = segno;
        }

        segno = find_next_bit(nsbi->segment_map, nsbi->nsb->main_segment_nums, ++segno);
    }

    out:
    if (*result == NULL_SEGNO)
        nvm_debug(NVM_DEBUG, "get nvm victim(segno is %d)", *result);
    else
        nvm_debug(NVM_DEBUG, "get nvm victim(result is %d)", *result);
    return 1;//不存在找不到的情况，此函数一定会成功
}
*/

/**
 * 设置双重标志
 */
static void set_double(struct f2fs_sb_info *sbi, unsigned int ssd_segno) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	unsigned int ssd_offset = ssd_segno + nsbi->nsb->main_segment_nums;
	unsigned int nvm_offset;
	//mpt区域每个块能保存多少项
	unsigned int entries_per_blk = F2FS_BLKSIZE / sizeof(unsigned int);
	spin_lock(&nsbi->mpt_lock);
	if (get_map_flag(nsbi, ssd_offset)) {
		nvm_offset = get_mpt_value(nsbi, ssd_offset);
		//设置double标志
		set_double_flag(nsbi, ssd_offset);
		set_double_flag(nsbi, nvm_offset);
		//设置对应mpt项为脏
		set_bit(ssd_offset / entries_per_blk, nsbi->mpt_dirty_map);
		set_bit(nvm_offset / entries_per_blk, nsbi->mpt_dirty_map);
	}
	spin_unlock(&nsbi->mpt_lock);
}

//nvm_segoff是段在NVM中相对于main区域的偏移
static void do_cold_to_ssd(struct f2fs_sb_info *sbi, unsigned int nvm_segoff, bool pre_move) {
	block_t ssd_start_blkno, nvm_start_blkno;
	struct page *read_start_page, *write_start_page;
	struct bio *read_bio;
	struct bio *write_bio;
	enum page_type type;
	unsigned int ssd_segoff;
	unsigned int pos = 0, st_pos = 0;
	struct seg_entry *se;
	unsigned char *cur_map;
	unsigned int blk_per_seg = sbi->blocks_per_seg;
	int i;//循环控制
//	int count = 0;
//	struct sit_info *sit_i = SIT_I(sbi);

	//ssd块偏移
	ssd_segoff = get_mpt_value(sbi->nsbi, nvm_segoff);
	se = get_seg_entry(sbi, ssd_segoff);
	type = IS_DATASEG(se->type) ? DATA : NODE;
	//有效块位图
	cur_map = se->cur_valid_map;
//	ckpt_map = se->ckpt_valid_map;

	if (!get_map_flag(sbi->nsbi, nvm_segoff)) {
		//此NVM段没有建立映射，无需回收
		return;
	}

	///开始进行段内有效块回收

//	if (se->valid_blocks < 512 || se->ckpt_valid_blocks < 512)
//		nvm_debug(NVM_INFO, "--start ssdhot_to_nvm,valid_count=%d,ckpt_vdconut=%d,ssd_segoff=%d,nvm_segoff=%d",
//				  se->valid_blocks, se->ckpt_valid_blocks, ssd_segoff, nvm_segoff);
	next:
	//获取连续段起始地址
//	tmp = find_next_bit(ckpt_map, blk_per_seg, pos);
//	pos = find_next_bit_le(cur_map, blk_per_seg, pos);
//	if (pos > tmp) {
//		pos = tmp;
//	}
	while (pos < blk_per_seg && !f2fs_test_bit(pos, cur_map)) {
		pos++;
	}
	if (pos >= blk_per_seg) {
		//没有有效块了，结束
		goto stop;
	}
	st_pos = pos;
	//设置nvm和ssd起始块地址
	nvm_start_blkno = sbi->nsbi->nsb->main_blkaddr + nvm_segoff * sbi->blocks_per_seg + st_pos;
	ssd_start_blkno = START_BLOCK(sbi, ssd_segoff) + st_pos;//ssd中对应地址
	//获取连续段结束地址
	pos++;
//	tmp = find_next_zero_bit(ckpt_map, blk_per_seg, pos);
//	pos = find_next_zero_bit_le(cur_map, blk_per_seg, pos);
//	if (pos < tmp) {
//		pos = tmp;
//	}
	while (pos < blk_per_seg && f2fs_test_bit(pos, cur_map)) {
		pos++;
	}

	//获取读取开始内存页
	read_start_page = sbi->nsbi->nvm_gc_start_page;

	///从nvm读数据
	read_bio = f2fs_bio_alloc(sbi, BIO_MAX_PAGES, true);
//    f2fs_target_device(sbi, ssd_start_blkno, read_bio);//SSD转移到NVM
	bio_set_dev(read_bio, sbi->nsbi->nbdev);
	read_bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK(nvm_start_blkno);
	read_bio->bi_private = NULL;

	nvm_debug(NVM_DEBUG, "bio,nvm blkno:%d,ssd blkno:%d", nvm_start_blkno, ssd_start_blkno);

	for (i = 0; i < pos - st_pos; i++) {
		if (bio_add_page(read_bio, read_start_page, PAGE_SIZE, 0) < PAGE_SIZE)
			f2fs_bug_on(sbi, 1);
//		if (se->valid_blocks < 512 || se->ckpt_valid_blocks < 512) {
//			nvm_debug(NVM_INFO, "cold move,ssd_segoff=%d,ssd_blk_no=%d#", ssd_segoff, ssd_start_blkno + i);
//		}
//		count++;
		read_start_page++;
	}
//	if (se->valid_blocks < 512 || se->ckpt_valid_blocks < 512) {
//		nvm_debug(NVM_INFO, "cold move,---");
//	}
	bio_set_op_attrs(read_bio, REQ_OP_READ, 0);
	//提交读操作
	//nvm_debug(NVM_DEBUG,"nvm_submit_bio");
//    nvm_submit_bio(sbi, read_bio, type);
	submit_bio_wait(read_bio);
	bio_put(read_bio);

	///写数据到ssd
	write_start_page = sbi->nsbi->nvm_gc_start_page;
	write_bio = f2fs_bio_alloc(sbi, BIO_MAX_PAGES, true);
	f2fs_target_device(sbi, ssd_start_blkno, write_bio);//这一次不会转移到NVM
	write_bio->bi_write_hint = f2fs_io_type_to_rw_hint(sbi, type, HOT);
	for (i = 0; i < pos - st_pos; i++) {
//        lock_page(now_page);
//        set_page_dirty(now_page);
		if (bio_add_page(write_bio, write_start_page, PAGE_SIZE, 0) < PAGE_SIZE)
			f2fs_bug_on(sbi, 1);
		write_start_page++;
	}
	bio_set_op_attrs(write_bio, REQ_OP_WRITE, REQ_SYNC);//REQ_SYNC或者REQ_BACKGROUND，目前都用REQ_SYNC
	//nvm_submit_bio(sbi, write_bio, type);
	//提交ssd写
	submit_bio_wait(write_bio);
//    nvm_submit_bio(sbi, write_bio, type);
	bio_put(write_bio);

	//处理下一连续段
	pos++;
	goto next;

	// __free_pages(tmp_page, sbi->log_blocks_per_seg);//释放内存页
	//取消映射关系，设置脏页信息。
	nvm_debug(NVM_DEBUG, "nvm gc,unset_mapping,nvm segoff:%d,ssd segoff:%d,free segs:%d", nvm_segoff, ssd_segoff,
			  sbi->nsbi->nsb->main_segment_free_nums);
	stop:
//	if (se->valid_blocks < 512 || se->ckpt_valid_blocks < 512)
//		nvm_debug(NVM_INFO, "--end ssdhot_to_nvm,valid_count=%d,ckpt_vdcount=%d,move_count=%d", se->valid_blocks,
//				  se->ckpt_valid_blocks, count);
	//如果是预迁移，直接设置为double标志即可；若真回收，则取消映射关系回收段
	if (pre_move) {
		set_double(sbi, ssd_segoff);
	} else {
		unset_mapping(sbi, ssd_segoff);
	}
}

//nvm_segoff是段在NVM中相对于main区域的偏移
static void do_hot_to_nvm(struct f2fs_sb_info *sbi, unsigned int ssd_segoff, unsigned int nvm_segoff) {
	block_t ssd_start_blkno, nvm_start_blkno;
	struct page *read_start_page, *write_start_page;
	struct bio *read_bio;
	struct bio *write_bio;
	enum page_type type;
	unsigned int pos = 0, st_pos = 0;
	struct seg_entry *se;

	unsigned char *cur_map;
	unsigned int blk_per_seg = sbi->blocks_per_seg;
	int i;//循环控制
//	struct sit_info *sit_i = SIT_I(sbi);
	se = get_seg_entry(sbi, ssd_segoff);
	type = IS_DATASEG(se->type) ? DATA : NODE;
	//有效块位图
	cur_map = se->cur_valid_map;
//	ckpt_map = (unsigned long *) se->ckpt_valid_map;

	///开始进行段内有效块回收
//	if (se->valid_blocks < 512 || se->ckpt_valid_blocks < 512)
//		nvm_debug(NVM_INFO, "--start ssdhot_to_nvm,valid_count=%d,ckpt_vdconut=%d,ssd_segoff=%d,nvm_segoff=%d",
//				  se->valid_blocks, se->ckpt_valid_blocks, ssd_segoff, nvm_segoff);
	next:
	//获取连续段起始地址
//	tmp = find_next_bit(ckpt_map, blk_per_seg, pos);
//	pos = find_next_bit_le(cur_map, blk_per_seg, pos);
//	if (pos > tmp) {
//		pos = tmp;
//	}
	while (pos < blk_per_seg && !f2fs_test_bit(pos, cur_map)) {
		pos++;
	}
	if (pos >= blk_per_seg) {
		//没有有效块了，结束
		goto stop;
	}
	st_pos = pos;
	//设置nvm和ssd起始块地址
	nvm_start_blkno = sbi->nsbi->nsb->main_blkaddr + nvm_segoff * sbi->blocks_per_seg + st_pos;
	ssd_start_blkno = START_BLOCK(sbi, ssd_segoff) + st_pos;//ssd中对应地址
	//获取连续段结束地址
	pos++;
//	tmp = find_next_zero_bit(ckpt_map, blk_per_seg, pos);
//	pos = find_next_zero_bit_le(cur_map, blk_per_seg, pos);
//	if (pos < tmp) {
//		pos = tmp;
//	}
	while (pos < blk_per_seg && f2fs_test_bit(pos, cur_map)) {
		pos++;
	}

	//获取读取开始内存页
	read_start_page = sbi->nsbi->ssd_to_nvm_start_page;

	///从ssd读数据
	read_bio = f2fs_bio_alloc(sbi, BIO_MAX_PAGES, true);
//    f2fs_target_device(sbi, ssd_start_blkno, read_bio);//SSD转移到NVM
	bio_set_dev(read_bio, sbi->sb->s_bdev);
	read_bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK(ssd_start_blkno);
	read_bio->bi_private = NULL;

//	nvm_debug(NVM_DEBUG, "bio,nvm blkno:%d,ssd blkno:%d", nvm_start_blkno, ssd_start_blkno);

	for (i = 0; i < pos - st_pos; i++) {
		if (bio_add_page(read_bio, read_start_page, PAGE_SIZE, 0) < PAGE_SIZE)
			f2fs_bug_on(sbi, 1);
		read_start_page++;
//		if (se->valid_blocks < 512 || se->ckpt_valid_blocks < 512) {
//			nvm_debug(NVM_INFO, "move,ssd_segoff=%d,ssd_blk_no=%d#", ssd_segoff, ssd_start_blkno + i);
//		}
//		count++;
	}
	bio_set_op_attrs(read_bio, REQ_OP_READ, 0);
	//提交读操作
	//nvm_debug(NVM_DEBUG,"nvm_submit_bio");
//    nvm_submit_bio(sbi, read_bio, type);
	submit_bio_wait(read_bio);
	bio_put(read_bio);

	///写数据到nvm
	write_start_page = sbi->nsbi->ssd_to_nvm_start_page;
	write_bio = f2fs_bio_alloc(sbi, BIO_MAX_PAGES, true);
	bio_set_dev(write_bio, sbi->nsbi->nbdev);
	write_bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK(nvm_start_blkno);
	write_bio->bi_write_hint = f2fs_io_type_to_rw_hint(sbi, type, HOT);
	for (i = 0; i < pos - st_pos; i++) {
//        lock_page(now_page);
//        set_page_dirty(now_page);
		if (bio_add_page(write_bio, write_start_page, PAGE_SIZE, 0) < PAGE_SIZE)
			f2fs_bug_on(sbi, 1);
		write_start_page++;
	}
	bio_set_op_attrs(write_bio, REQ_OP_WRITE, REQ_SYNC);//REQ_SYNC或者REQ_BACKGROUND，目前都用REQ_SYNC
	//nvm_submit_bio(sbi, write_bio, type);
	//提交nvm写
	submit_bio_wait(write_bio);
//    nvm_submit_bio(sbi, write_bio, type);
	bio_put(write_bio);

	//处理下一连续段
	pos++;
	goto next;
	stop:
//	if (se->valid_blocks < 512 || se->ckpt_valid_blocks < 512)
//		nvm_debug(NVM_INFO, "--end ssdhot_to_nvm,valid_count=%d,ckpt_vdcount=%d,move_count=%d", se->valid_blocks,
//				  se->ckpt_valid_blocks, count);
	return;
}

static int cold_to_ssd_collect(struct f2fs_sb_info *sbi, unsigned int start_segno) {
	struct blk_plug plug;
	unsigned int segno = start_segno;//nvm段号
	unsigned int end_segno = start_segno + sbi->segs_per_sec;
	int seg_freed = 0;

	blk_start_plug(&plug);

	for (segno = start_segno; segno < end_segno; segno++) {
		do_cold_to_ssd(sbi, segno, false);
		seg_freed++;
	}

	blk_finish_plug(&plug);

	return seg_freed;
}

//真正回收段到ssd，释放nvm设备空间
int cold_to_ssd(struct f2fs_sb_info *sbi) {
	int seg_freed = 0, seg_freed_gc = 0, seg_freed_double = 0, total_freed = 0, secno, ssd_segno;
	int i, ret = 0;
	int nvm_main_seg_num = sbi->nsbi->nsb->main_segment_nums;
	struct max_heap *mh = sbi->nsbi->mh;
	struct cp_control cpc;
	unsigned int valid_blocks;
	unsigned int cmp = sbi->blocks_per_seg * SEG_VALID_BLOCKS / 100;
	unsigned int nvm_segoff;
	int gc_type = FG_GC;


	gc_more:
	if (has_not_enough_free_nvm_secs(sbi->nsbi)) {
/*
		 * For example, if there are many prefree_segments below given
		 * threshold, we can make them free by checkpoint. Then, we
		 * secure free segments which doesn't need fggc any more.
		 */
		///NOTE：释放空闲段机制不能去除，否则对性能影响很大！！！
		if (prefree_segments(sbi)) {
			cpc.reason = __get_cp_reason(sbi);
			ret = f2fs_write_checkpoint(sbi, &cpc);
			if (ret)
				goto stop;
		}
		if (!has_not_enough_free_nvm_secs(sbi->nsbi))
			goto stop;
	}

	///TODO: 选取NVM受害者段，通过LFU 大根堆选择访问计数最少的前k个段
	/* start */
	// struct max_heap *mh = init_max_heap(sbi, NVM_LFU_FREE);
	reset_max_heap(mh);// 将当前大根堆内当前节点数置0
	if (get_max_k(sbi, mh, nvm_main_seg_num, false) || mh->cur_num == 0) {
//		nvm_debug(NVM_ERR, "choose nvm segment fail: nvm segment is almost clear！");
		goto stop;
	}
	//记录本次回收段中最热的一个
	mh->max_count = mh->nodes[0].count;

	nvm_debug(NVM_ERR, "gc start");
	nvm_debug(NVM_INFO, "dump_stack");
//	dump_stack();
	//3. 遍历大根堆，针对每个选中的NVM段进行GC操作
	for (i = 0; i < mh->cur_num; i++) {
		nvm_segoff = mh->nodes[i].seg_off;
		ssd_segno = get_mpt_value(sbi->nsbi, nvm_segoff);
//		nvm_debug(NVM_DEBUG, "%dth element in top-k max-heap, nvm_segoff=%d, ssd_segoff=%d, lfu_count=%d", i, segno,
//				  ssd_segno,
//				  mh->nodes[i].count);

		secno = GET_SEC_FROM_SEG(sbi, ssd_segno);
		if (sec_usage_check(sbi, secno))//是当前在用的seg
			continue;
		//如果是双重数据，直接取消映射关系进行回收
		if (get_double_flag(sbi->nsbi, nvm_segoff)) {
			seg_freed_double++;
			unset_mapping(sbi, get_mpt_value(sbi->nsbi, nvm_segoff));
			continue;
		}
		//对于有效块少的段，直接使用F2FS原有的GC机制回收
		valid_blocks = get_valid_blocks(sbi, ssd_segno, false);
		if (valid_blocks == 0)continue;//有效块为0的不做GC，等待ckpt回收
		if (valid_blocks < cmp) {
			struct gc_inode_list gc_list = {
					.ilist = LIST_HEAD_INIT(gc_list.ilist),
					.iroot = RADIX_TREE_INIT(gc_list.iroot, GFP_NOFS),
			};
//			nvm_debug(NVM_DEBUG, "less valid blocks=(%d),nvm_segoff=(%d),ssd_segoff=(%d)", valid_blocks, segno,
//					  ssd_segno);
//			nvm_debug(NVM_ERR, "ssd gc,ssd_segno=%d,cur_thread=%d#", ssd_segno, current->pid);
			mutex_lock(&sbi->gc_mutex);
			seg_freed_gc += do_garbage_collect(sbi, ssd_segno, &gc_list, gc_type);
//            nvm_assert(seg_freed != 0);
			mutex_unlock(&sbi->gc_mutex);
		} else {
//			nvm_debug(NVM_ERR, "gc,cold to ssd,ssd_segno=%d,cur_thread=%d#", ssd_segno, current->pid);
//			mutex_lock(&sbi->nsbi->nvm_move_lock);
			do_cold_to_ssd(sbi, nvm_segoff, false);
			seg_freed++;
//			mutex_unlock(&sbi->nsbi->nvm_move_lock);
		}
	}

	if (has_not_enough_free_nvm_secs(sbi->nsbi)) {
//			segno = NULL_SEGNO;
		goto gc_more;
	}

	total_freed = seg_freed + seg_freed_gc + seg_freed_double;

//	if (sync)
//		ret = sec_freed ? 0 : -EAGAIN;

	stop:
	nvm_debug(NVM_ERR,
			  "gc,cold to ssd,total_freed=%d,seg_freed=%d,seg_freed_gc=%d,seg_freed_double=%d,mh->max_count=%d",
			  total_freed, seg_freed, seg_freed_gc, seg_freed_double, mh->max_count);
	return total_freed;
}

//预回收段到ssd，仅仅迁移数据并添加double标志
int precold_to_ssd(struct f2fs_sb_info *sbi) {
	int seg_freed = 0, secno, ssd_segno;
	int i;
	int nvm_main_seg_num = sbi->nsbi->nsb->main_segment_nums;
	struct max_heap *mh = sbi->nsbi->mh;
	unsigned int segno;

	///TODO: 选取NVM受害者段，通过LFU 大根堆选择访问计数最少的前k个段
	/* start */
	// struct max_heap *mh = init_max_heap(sbi, NVM_LFU_FREE);
	reset_max_heap(mh);// 将当前大根堆内当前节点数置0
	if (get_max_k(sbi, mh, nvm_main_seg_num, true) || mh->cur_num == 0) {
//		nvm_debug(NVM_ERR, "choose nvm segment fail: nvm segment is almost clear！");
		goto stop;
	}

	//预回收需要判断是否最热的块1.5倍低于前一次热迁移最冷的块，看是否值得回收
	if (mh->nodes[0].count * hot_multiple_fenzi / hot_multiple_fenmu >= sbi->nsbi->minh->min_count) {
		goto stop;
	}

	//遍历大根堆，针对每个选中的NVM段进行GC操作
	for (i = 0; i < mh->cur_num; i++) {
		segno = mh->nodes[i].seg_off;
		ssd_segno = get_mpt_value(sbi->nsbi, segno);

		//跳过当前在用的seg
		secno = GET_SEC_FROM_SEG(sbi, ssd_segno);
		if (sec_usage_check(sbi, secno))
			continue;
		do_cold_to_ssd(sbi, segno, true);
		seg_freed++;
	}

	nvm_debug(NVM_INFO, "precold,total_move=%d", seg_freed);
	stop:
	return seg_freed;
}


//是否达到迁移条件
static bool can_move(struct nvm_sb_info *nsbi) {
	return nsbi->minh->min_count > MIN_HOT
		   && nsbi->minh->min_count > nsbi->mh->max_count;
}

//将ssd热数据转移到nvm
int hot_to_nvm(struct f2fs_sb_info *sbi) {
	int i, ret = 0;
	unsigned int ssd_segoff, nvm_segoff, secno;
	struct min_heap *minh = sbi->nsbi->minh;
	unsigned int ssd_main_seg_num = sbi->raw_super->segment_count_main;
	struct free_segmap_info *free_i = FREE_I(sbi);
	struct nvm_sb_info *nsbi = sbi->nsbi;
//	unsigned int valid_blocks;
//	unsigned int cmp;

	//先判断空闲空间比例，空闲空间比例高于阈值(有足够空闲段)才进行up hot操作,否则直接退出
	if (has_not_enough_free_nvm_secs(sbi->nsbi)) {
		ret = -ELITTLESPACE;
		goto stop;
	}

	///通过小根堆，选取SSD中计数最少的k个段迁移到nvm
	reset_min_heap(minh);// 将当前小根堆内当前节点数置0
	ret = get_min_k(sbi, minh, ssd_main_seg_num);
	if (ret == -ENOHOT) {
		nvm_debug(NVM_ERR, "select ssd segments fail，ENOHOT,hot_count=%d,cold_count=%d", minh->min_count,
				  nsbi->mh->max_count);
		goto stop;
	}
	//记录本次转移热段中最冷的一个
	minh->min_count = minh->nodes[0].count;

	//如果选出的k个热段中最冷的段比回收时候选出的j个段中最热的段更冷，则无需迁移
	//热大于冷1.5倍，300封顶
//	if (sbi->nsbi->mh->max_count > hot_multiple_top * 2) {
//		cmp = sbi->nsbi->mh->max_count + hot_multiple_top;
//	} else {
//		cmp = sbi->nsbi->mh->max_count + sbi->nsbi->mh->max_count / hot_multiple;
//	}
	if (!can_move(sbi->nsbi)) {
		nvm_debug(NVM_ERR, "hot_to_nvm,ready，mh->min_count=%d,gc-max_count=%d", minh->min_count,
				  sbi->nsbi->mh->max_count);
		ret = -ENOHOT;
		goto stop;
	}


	//遍历小根堆，针对每个选中的SSD段将其数据转移到NVM
	for (i = 0; i < minh->cur_num; i++) {
		ssd_segoff = minh->nodes[i].seg_off;

		secno = GET_SEC_FROM_SEG(sbi, ssd_segoff);
		if (sec_usage_check(sbi, secno))//如果是当前在用的seg，则跳过
			continue;

		//对于有效块少的段，不做热数据迁移,跳过;[不再进行二次判断]
//		valid_blocks = get_valid_blocks(sbi, ssd_segoff, false);
//		if (valid_blocks < sbi->blocks_per_seg * UP_HOT_SEG_VALID_BLOCKS / 100) {
//			nvm_debug(NVM_INFO, "hot data ready to nvm，valid count is not enough,ssd_segoff=%d,count=%d", ssd_segoff,
//					  valid_blocks);
//			continue;
//		}
		///一切就绪，准备转移ssd数据到nvm
		//1、从nvm分配段
		nvm_segoff = nvm_allocate_segment(sbi, ssd_segoff, 0);
		if (nvm_segoff == -ENVMNOSPACE) {
			//如果nvm空间不足，没有分配成功，则结束本次热数据转移
			nvm_debug(NVM_ERR, "ERROR:hot data ready to nvm，nvm no space!");
//			ret = -ENVMNOSPACE;
			goto stop;
		}
		//2、做数据转移
		do_hot_to_nvm(sbi, ssd_segoff, nvm_segoff);
		//3、如果该段数据还有效，则建立映射表
		spin_lock(&free_i->segmap_lock);//加锁，重要！避免数据不一致发生
		if (test_bit(ssd_segoff, free_i->free_segmap)) {
			if (get_map_flag(sbi->nsbi, nvm_segoff)) {
				spin_unlock(&free_i->segmap_lock);
				//此NVM段有映射，异常
//				ret = -EINVAL;
				//如果数据段已经被回收，则释放之前分配的nvm段
				spin_lock(&nsbi->segment_map_lock);
				//修改segment位图，增加空闲segment数目
				clear_bit(nvm_segoff, nsbi->segment_map);
				nsbi->nsb->main_segment_free_nums++;
				spin_unlock(&nsbi->segment_map_lock);
				goto stop;
			}
			//创建映射表，并标记为双重数据，nvm垃圾回收时如果有该标记则直接取消映射，无需再转移数据
			create_mapping(sbi, ssd_segoff, nvm_segoff, rw_map | rw_redirect | rw_double);
//			nvm_debug(NVM_INFO, "f2fs_up_hot, ssd_segoff=%d, nvm_segoff=%d, lfu_count=%d", ssd_segoff, nvm_segoff,
//					  minh->nodes[i].count);
			spin_unlock(&free_i->segmap_lock);
			ret++;
		} else {
			spin_unlock(&free_i->segmap_lock);
			//如果数据段已经被回收，则释放之前分配的nvm段
			spin_lock(&nsbi->segment_map_lock);
			//修改segment位图，增加空闲segment数目
			clear_bit(nvm_segoff, nsbi->segment_map);
			nsbi->nsb->main_segment_free_nums++;
			spin_unlock(&nsbi->segment_map_lock);
		}
	}
	stop:
	return ret;
}

int data_move_thread(void *data) {
	int ret;
	unsigned int sleep_time;
	int all_count;
	struct f2fs_sb_info *sbi = (struct f2fs_sb_info *) data;
	atomic_t *lfu_count = sbi->nsbi->lfu_count[sbi->nsbi->now_lfu_counter];
//	unsigned int percent_count = sbi->nsbi->nsb->main_segment_nums * NVM_LIMIT_FREE / 100;
	__le32 all = sbi->nsbi->nsb->main_segment_nums;
	__le32 used;
//	int up = 3 * SSD_TO_NVM / 5;
//	int down = SSD_TO_NVM / 10;
	static int times = 0;
	while (!kthread_should_stop()) {

		//计算休眠时间
		sleep_time = MIN_SLEEP_TIME + SLEEP_TIME_EXP * write_ratio / 100;
		if (util_nvm < IO_UTIL_PERCENT && util_ssd < IO_UTIL_PERCENT) {
			//io饱和率低的时候，休眠时间减半
			sleep_time /= 2;
		}
		SLEEP_MILLI_SEC(sleep_time)
		times++;
		times %= 60;

		nvm_debug(NVM_ERR, "sleep_time=%d,write_ratio=%d,util_ssd=%d,util_nvm=%d", sleep_time, write_ratio, util_ssd,
				  util_nvm);

		//判断是否需要移动计数器
		used = all - sbi->nsbi->nsb->main_segment_free_nums;
		all_count = atomic_read(&lfu_count[sbi->raw_super->segment_count_main]);
//		if ((all_count >> MOVE_LFU_COUNT) > used) {
		if (times == 0) {
			// printk(KERN_ERR "MOVE_LFU_COUNT");
			nvm_move_lfu_count(sbi);
		}

		//如果空闲段不足，执行冷数据迁移
//		mutex_lock(&sbi->gc_mutex);
		if (has_not_enough_free_nvm_secs(sbi->nsbi)) {
			ret = cold_to_ssd(sbi);
			if (ret > 0) {
				//有冷数据迁移
				nvm_debug(NVM_ERR, "move cold to ssd,count=%d", ret);
			}
		}
		//判断是否需要执行预回写
//		if (util_nvm < IO_UTIL_PERCENT && util_ssd < IO_UTIL_PERCENT && !has_not_enough_free_nvm_secs(sbi->nsbi)) {
//			//io饱和率低的时候，执行预回写
//			ret = precold_to_ssd(sbi);
//			if (ret > 0) {
//				nvm_debug(NVM_ERR, "pre move cold to ssd,count=%d", ret);
//			}
//		}
		//是否需要调整单次迁移热段数目
//		used = all - sbi->nsbi->nsb->main_segment_free_nums;
//		if (5 * used > 4 * all) {
//			sbi->nsbi->minh->k = up - (up - down) * (5 * used - 4 * all) / all;
//		}

		//进行uphot操作
		ret = hot_to_nvm(sbi);
//		mutex_unlock(&sbi->gc_mutex);
		if (ret > 0) {
			//有热数据迁移
			nvm_debug(NVM_ERR, "move hot to nvm,count=%d", ret);
		}
	}
	return 0;
}

//弃用，数据迁移统一放到一个线程处理
int start_cold_to_ssd_thread(struct f2fs_sb_info *sbi) {
	struct nvm_sb_info *nsbi = sbi->nsbi;
	struct f2fs_gc_kthread *gc_th;
	dev_t dev = nsbi->nbdev->bd_dev;
	int err = 0;

	gc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_gc_kthread), GFP_KERNEL);
	if (!gc_th) {
		err = -ENOMEM;
		goto out;
	}

	gc_th->urgent_sleep_time = DEF_GC_THREAD_URGENT_SLEEP_TIME;
	gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	gc_th->gc_wake = 0;

	nsbi->gc_thread = gc_th;
	init_waitqueue_head(&nsbi->gc_thread->gc_wait_queue_head);
	nsbi->gc_thread->f2fs_gc_task = kthread_run(cold_to_ssd_thread, sbi, "f2fs_gc-%u:%u", MAJOR(dev), MINOR(dev));

	if (IS_ERR(gc_th->f2fs_gc_task)) {
		err = PTR_ERR(gc_th->f2fs_gc_task);
		kfree(gc_th);
		sbi->nsbi->gc_thread = NULL;
	}
	out:
	return err;
}

/**
 * 定时up ssd的热数据
 */
int start_data_move_thread(struct f2fs_sb_info *sbi) {
	int err;
	struct task_struct *data_move_task;
	data_move_task = kthread_create(data_move_thread, sbi, "uphot_task");
	if (IS_ERR(data_move_task)) {
		nvm_debug(NVM_ERR, "Unable to start kernel thread.");
		err = PTR_ERR(data_move_task);
		data_move_task = NULL;
		return err;
	}
	sbi->nsbi->data_move_task = data_move_task;
	wake_up_process(data_move_task);
	return 0;
}