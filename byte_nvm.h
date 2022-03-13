//
// 2022年1月6日 Created by cocopork
//
#ifndef __F2FS_BYTE_NVM_H__
#define __F2FS_BYTE_NVM_H__

#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include "f2fs.h"

/*
 *  byte_nvm.c函数声明 
 */
int init_byte_nvm_dax(struct f2fs_sb_info *sbi, struct nvm_super_block **byte_nsb);
int init_byte_nvm_private_info(struct f2fs_sb_info *sbi);
int init_byte_nvm_sb_info(struct f2fs_sb_info *sbi, struct nvm_super_block *byte_nsb);
void byte_nvm_flush_mpt_pages(struct f2fs_sb_info *sbi, int flush_all);
//f2fs_fill_super
int f2fs_bnvm_get_valid_checkpoint_first_mount(struct f2fs_sb_info *sbi);
int f2fs_move_cp_super_to_bnvm(struct f2fs_sb_info *sbi);
int f2fs_move_cp_content_to_bvnm(struct f2fs_sb_info *sbi);
//do_checkpoint
int bnvm_do_checkpoint(struct f2fs_sb_info *sbi, struct cp_control *cpc);
//build_curseg
void bnvm_read_compacted_summaries(struct f2fs_sb_info *sbi);
int bnvm_read_normal_summaries(struct f2fs_sb_info *sbi, int type);

/* 访问元数据区域函数 */
inline struct f2fs_nat_entry * f2fs_bnvm_get_nat_entry(struct f2fs_sb_info *sbi, 
                                        nid_t nid);
inline struct f2fs_sit_entry * f2fs_bnvm_get_sit_entry(struct f2fs_sb_info *sbi, 
                                        unsigned int segno);
inline struct f2fs_summary_block * f2fs_bnvm_get_sum_blk(struct f2fs_sb_info *sbi, 
										unsigned int segno);                                        

/* 测试函数 */
void print_byte_nvm_mount_parameter(struct f2fs_sb_info *sbi);
void print_raw_info(struct f2fs_sb_info *sbi);
void print_ckpt_info(struct f2fs_sb_info *sbi);

/* 
 * inline function
 */
static inline struct nvm_sb_info * F2FS_BYTE_NSB_I(struct f2fs_sb_info *sbi) 
{
    return sbi->byte_nsbi;
}

static inline struct byte_nvm_private *F2FS_BYTE_NVM_PRIVATE(struct f2fs_sb_info *sbi) 
{
    return sbi->byte_nsbi->byte_private;
}

static inline unsigned char *F2FS_BYTE_NVM_ADDR(struct f2fs_sb_info *sbi) 
{
    return sbi->byte_nsbi->byte_private->virt_addr;
}

static inline unsigned char *F2FS_BYTE_NVM_BLK_TO_ADDR(struct f2fs_sb_info *sbi, block_t blk_addr) 
{
    return sbi->byte_nsbi->byte_private->virt_addr + blk_addr * sbi->blocksize;
}

static inline struct f2fs_checkpoint * f2fs_bnvm_get_cp_ptr(struct f2fs_sb_info *sbi, unsigned int cp_pack_no)
{
    if(!(sbi->byte_nsbi->nvm_flag & NVM_BYTE_PRIVATE_READY)) {
        printk(KERN_INFO"ZN trap: bnvm private is not ready");
        return NULL;
    }
        
    if (cp_pack_no == 1)
        /* 获取cp pack的块首 */
        return (struct f2fs_checkpoint *)(F2FS_BYTE_NVM_ADDR(sbi)
                                     + F2FS_BYTE_NVM_PRIVATE(sbi)->cp_blkaddr * F2FS_BLKSIZE);
    else if (cp_pack_no == 2)
    /* 获取cp pack 2的块首 */
        return (struct f2fs_checkpoint *)(F2FS_BYTE_NVM_ADDR(sbi)
                                    + (F2FS_BYTE_NVM_PRIVATE(sbi)->cp_blkaddr + sbi->blocks_per_seg) 
                                    * F2FS_BLKSIZE);  
    else
        return NULL;      
}

static inline block_t f2fs_bnvm_get_valid_cp_addr(struct f2fs_sb_info *sbi)
{
    block_t sum_blk = F2FS_BYTE_NVM_PRIVATE(sbi)->cp_blkaddr;
    if (sbi->cur_cp_pack == 2)
		sum_blk += sbi->blocks_per_seg;
    return sum_blk;    
}

static inline block_t f2fs_bnvm_get_cp_sum(struct f2fs_sb_info *sbi)
{/* 获取nvm有效cp pack中的第一个summary blk块号 */
    block_t sum_blk = f2fs_bnvm_get_valid_cp_addr(sbi);
    sum_blk += le32_to_cpu(F2FS_CKPT(sbi)->cp_pack_start_sum);
    return sum_blk;
}
/**
 * @brief 获取不同温度的data或node summary block地址，参考sum_blk_addr
 * 
 * @param sbi 
 * @param base 有node summary就是NR_CURSEG_TYPE，没有则是NR_CURSEG_DATA_TYPE
 * @param type CURSEG_XXX_DATA或CURSEG_XXX_NODE
 * @return block_t 
 */
static inline block_t f2fs_bnvm_get_sum_blk_addr(struct f2fs_sb_info *sbi, int base, int type)
{
    return f2fs_bnvm_get_valid_cp_addr(sbi) +
            le32_to_cpu(F2FS_CKPT(sbi)->cp_pack_total_block_count)
                    - (base + 1) + type;/* 这个1是cp pack尾部的cp super block */
}
/*
 * flag for byte_nvm_private->s_flag
*/
#define BNVM_PRIVATE_READY  1 /* 私有参数准备完毕 */
#endif  /* __F2FS_BYTE_NVM_H__ */
