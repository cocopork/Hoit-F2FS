# include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
int main (int argc, char* argv[]){
    int fd;
    char string[32] = "HelloWorld\0";
    off_t offset=0;
    printf("start writing data to /mnt/f2fs/newfile\n");
    fd = open("/mnt/f2fs/newfile",O_RDWR|O_CREAT|O_TRUNC);
    if(fd == -1){
        printf("error to open /mnt/f2fs/newfile\n");
        return 0;
    }
    
    // printf("argc = %d \n",argc);
    // printf("argv[1] = %s \n",argv[1]);
    if(argc == 2)
        offset = atoi(argv[1]) * 1024;//单位是1KB
    
    printf("write offset is %ld KB\n",offset);

    lseek(fd, offset, SEEK_SET);
    write(fd, string, sizeof(string));
    close(fd);
}