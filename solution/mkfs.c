#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include "wfs.h"

int myround(int n, int r) {
    return (n > 0) ? ((n + r - 1) / r) * r : 0;
}

int main(int argc, char *argv[]) {
    int raid_mode = -1;
    char **disks = NULL;
    int disk_cnt = 0;
    struct wfs_sb sb = {0};
    int opt;

    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) switch (opt) {
        case 'r':
            if (strcmp(optarg, "0") == 0) raid_mode = 0;
            else if (strcmp(optarg, "1") == 0) raid_mode = 1;
            else if (strcmp(optarg, "1v") == 0) raid_mode = 2;

            else exit(1);          
            break;
        case 'd':
            if(disk_cnt >= MAX_DISK) exit(1);
            
            disks = realloc(disks, sizeof(char *) * (disk_cnt + 1));
            if (!disks) exit(1);
            
            disks[disk_cnt++] = strdup(optarg);

            if(strlen(disks[disk_cnt - 1]) > MAX_NAME) exit(1);
            break;
        case 'i':
            sb.num_inodes = myround(atoi(optarg), 32);
            break;
        case 'b':
            sb.num_data_blocks = myround(atoi(optarg), 32);
            break;
        default:
            exit(1);
    }

    if ((raid_mode != 1 && raid_mode != 0) || disk_cnt < 2 || sb.num_inodes == 0 || sb.num_data_blocks == 0) exit(1);

    size_t inode_bitmap_size = (size_t)myround(sb.num_inodes, 8) / 8;
    size_t data_block_bitmap_size = (size_t)myround(sb.num_data_blocks, 8) / 8;

    sb.raid_mode = raid_mode;
    sb.i_bitmap_ptr = (off_t)sizeof(struct wfs_sb);
    sb.d_bitmap_ptr = (off_t)sb.i_bitmap_ptr + inode_bitmap_size;
    sb.i_blocks_ptr = (off_t)myround((sb.d_bitmap_ptr + data_block_bitmap_size), BLOCK_SIZE);
    sb.d_blocks_ptr = (off_t)myround((sb.i_blocks_ptr + sb.num_inodes * BLOCK_SIZE), BLOCK_SIZE);

    off_t total_size = myround(sb.d_blocks_ptr + sb.num_data_blocks * BLOCK_SIZE, BLOCK_SIZE);

    sb.timestamp = (int) time(NULL);

    for (int i = 0; i < disk_cnt; i++) strcpy(sb.disks[i], disks[i]);

    for (int i = 0; i < disk_cnt; i++) {
        int fd;
        sb.mount_index = i;
        if ((fd = open(disks[i], O_RDWR)) <= 0) exit(1);

        struct stat st;
        if (stat(disks[i], &st) != 0) exit(1);

        if (total_size > st.st_size) exit(-1);

        if (lseek(fd, 0, SEEK_SET) < 0 || write(fd, &sb, sizeof(struct wfs_sb)) < 0) exit(-1);
 
        uint8_t *zeroed_i_bitmap = calloc(1, inode_bitmap_size);
        uint8_t *zeroed_d_bitmap = calloc(1, data_block_bitmap_size);
        if (!zeroed_i_bitmap || !zeroed_d_bitmap) exit(1);

        zeroed_i_bitmap[0] = 1;

        if (lseek(fd, sb.i_bitmap_ptr, SEEK_SET) < 0 || write(fd, zeroed_i_bitmap, inode_bitmap_size) < 0) exit(-1);
        if (lseek(fd, sb.d_bitmap_ptr, SEEK_SET) < 0 || write(fd, zeroed_d_bitmap, data_block_bitmap_size) < 0) exit(-1);

        free(zeroed_i_bitmap);
        free(zeroed_d_bitmap);

        struct wfs_inode rootInode = {0};
        rootInode.num = 0;
        rootInode.mode = S_IRWXU | S_IFDIR;
        rootInode.uid = getuid();
        rootInode.gid = getgid();
        rootInode.nlinks = 2;
        rootInode.size = 0;
        rootInode.atim = time(NULL);
        rootInode.mtim = time(NULL);
        rootInode.ctim = time(NULL);
        for(int i = 0; i < N_BLOCKS; i ++) {
            rootInode.blocks[i] = -1;
        }

        if (lseek(fd, sb.i_blocks_ptr, SEEK_SET) < 0 || write(fd, &rootInode, sizeof(struct wfs_inode)) < 0) exit(1);
    }

    free(disks);
    return 0;
}
