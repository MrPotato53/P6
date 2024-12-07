#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include "wfs.h"

int disk_count;
void *regions[MAX_DISK];
int raid_mode = -1;


void free_block(off_t blk, int disk_count) {
	// Retrieve superblock for disk
	struct wfs_sb* sb = (struct wfs_sb*)regions[disk_count];

	memset((char*)regions[disk_count] + blk, 0, BLOCK_SIZE);

	off_t blk_index = (blk - sb->d_blocks_ptr) / BLOCK_SIZE;
	uint32_t* bitmap = (uint32_t*)((char*)regions[disk_count] + sb->d_bitmap_ptr);

	bitmap[blk_index / 32] ^= (1 << blk_index % 32);
}

void free_inode(struct wfs_inode* inode, int disk_count) {
	struct wfs_sb* sb = (struct wfs_sb*)regions[disk_count];

	memset((char*)inode, 0, BLOCK_SIZE);

	off_t blk_index = ((char*)inode - (char *)regions[disk_count] + sb->i_blocks_ptr) / BLOCK_SIZE;
	uint32_t* bitmap = (uint32_t*)((char*)regions[disk_count] + sb->i_blocks_ptr);

	bitmap[blk_index / 32] ^= (1 << blk_index % 32);
}

struct wfs_inode* get_inode(int n, int disk_count) {
	struct wfs_sb* sb = (struct wfs_sb*)regions[disk_count];

	uint32_t* bitmap = (uint32_t*)((char*)regions[disk_count] + sb->i_blocks_ptr);

	if (bitmap[n / 32] & (1 << n % 32))
	return (struct wfs_inode*)(((char*)regions[disk_count] + sb->i_blocks_ptr) + n * BLOCK_SIZE);

	return NULL;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    return 0;
}

static int wfs_mknod(const char* path, mode_t mode, dev_t rdev) {
    return 0;

}

static int wfs_mkdir(const char* path, mode_t mode) {
    return 0;

}

static int wfs_unlink(const char* path) {
    return 0;

}

static int wfs_rmdir(const char* path) {
    return 0;

}

static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    return 0;

}

static int wfs_write(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    return 0;

}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    return 0;

}


static struct fuse_operations ops = {
  .getattr = wfs_getattr,
  .mknod   = wfs_mknod,
  .mkdir   = wfs_mkdir,
  .unlink  = wfs_unlink,
  .rmdir   = wfs_rmdir,
  .read    = wfs_read,
  .write   = wfs_write,
  .readdir = wfs_readdir,
};


int main(int argc, char *argv[]) {
	int fd[MAX_DISK];
	void *tmp_region[MAX_DISK];
	struct stat stats;
	int unique_run_id = -1;
    
	// Count disks
	for(int i = 0; i < argc; i++) {
		if(argv[i][0] == '-') break;
		disk_count++;
	}

	if(disk_count < 2) {
		perror("Not enough disks, need at least 2\n");
		exit(1);
	}

	// Open all disks and map them to regions array
	for(int i = 0; i < disk_count; i++) {
		if(open(argv[i + 1], O_RDWR, 0666) <= 0) {
			perror("open failed\n");
			exit(-ENOENT);
		}

		if(fstat(fd[i], &stats) < 0) {
			perror("fstat failed\n");
			exit(-1);
		}

		tmp_region[i] = mmap(NULL, stats.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd[i], 0);
		if(tmp_region[i] == 0) {
			perror("mmap failed\n");
			exit(-1);
		}
	}

	// Reorder disks based on index in superblock
	for(int i = 0; i < disk_count; i++) {
		regions[((struct wfs_sb *)tmp_region[i])->mount_index] = tmp_region[i];

		// make sure all disks are from same mkfs run
		if(unique_run_id == -1) unique_run_id = ((struct wfs_sb *)tmp_region[i])->timestamp;
		if(unique_run_id != ((struct wfs_sb *)tmp_region[i])->timestamp) {
			perror("disks are not from same run of mkfs\n");
			exit(1);
		}
	}

	// Update argc and remove disk arguments in argv
	argc -= disk_count;
	argv[disk_count] = argv[0];
	argv += disk_count;

	raid_mode = ((struct wfs_sb *)regions[0])->raid_mode;

	int fuse_out = fuse_main(argc, argv, &ops, NULL);

	// Unmap all regions
	for(int i = 0; i < disk_count; i++) {
		if(fstat(fd[i], &stats) < 0) {
			perror("fstat failed\n");
			exit(-1);
		}

		// using tmp_regions because it is in same order as fd
		munmap(tmp_region[i], stats.st_size);
		close(fd[i]);
	}

    return fuse_out;
}
