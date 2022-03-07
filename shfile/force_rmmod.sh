#! /bin/bash
# author:cocopork
# force remove f2fs when oops happens

#  在f2fs直接删除，通过 `modname` 指定待卸载驱动的信息
insmod ../force_rmmod/force_rmmod.ko modname=f2fs

#  查看是否加载成功, `exit` 函数是否正常替换
dmesg | tail -l

#  卸载 `f2fs` 驱动
sudo rmmod f2fs

#  卸载 `force_rmmod` 驱动
sudo rmmod force_rmmod