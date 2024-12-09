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


void free_block(off_t blk) {
	int disk;
	if(raid_mode == 0) {
		disk = regions[blk % disk_count];
		uint8_t* bitmap = (uint8_t*)((char*)regions[disk] + superblock->d_bitmap_ptr);
		bitmap[blk / 8] &= !(1 << (blk % 8));
	} else {
		for(int i = 0; i < disk_count; i++) {
			uint8_t* bitmap = (uint8_t*)((char*)regions[i] + superblock->d_bitmap_ptr);
			bitmap[blk / 8] &= !(1 << (blk % 8));
		}
	}
}

void free_inode(int index) {
	struct wfs_inode *inode;
	if((inode = get_inode(index, -1)) <= 0) {
		return inode;
	}

	for(int i = 0; i <= D_BLOCK; i++) {
		if(inode->blocks[i] != 0) {
			free_block(inode->blocks[i]);
		}
	}

	if(inode->blocks[IND_BLOCK] != 0) {
		off_t *ind_block;
		if((ind_block = get_block(inode->blocks[IND_BLOCK], -1)) <= 0) {
			return ind_block;
		}

		for(int i = 0; i <= BLOCK_SIZE / sizeof(off_t); i++) {
			if(ind_block[i] != 0) {
				free_block(ind_block[i]);
			}
		}
		free(ind_block);
	}

	for(int i = 0; i <= disk_count; i++) {
		uint8_t* bitmap = (uint8_t*)((char*)regions[i] + superblock->i_blocks_ptr);
		bitmap[index / 8] &= !(1 << (index % 8));
	}
	return 0;
}

struct wfs_inode* get_inode(int n, int disk) {
	uint8_t* bitmap = (uint8_t*)((char*)regions[disk] + superblock->i_blocks_ptr);

	if (bitmap[n / 8] & (1 << n % 8))
	return (struct wfs_inode*)(((char*)regions[disk] + superblock->i_blocks_ptr) + n * BLOCK_SIZE);

	return NULL;
}

off_t allocate_block() {
	uint8_t* bitmap = (uint8_t*)((char*)regions[0] + superblock->d_bitmap_ptr);

	off_t blk = -1;
	for (uint8_t i = 0; i < superblock->num_data_blocks / 8; i++) {
        if (bitmap[i] == 0xFF)
            continue;

        for (uint8_t k = 0; k < 8; k++) {
            if (!((bitmap[i] >> k) & 1)) {
				// allocate inodes on all disks if raid is 1 or 1v
				for(int j = 0; j < disk_count; j++) {
					bitmap = (uint8_t*)((char*)regions[j] + superblock->d_bitmap_ptr);
					bitmap[i] = bitmap[i] | (1 << k);
				}
                blk =  8 * i + k;
            }
		}
    }
	
    if (blk < 0) return -ENOSPC;

    return superblock->d_blocks_ptr + BLOCK_SIZE * blk;
}

off_t allocate_inode(mode_t mode) {
	uint8_t* bitmap = (uint8_t*)((char*)regions[0] + superblock->i_bitmap_ptr);

	off_t blk = -1;
	for (uint32_t i = 0; i < superblock->num_inodes / 8; i++) {
        if (bitmap[i] == 0xFF)
            continue;

        for (uint32_t k = 0; k < 8; k++) {
            if (!((bitmap[i] >> k) & 1)) {
				// allocate inodes on all disks
				for(int j = 0; j < disk_count; j++) {
					bitmap = (uint8_t*)((char*)regions[j] + superblock->i_bitmap_ptr);
					bitmap[i] = bitmap[i] | (1 << k);
				}
                blk =  8 * i + k;
            }
		}
    }

    if (blk < 0) return -ENOSPC;

    struct wfs_inode* inode = (struct wfs_inode*)((char*)regions[disk_count] + superblock->i_bitmap_ptr + BLOCK_SIZE * blk);
    inode->num = blk;
	inode->mode = mode;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->size = 0;
    inode->nlinks = 1;
    return blk;
    return blk;
}

int block_exists(struct wfs_sb *sb, off_t block_index) {
	int8_t *b_bitmap = sb + superblock->d_bitmap_ptr;
	return (int)(b_bitmap + (block_index / 8)) & 0x1 << block_index % 8;
}

// use disk = -1 when using RAID 1v to get most frequent disk with lowest index
void *get_block(off_t block_index, int disk) {
	struct wfs_dentry *dir_block;
	if(disk >= disk_count || disk < -1) {
		perror("Invalid disk\n");
		return(-1);
	}
	if(raid_mode == 0) {
		// Raid 0 Case
		int disk = block_index % disk_count;
		int index = block_index / disk_count;

		if(!block_exists((struct wfs_sb*)regions[disk], index)) {
			perror("Block was not allocated\n");
			return -ENOENT;
		}
		dir_block = (struct wfs_dentry *)((char *)regions[disk] + superblock->d_blocks_ptr + (index * BLOCK_SIZE));
	} else if(raid_mode == 1 || disk != -1) {
		// Raid 1 Case

		// All disks are identical so just take from disk 0
		if(!block_exists((struct wfs_sb*)regions[disk], block_index)) {
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

struct wfs_dentry *find_dentry(struct wfs_inode *dir_inode, const char *name, off_t *blocknumber, void **blockptr) {
	off_t *block_indicies = dir_inode->blocks;
	struct wfs_dentry *curr_dentry;

	// Search each data block
	for(int i = 0; i < N_BLOCKS; i++) {
		if(block_indicies[i] == 0) continue;
		if((curr_dentry = (struct wfs_dentry *) get_block(block_indicies[i], -1)) <= 0) return curr_dentry;
		// Search each dentry in data block
		for(int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
			if(strcmp(curr_dentry[j].name, name) == 0) {
				*blocknumber = block_indicies[i];
				*blockptr = curr_dentry;
				return &curr_dentry[j];
			}
		}
	}
	// No matching dentry found
	return -ENOENT;
}

int alloc_dentry(struct wfs_inode* dir_inode, int num, char* name) {
	struct wfs_dentry *curr_dentry;

	// find free block
    for (int i = 0; i < D_BLOCK; i++) {
        if (dir_inode->blocks[i] == 0) continue;
        if((curr_dentry = (struct wfs_dentry *) get_block(dir_inode->blocks[i], -1)) <= 0) return curr_dentry;

        // find free dentry in this block
        for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
            if (curr_dentry[j].num == 0) {
                curr_dentry[j].num = num;
                strncpy(curr_dentry[j].name, name, MAX_NAME);
                dir_inode->nlinks++; 
                return 0;
            }
        }
    }

    // no free dentry or block found
    for (int i = 0; i < D_BLOCK; i++) {
        if (dir_inode->blocks[i] == 0) { // allocate unallocated block from before
            off_t new_block = allocate_data_block();
            if (new_block < 0) return new_block;

            dir_inode->blocks[i] = new_block;

            // initialize entries
			if((curr_dentry = (struct wfs_dentry *) get_block(dir_inode->blocks[i], -1)) <= 0) return curr_dentry;
            memset(curr_dentry, 0, BLOCK_SIZE);
            
			curr_dentry[0].num = num;
            strncpy(curr_dentry[0].name, name, MAX_NAME);
            dir_inode->nlinks++;
            dir_inode->size += BLOCK_SIZE;

            return 0;
        }
    }

    return -ENOSPC;
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
		off_t blocknumber;
		void *blockptr;
		if((dentry = find_dentry(curr_inode, tok, &blocknumber, &blockptr)) <= 0) {
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

int update_all_inodes(struct wfs_inode *inode) {
	struct wfs_inode *toreplace;
	for(int i = 0; i < disk_count; i++) {
		if((toreplace = get_inode(inode->num, i)) <= 0) return toreplace;
		if(memcpy(toreplace, inode, BLOCK_SIZE) <= 0) {
			perror("memcpy failed\n");
			return -1;
		}
	}
	return 0;
}

int update_all_datablocks(off_t index, void *block) {
	if(raid_mode >= 1) {
		for(int i = 0; i < disk_count; i++) {
			if((memcpy((char *)regions[i] + superblock->d_blocks_ptr + index * BLOCK_SIZE, block, BLOCK_SIZE)) <= 0) {
				perror("memcpy failed\n");
				return -1;
			}
		}
	}
	return 0;
}

off_t get_datablock_index_from_inode_block_index(int i, off_t *blocks) {
	if(i >= 0 || i <= D_BLOCK) {
		if(blocks[i] == 0) return -1;
		return blocks[i];
	} else if (i <= (BLOCK_SIZE / sizeof(off_t)) + D_BLOCK){
		off_t *block;
		if(blocks[IND_BLOCK] == 0) return -1;
		if((block = get_block(blocks[IND_BLOCK], -1)) <= 0) return block;
		if(block[i - (D_BLOCK + 1)] == 0) return -1;
		return block[i - (D_BLOCK + 1)];
	}
	perror("Block index out of range\n");
	return -1;
}

int unlink_(struct wfs_inode *parent, char *filename) {
	if(!I_IFDIR(parent->mode)) {
		perror("Trying to unlink from non-directory\n");
		return -1;
	}

	struct wfs_dentry *dentry;
	void *blockptr;
	off_t blocknumber;
	if((dentry = find_dentry(parent, filename, &blocknumber, &blockptr)) <= 0) {
		return dentry;
	}

	struct wfs_inode *inode;
	if((inode = get_inode(dentry->num, 0)) <= 0) {
		return inode;
	}
    if(S_IFDIR(inode->mode)) {
        return -1;
    }

	inode->nlinks--;
	if(inode->nlinks <= 0) {
		free_inode(dentry->num);
	}
	dentry->num = 0;

	update_all_inodes(inode);
	update_all_datablocks(blocknumber, blockptr);
	return 0;
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
    (void)rdev;
	printf("wfs_mknod, path: (%s)\n", path);

	char *path_copy1 = strdup(path);
    char *path_copy2 = strdup(path);
    if (!path_copy1 || !path_copy2) {
        free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
    }

    char *parent_path = dirname(path_copy1);
    char *entry_name = basename(path_copy2);

    struct wfs_inode* parent = get_inode_from_path(parent_path);
    if (parent < 0) {
		free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
	}

	off_t blk = allocate_inode(S_IFREG | mode);
	if (blk < 0) {
		free(path_copy1);
        free(path_copy2);
		return -ENOENT;
	}

    if (alloc_dentry(parent, blk, entry_name) < 0) return -ENOSPC;
	
	return 0;

}

static int wfs_mkdir(const char* path, mode_t mode) {
    printf("wfs_mkdir, path: (%s)\n", path);

	char *path_copy1 = strdup(path);
    char *path_copy2 = strdup(path);
    if (!path_copy1 || !path_copy2) {
        free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
    }

    char *parent_path = dirname(path_copy1);
    char *entry_name = basename(path_copy2);

	struct wfs_inode* parent = get_inode_from_path(parent_path);
    if (parent < 0) {
		free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
	}

    off_t blk = allocate_inode(S_IFDIR | mode);
	if (blk < 0) {
		free(path_copy1);
        free(path_copy2);
		return -ENOENT;
	}

    if (alloc_dentry(parent, blk, entry_name) < 0) return -ENOSPC;
	
	return 0;

}

static int wfs_unlink(const char* path) {
    struct wfs_inode *inode_to_unlink;
	if((inode_to_unlink = get_inode_from_path(path)) <= 0) {
		return inode_to_unlink;
	}

	int last_slash_index = -1;
	for(int i = 0; i < strlen(path); i++) {
		if(path[i] == '/') last_slash_index = i;
	}

	if(last_slash_index <= 0) {
		perror("No parent in path\n");
		return -1;
	}

	char *parent_path = strdup(path);
	parent_path[last_slash_index] = '\0';

	struct wfs_inode *parent;
	if((parent = get_inode_from_path(parent_path)) <= 0) {
		return parent;
	}

	if(last_slash_index == (strlen(path) - 1)) {
		perror("No filename specified\n");
		return -ENOENT;
	}
	char *child_filename = path[last_slash_index + 1];

	unlink_(parent, child_filename);
	free(parent_path);
	
	return 0;
}

static int wfs_rmdir(const char* path) {
	printf("wfs_rmdir, path: %s\n", path);

	if (strcmp(path, "/") == 0) return -EPERM;

    char *path_copy1 = strdup(path);
    char *path_copy2 = strdup(path);
    if (!path_copy1 || !path_copy2) {
        free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
    }
    char *parent_path = dirname(path_copy1);
    char *entry_name = basename(path_copy2);

    struct wfs_inode* parent = get_inode_from_path(parent_path);
    if (parent < 0) {
		free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
	}

    struct wfs_inode* rem_dir = get_inode_from_path(path);
    if (rem_dir < 0) {
		free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
	}

	// check if actually a directory
    if ((rem_dir->mode & S_IFDIR) == 0) return -ENOTDIR;

	// make sure directory empty
	struct wfs_dentry *curr_dentry;
    for (int i = 0; i < N_BLOCKS; i++) {
        if (rem_dir->blocks[i] != 0 && rem_dir->blocks[i] != -1) {
            if ((curr_dentry = (struct wfs_dentry*) get_block(rem_dir->blocks[i], -1)) < 0) return curr_dentry;
            
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
                if (block[j].num != 0) return -ENOTEMPTY; // dentry found, directory not empty
        }
    }

	int blk_index; 
	void *blk_ptr;

	struct wfs_dentry *entry = NULL;
	struct wfs_dentry pblock;

	for (int i = 0; i < N_BLOCKS && entry == NULL; i++) {
		if (parent->blocks[i] == -1 || parent->blocks[i] == 0) continue;
		
		if ((pblock = (struct wfs_dentry*) get_block(parent->blocks[i], -1)) < 0) return pblock;

		for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
			if (pblock[j].num != 0 && strcmp(pblock[j].name, entry_name) == 0) {
				pblock[j].num = 0;
				blk_index = parent->blocks[i];
				blk_ptr = pblock;
				entry = &pblock[j];
				break;
			}
		}
	}
	if (!entry) return -ENOENT; // no block was removed

	// update meta data

	// free directory inode + all blocks

    return 0;

}

static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    (void)fi;
	struct wfs_inode *inode;
	if((inode = get_inode_from_path(path)) <= 0) {
		return inode;
	}

	size_t curr_position = offset;
	size_t read = 0;
	size_t to_read;

	int curr_block_index;
	char *curr_block;
	while((read < size) && (curr_position < inode->size)) {
		// Retrieve block to read from
		// get_block handles all raid 1v block selection on a per block basis if disk argument is -1
		curr_block_index = get_datablock_index_from_inode_block_index(curr_position / BLOCK_SIZE, inode->blocks);
		if(curr_block_index <= 0) {
			perror("Inode does not contain this block\n");
			return -ENOENT;
		}
		if((curr_block = get_block(curr_block_index, -1)) <= 0) return curr_block;

		// Calcuate size to read
		to_read = BLOCK_SIZE - (curr_position % BLOCK_SIZE);
		if(inode->size - curr_position < to_read) to_read = inode->size - curr_position;

		// Perform Read Operation
		if(memcpy(buf + read, curr_block + (curr_position % BLOCK_SIZE), to_read) <= 0) {
			perror("memcpy failed\n");
			return -1;
		}
		
		// Update indexing variables
		read += to_read;
		curr_position += to_read;
	}

	return read;
}

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
	(void)fi;
	struct wfs_inode *inode;
	if((inode = get_inode_from_path(path)) <= 0) {
		return inode;
	}

	size_t written = 0;
	size_t to_write;
	size_t curr_position = offset;

	int curr_block_index;
	char *curr_block;
	while(written < size) {
		// Find index in data block array of inode
		curr_block_index = get_datablock_index_from_inode_block_index(curr_position / BLOCK_SIZE, inode->blocks);

		// Make sure there is an existing entry, alloc if not
		if(curr_block_index == -ENOENT) {
			if((curr_block_index = allocate_block()) <= 0) return curr_block_index;
			inode->blocks[curr_position / BLOCK_SIZE] = curr_block_index;
			inode->size += BLOCK_SIZE;
			if(update_all_inodes(inode) < 0) return -1;
		} else if(curr_block_index <= 0) {
			return -ENOENT;
		}

		// Calculate size to write
		to_write = BLOCK_SIZE - (curr_position % BLOCK_SIZE);
		if(size - written < to_write) to_write = size - written;
		

		// Retrieve corresponding data block in memory
		if((curr_block = get_block(curr_block_index, 0)) <= 0) return curr_block;

		// Perform write Operation
		if(memcpy(curr_block + (curr_position % BLOCK_SIZE), buf + written, to_write) <= 0) {
			perror("memcpy failed\n");
			return -1;
		}

		update_all_datablocks(curr_block_index, (void*)curr_block);

		// Update indexing variables
		written += to_write;
		curr_position += to_write;
	}

    return written;
}

static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	struct wfs_inode *inode;
	if((inode = get_inode_from_path(path)) <= 0 || !S_IFDIR(inode->mode)) {
		return -ENOENT;
	}

	struct wfs_dentry *block;
	for(int i = 0; i < N_BLOCKS; i++) {
		if(inode->blocks[i] != 0) {
			if((block = get_block(inode->blocks[i], -1)) <= 0) {
				return block;
			}

			for(int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
				if(block[j].num != 0) {
					filler(buf, block[j].name, NULL, 0);
				}
			}
		}
	}
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
