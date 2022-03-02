//
// Created by ZN on 2022年2月25日.
//

#include <linux/blkdev.h>
#include <linux/fs.h>
#include "f2fs.h"

void * f2fs_dax_addr(struct f2fs_sb_info *sbi, size_t offset){
    struct nvm_sb_info *byte_nsbi = F2FS_BYTE_NSB_I(sbi);
    return (byte_nsbi->byte_private->virt_addr + offset);
}