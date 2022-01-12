//
// 2022年1月6日 Created by cocopork
//

#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include "f2fs.h"
/*
 *  byte_nvm.c函数声明 
 */
int init_byte_nvm_dax(struct f2fs_sb_info *sbi, struct nvm_super_block **byte_nsb);
int init_byte_nvm_sb_info(struct f2fs_sb_info *sbi, struct nvm_super_block *byte_nsb);
void byte_nvm_flush_mpt_pages(struct f2fs_sb_info *sbi, int flush_all);

