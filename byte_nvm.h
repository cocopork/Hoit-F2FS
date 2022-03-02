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
int init_byte_nvm_sb_info(struct f2fs_sb_info *sbi, struct nvm_super_block *byte_nsb);
void byte_nvm_flush_mpt_pages(struct f2fs_sb_info *sbi, int flush_all);
void print_byte_nvm_mount_parameter(struct f2fs_sb_info *sbi);
void print_meta_parameter(struct f2fs_sb_info *sbi);

/* 访问元数据区域函数 */
struct f2fs_nat_entry * f2fs_byte_nvm_get_nat_entry(struct f2fs_sb_info *sbi, nid_t nid);

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

static inline char *F2FS_BYTE_NVM_ADDR(struct f2fs_sb_info *sbi) 
{
    return sbi->byte_nsbi->byte_private->virt_addr;
}

static inline struct f2fs_checkpoint * F2FS_BYTE_NVM_CP_1(struct f2fs_sb_info *sbi)
{/* 获取cp pack 1的块首 */
    return (struct f2fs_checkpoint *)(F2FS_BYTE_NVM_ADDR(sbi)
                                 + F2FS_BYTE_NVM_PRIVATE(sbi)->cp_blkaddr * F2FS_BLKSIZE);
}

static inline struct f2fs_checkpoint * F2FS_BYTE_NVM_CP_2(struct f2fs_sb_info *sbi)
{/* 获取cp pack 2的块首 */
    return (struct f2fs_checkpoint *)(F2FS_BYTE_NVM_ADDR(sbi)
                                 + (F2FS_BYTE_NVM_PRIVATE(sbi)->cp_blkaddr + sbi->blocks_per_seg) 
                                 * F2FS_BLKSIZE);
}


#endif  /* __F2FS_BYTE_NVM_H__ */
