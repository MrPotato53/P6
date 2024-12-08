#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "wfs.h"

int disk_count;
void *regions[MAX_DISK];
int raid_mode = -1;
struct wfs_sb *superblock;


void free_block(off_t blk, int disk_count) {
	memset((char*)regions[disk_count] + blk, 0, BLOCK_SIZE);

	off_t blk_index = (blk - superblock->d_blocks_ptr) / BLOCK_SIZE;
	uint32_t* bitmap = (uint32_t*)((char*)regions[disk_count] + superblock->d_bitmap_ptr);

	bitmap[blk_index / 32] ^= (1 << blk_index % 32);
}

void free_inode(struct wfs_inode* inode, int disk_count) {
	memset((char*)inode, 0, BLOCK_SIZE);

	off_t blk_index = ((char*)inode - (char *)regions[disk_count] + superblock->i_blocks_ptr) / BLOCK_SIZE;
	uint32_t* bitmap = (uint32_t*)((char*)regions[disk_count] + superblock->i_blocks_ptr);

	bitmap[blk_index / 32] ^= (1 << blk_index % 32);
}

struct wfs_inode* get_inode(int n, int disk_count) {
	uint32_t* bitmap = (uint32_t*)((char*)regions[disk_count] + superblock->i_blocks_ptr);

	if (bitmap[n / 32] & (1 << n % 32))
	return (struct wfs_inode*)(((char*)regions[disk_count] + superblock->i_blocks_ptr) + n * BLOCK_SIZE);

	return NULL;
}

off_t allocate_block(int disk_count) {
    struct wfs_sb* sb = (struct wfs_sb*)region[disk_count];
	uint32_t* bitmap = (uint32_t*)((char*)regions[disk_count] + superblock->d_bitmap_ptr);

	off_t blk = -1;
	for (uint32_t i = 0; i < sb->num_data_blocks / 32; i++) {
        if (bitmap[i] == 0xFFFFFFFF)
            continue;

        for (uint32_t k = 0; k < 32; k++)
            if (!((bitmap[i] >> k) & 1)) {
                bitmap[i] = bitmap[i] | (1 << k);
                blk =  32 * i + k;
            }
    }
	
    if (blk < 0) return -ENOSPC;

    return sb->d_blocks_ptr + BLOCK_SIZE * blk;
}

struct wfs_inode* allocate_inode(int disk) {
	struct wfs_sb* sb = (struct wfs_sb*)region[disk_count];
	uint32_t* bitmap = (uint32_t*)((char*)regions[disk_count] + superblock->i_bitmap_ptr);

	off_t blk = -1;
	for (uint32_t i = 0; i < sb->num_inodes / 32; i++) {
        if (bitmap[i] == 0xFFFFFFFF)
            continue;

        for (uint32_t k = 0; k < 32; k++)
            if (!((bitmap[i] >> k) & 1)) {
                bitmap[i] = bitmap[i] | (1 << k);
                blk =  32 * i + k;
            }
    }

    if (blk < 0) return -ENOSPC;

    struct wfs_inode* inode = (struct wfs_inode*)((char*)regions[disk_count] + superblock->i_bitmap_ptr + BLOCK_SIZE * blk);
    inode->num = blk;
    return inode;
}

int block_exists(struct wfs_sb *sb, off_t block_index) {
	int8_t *b_bitmap = sb + superblock->d_bitmap_ptr;
	return (int)(b_bitmap + (block_index / 8)) & 0x1 << block_index % 8;
}

void *get_block(off_t block_index) {
	struct wfs_dentry *dir_block;
	if(raid_mode == 0) {
		// Raid 0 Case
		int disk = block_index % disk_count;
		int index = block_index / disk_count;

		if(!block_exists((struct wfs_sb*)regions[disk], index)) {
			perror("Block was not allocated\n");
			return -ENOENT;
		}
		dir_block = (struct wfs_dentry *)((char *)regions[disk] + superblock->d_blocks_ptr + (index * BLOCK_SIZE));
	} else if(raid_mode == 1) {
		// Raid 1 Case

		// All disks are identical so just take from disk 0
		if(!block_exists((struct wfs_sb*)regions[0], block_index)) {
			perror("Block was not allocated\n");
			return -ENOENT;
		}
		dir_block = (struct wfs_dentry *)((char *)regions[0] + superblock->d_blocks_ptr + (block_index * BLOCK_SIZE));
	} else if(raid_mode == 2) {
		// Raid 1v Case
		struct wfs_dentry *unique_blocks[disk_count];
		memset(unique_blocks, 0, sizeof(unique_blocks));
		int block_counts[disk_count];
		memset(block_counts, 0, sizeof(block_counts));
		int max_count_index = 0;
		struct wfs_dentry *curr_block;

		// Walk through all disks
		// take block at index in every disk, compare them against other disks
		// Find block with the most occurrences, return that with lowest index
		for(int i = 0; i < disk_count; i++) {
			curr_block = (struct wfs_dentry *)((char *)regions[i] + superblock->d_blocks_ptr + (block_index * BLOCK_SIZE));
			
			// Skip nonexistent blocks
			if(!block_exists((struct wfs_sb*)regions[i], block_index)) continue;
			
			for(int j = 0; j < sizeof(unique_blocks); j++) {
				if(unique_blocks[j] == 0) {
					// Never before seen block
					unique_blocks[i] = curr_block;
					block_counts[j]++;
				} else if(memcmp(curr_block, unique_blocks[j], BLOCK_SIZE) == 0) {
					// Seen before block
					block_counts[j]++;
				} else {
					continue;
				}

				// Update the most frequently occurring block
				if(block_counts[j] >= block_counts[max_count_index]) {
					max_count_index = j > max_count_index ? max_count_index : j;
				}
				break;
			}
		}
		if((dir_block = unique_blocks[max_count_index]) == 0) {
			perror("Block was not allocated on any disk\n");
			return -ENOENT;
		}
	} else {
		perror("Invalid Raid Mode\n");
		return -1;
	}
	return (void *)dir_block;
}

struct wfs_dentry *find_dentry(struct wfs_inode *dir_inode, const char *name) {
	off_t *block_indicies = dir_inode->blocks;
	struct wfs_dentry *curr_dentry;

	// Search each data block
	for(int i = 0; i < D_BLOCK; i++) {
		if(block_indicies[i] == 0) continue;
		if((curr_dentry = (struct wfs_dentry *) get_block(block_indicies[i])) <= 0) return curr_dentry;
		// Search each dentry in data block
		for(int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
			if(strcmp(curr_dentry[j].name, name) == 0) {
				return &curr_dentry[j];
			}
		}
	}
	// No matching dentry found
	return -ENOENT;
}

struct wfs_inode *get_inode_from_path(char *path) {

	struct wfs_inode *curr_inode = get_inode(0, 0);

	if(strcmp(path, "/") == 0) {
		return curr_inode;
	}

	char *path_copy = strdup(path);
	char *rem_path = path_copy;
	char *tok = strtok(rem_path, "/");
	struct wfs_dentry *dentry;
	while(tok) {
		// Check if curr inode is directory
		if(!S_IFDIR(curr_inode->mode)) {
			free(rem_path);
			return -ENOENT;
		}

		// Find dentry in current inode and corresponding next inode
		if((dentry = find_dentry(curr_inode, tok)) <= 0) {
			free(rem_path);
			return dentry;
		}

		if((curr_inode = get_inode(dentry->num, 0)) <= 0) {
			free(rem_path);
			return curr_inode;
		}

		tok = strtok(NULL, "/");
	}

	free(rem_path);
	return curr_inode;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
	struct wfs_inode *inode;
	if((inode = get_inode_from_path(path)) <= 0) {
		return inode;
	}

	stbuf->st_uid = inode->uid;
	stbuf->st_gid = inode->gid;
	stbuf->st_atime = inode->atim;
	stbuf->st_mtime = inode->mtim;
	stbuf->st_mode = inode->mode;
	stbuf->st_size = inode->size;
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

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
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

	superblock = ((struct wfs_sb *)regions[0]);
	raid_mode = superblock->raid_mode;

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
