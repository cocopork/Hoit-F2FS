#! /bin/bash
# author:cocopork
# write "HelloWorld" to a newfile

###################################
#           shell 方式写入
###################################
# if [ -f "/mnt/f2fs/newfile" ]; then
# rm /mnt/f2fs/newfile
# fi

# touch /mnt/f2fs/newfile
# chmod 777 /mnt/f2fs/newfile

# # * 是通配符，乘法要用 \*
# count=`expr $1 \* 1`
# string="TestString:"
# loopstring="a"

# echo $string>>/mnt/f2fs/newfile
# for i in $( seq 1 $count )
# do
#     echo $loopstring>>/mnt/f2fs/newfile
# done

###################################
#           C语言 方式写入
###################################
gcc -o ./shfile/c_write_to_file ./shfile/c_write_to_file.c
./shfile/c_write_to_file $1
