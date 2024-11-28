#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "wfs.h"

#define BK_ROUND_UP(n) ((n + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE)


void write_superblock(int fd, struct wfs_sb *sb) {

    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek");
        exit(-1);
    }
    if (write(fd, sb, sizeof(struct wfs_sb)) < 0) {
        perror("write");
        exit(-1);
    }
}

void write_bitmaps(int fd, int ibitmapsize, int dbitmapsize, struct wfs_sb *sb) {
    char *ibitmap_zero = calloc(1, ibitmapsize);
    char *dbitmap_zero = calloc(1, dbitmapsize);
    if(!ibitmap_zero || !dbitmap_zero) {
        perror("calloc");
        exit(-1);
    }

    ibitmap_zero[0] = 1; // for root inode

    // write inode bitmap
    if (lseek(fd, sb->i_bitmap_ptr, SEEK_SET) < 0) {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, ibitmap_zero, ibitmapsize) < 0) {
        perror("write");
        exit(-1);
    }

    // write data block bitmap
    if (lseek(fd, sb->d_bitmap_ptr, SEEK_SET) < 0) {
        perror("lseek");
        exit(-1);
    }
    if(write(fd, dbitmap_zero, dbitmapsize) < 0) {
        perror("write");
        exit(-1);
    }

    free(ibitmap_zero);
    free(dbitmap_zero);
}

void write_root_inode(int fd, struct wfs_sb *sb) {
    struct wfs_inode rootInode = {0};
    rootInode.num = 0;
    rootInode.mode = S_IRWXU;
    rootInode.uid = getuid();
    rootInode.gid = getgid();
    rootInode.nlinks = 2;  // for .. and . directories
    rootInode.size = 0;
    rootInode.atim = time(NULL);
    rootInode.mtim = time(NULL);
    rootInode.ctim = time(NULL);

    if (lseek(fd, sb->i_blocks_ptr, SEEK_SET) < 0) {
        perror("lseek");
        exit(-1);
    }

    if (write(fd, &rootInode, sizeof(struct wfs_inode)) < 0) {
        perror("write");
        exit(-1);
    }
}

void write_inodes(int fd, struct wfs_sb *sb) {
    int size = sb->num_inodes * BLOCK_SIZE;
    char *iblocks_zeros = calloc(1, size);
    if (lseek(fd, sb->i_blocks_ptr, SEEK_SET) < 0) {
        perror("lseek");
        exit(-1);
    }
    if (write(fd, iblocks_zeros, size) < 0) {
        perror("write");
        exit(-1);
    }
    free(iblocks_zeros);
}

int main (int argc, char *argv[]) {
    int raid_mode = -1;
    size_t inode_count = 0, data_block_count = 0;
    char **disk_files = NULL;
    int disks_count = 0;

    int opt;

    struct wfs_sb sb = {0};

    // Parse arguments
    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) {
        switch(opt) {
            case('r'):
                raid_mode = atoi(optarg);
                if((raid_mode != 0) && (raid_mode != 1) && (raid_mode != 5) && (raid_mode != 10)) {
                    perror("Invalid raid mode, must be 0, 1, 5, or 10.\n");
                    exit(1);
                }
                break;
            case('d'):
                disk_files = realloc(disk_files, sizeof(char *) * (disks_count + 1));
                if(!disk_files) {
                    perror("Realloc failed\n");
                    exit(1);
                }
                disk_files[disks_count++] = strdup(optarg);
                break;
            case('i'):
                inode_count = atoi(optarg);
                break;
            case('b'):
                data_block_count = atoi(optarg);
                break;
            default:
                perror("Usage: mkfs -d [raid_mode] -d [disk1] -d [disk2] -i [inode count] -b [block count]\n");
                exit(1);
        }
    }

    data_block_count = (data_block_count + 31) / 32 * 32; // Round data blocks up by 32
    inode_count = (inode_count + 31) / 32 * 32; // Round inodes up by 32
    // Check that arguments are parsed correctly
    if(raid_mode == -1 || disks_count == 0 || data_block_count == 0 || inode_count == 0) {
        perror("Incorrect initializiation arguments\n");
        exit(1);
    }

    // Calculate offsets / sizes
    sb.raid_mode = raid_mode;
    sb.num_inodes = inode_count;
    sb.num_data_blocks = data_block_count;
    int ibitmap_size = (inode_count + 7) / 8;
    int dbitmap_size = (data_block_count + 7) / 8;

    sb.i_bitmap_ptr = (off_t)sizeof(struct wfs_sb);
    sb.d_bitmap_ptr = sb.i_bitmap_ptr + ibitmap_size;
    sb.i_blocks_ptr = BK_ROUND_UP(sb.d_bitmap_ptr + dbitmap_size);
    sb.d_blocks_ptr = BK_ROUND_UP(sb.i_blocks_ptr + BLOCK_SIZE * inode_count);

    off_t total_size = BK_ROUND_UP(sb.d_blocks_ptr + BLOCK_SIZE * data_block_count);

    // Write each of the disks
    int fd;
    struct stat disk_st;
    for (int i = 0; i < disks_count; i++) {
        strcpy(sb.disks[i], disk_files[i]); // Add disk to supernode

        if ((fd = open(disk_files[i], O_CREAT | O_RDWR)) <= 0) {
            perror("Failed to open disk file\n");
            exit(1);
        }

        if (stat(disk_files[i], &disk_st) != 0)
        {
            perror("Failed to stat disk\n");
            exit(-1);
        }

        if(total_size > disk_st.st_size) {
            perror("Not enough room in disk\n");
            exit(-1);
        }

        write_superblock(fd, &sb);
        write_bitmaps(fd, ibitmap_size, dbitmap_size, &sb);
        write_root_inode(fd, &sb);
    }

    free(disk_files);

    return 0;
}