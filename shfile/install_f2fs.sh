#! /bin/bash
# author:cocopork
# install f2fs mod and mount to /mnt/f2fs

insmod f2fs.ko

mkfs.f2fs -f -t f2fs /dev/sda3

mount -o nvm_path=/dev/pmem0 /dev/sda3 /mnt/f2fs
