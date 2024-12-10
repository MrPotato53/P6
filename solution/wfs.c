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
#include <time.h>
#include "wfs.h"


int disk_count;
void *regions[MAX_DISK];
int raid_mode = -1;
struct wfs_sb *superblock;
void *metadata;

// No Return
void update_metadata() {
	// Keep metadata consistent across all disks
	for(int i = 0; i < disk_count; i++) {
		// Write inode bitmap into memory
		memcpy((char *)regions[i] + superblock->i_bitmap_ptr, (char *)metadata + superblock->i_bitmap_ptr, superblock->d_bitmap_ptr - superblock->i_bitmap_ptr);
		if(raid_mode >= 1) {
			// Only copy blocks bitmap if using raid 1 or 1v
			memcpy((char *)regions[i] + superblock->d_bitmap_ptr, (char *)metadata + superblock->d_bitmap_ptr, superblock->i_blocks_ptr - superblock->d_bitmap_ptr);
		}
		// Write inode blocks into memory
		memcpy((char *)regions[i] + superblock->i_blocks_ptr, (char *)metadata + superblock->i_blocks_ptr, superblock->num_inodes * BLOCK_SIZE);
	}
}

// No Return
void update_all_datablocks(off_t index, void *block) {
	if(raid_mode >= 1) {
		for(int i = 0; i < disk_count; i++) {
			memcpy((char *)regions[i] + superblock->d_blocks_ptr + index * BLOCK_SIZE, block, BLOCK_SIZE);
		}
	}
}

// Return NULL if fail
struct wfs_inode* get_inode(int n) {
	uint8_t* bitmap = (uint8_t*)((char*)metadata + superblock->i_bitmap_ptr);

	if (bitmap[n / 8] & (1 << n % 8))
		return (struct wfs_inode*)(((char*)metadata + superblock->i_blocks_ptr) + n * BLOCK_SIZE);

	return NULL;
}

int block_exists(off_t block_index) {
	int8_t *b_bitmap;
	if(raid_mode == 0) {
		b_bitmap = (int8_t*)((char *)regions[block_index % disk_count] + superblock->d_bitmap_ptr);
	} else {
		b_bitmap = (int8_t*)((char *)metadata + superblock->d_bitmap_ptr);
	}
	return (int)(b_bitmap[block_index / 8]) & 0x1 << (block_index % 8);
}

// Return NULL if fail
void *get_block(off_t block_index) {
	if(!block_exists(block_index)) {
		perror("Block was not allocated\n");
		return NULL;
	}

	if(raid_mode == 0) {
		// Raid 0 Case
		int disk = block_index % disk_count;
		int index = block_index / disk_count;
		
		return (void *)((char *)regions[disk] + superblock->d_blocks_ptr + (index * BLOCK_SIZE));
	} else if(raid_mode == 1) {
		// Raid 1 Case
		return (void *)((char *)regions[0] + superblock->d_blocks_ptr + (block_index * BLOCK_SIZE));
	} else if(raid_mode == 2) {
		// Raid 1v Case
		int block_votes[MAX_DISK] = {0}; // number of matches each block has
		void *blocks[MAX_DISK] = {0};
		int max_votes = 0, majority_index = 0;

		for (int i = 0; i < disk_count; i++) {
			blocks[i] = (char *)regions[i] + superblock->d_blocks_ptr + block_index * BLOCK_SIZE;
		}

		// comparing blocks on each disk with blocks on all other disks
		for (int i = 0; i < disk_count; i++) {
			for (int j = 0; j < disk_count; j++)
				if (memcmp(blocks[i], blocks[j], BLOCK_SIZE) == 0) block_votes[i]++; // indentical blocks
			
			// update majority block
			if (block_votes[i] > max_votes || (block_votes[i] == max_votes && i < majority_index)) {
				max_votes = block_votes[i];
				majority_index = i;
			}
		}
		// update block with most matches
		return (void *)blocks[majority_index];
	}
	return NULL;
}

// No Return
void free_block(off_t blk) {
	void *disk;

	// Update specific disk if raid 0, otherwise update metadata and therefore all disks
	if(raid_mode == 0) 
		disk = regions[blk % disk_count];	
	else 
		disk = metadata;

	uint8_t* bitmap = (uint8_t*)((char*)disk + superblock->d_bitmap_ptr);
	bitmap[blk / 8] &= ~(1 << (blk % 8));
	update_metadata();
}

// No Return
void free_inode(int index) {

	struct wfs_inode *inode = get_inode(index);

	if (inode == NULL) {
		perror("no inode with given index");
		return;
	}

	// Free all direct blocks
	for(int i = 0; i <= D_BLOCK; i++) {
		if(inode->blocks[i] > -1) {
			free_block(inode->blocks[i]);
		}
	}

	// Free indirect block and inner blocks
	if(inode->blocks[IND_BLOCK] > -1) {
		off_t *ind_block = (off_t *)get_block(inode->blocks[IND_BLOCK]);
		if(ind_block == NULL) return;

		for(int i = 0; i <= BLOCK_SIZE / sizeof(off_t); i++) {
			if(ind_block[i] > -1) {
				free_block(ind_block[i]);
			}
		}
		free_block(inode->blocks[IND_BLOCK]);
	}

	uint8_t* bitmap = (uint8_t*)((char*)metadata + superblock->i_bitmap_ptr);
	bitmap[index / 8] &= ~(1 << (index % 8));
	update_metadata();
}

// Return -ENOSPC if fail
off_t allocate_block() {
	uint8_t* bitmap;
	off_t blk = -1;
	// For each possible block
	for(int i = 0; i < superblock->num_data_blocks; i++) {
		// Check bitmap for availability
		if(raid_mode == 0) {
			int disk = i % disk_count;
			bitmap = (uint8_t*)((char*)regions[disk] + superblock->d_bitmap_ptr);
		} else {
			bitmap = (uint8_t*)((char*)metadata + superblock->d_bitmap_ptr);
		}
		
		// Set bit to 1 if 0
		if (!(bitmap[i / 8] & (1 << i % 8))) {
			bitmap[i / 8] |= (1 << i % 8);
			blk = i;
			break;
		}
	}

	if(blk == -1) return -ENOSPC;

	update_metadata();
	// Clear data in new block
	void *newblock = get_block(blk);
	memset(newblock, 0, BLOCK_SIZE);
	update_all_datablocks(blk, newblock);
	return blk;
}

// Return -ENOSPC if fail
off_t allocate_inode(mode_t mode) {
	uint8_t* bitmap = (uint8_t*)((char*)metadata + superblock->i_bitmap_ptr);

	off_t blk = -1;
	for (uint32_t i = 0; i < superblock->num_inodes / 8; i++) {
		if (bitmap[i] == 0xFF)
			continue;

		for (uint32_t k = 0; k < 8; k++) {
			if (!(bitmap[i] & (1 << k))) {
				// allocate inodes on all disks
				bitmap[i] |= (1 << k);
				blk =  8 * i + k;
				goto found;
			}
		}
	}

	found:
	if (blk < 0) return -ENOSPC;
	
	// Fill inode with initial information
	struct wfs_inode* inode = (struct wfs_inode*)((char*)metadata + superblock->i_blocks_ptr + BLOCK_SIZE * blk);
	for(int i = 0; i < N_BLOCKS; i++) {
		inode->blocks[i] = -1;
	}
	inode->num = blk;
	inode->mode = mode;
	inode->uid = getuid();
	inode->gid = getgid();
	inode->size = 0;
	inode->nlinks = 0;
	time_t t;
	time(&t);
	inode->ctim = t;
	inode->atim = t;
	inode->mtim = t;
	update_metadata();
	return blk;
}



// Return -1 if fail
off_t get_datablock_index_from_inode(int i, off_t *blocks) {
	if (i < 0) return -1;
	if(i <= D_BLOCK) {
		return blocks[i];
	} else if(i <= D_BLOCK + BLOCK_SIZE / sizeof(off_t)) {
		if(blocks[IND_BLOCK] == -1) return -1;
		off_t *block = (off_t *)get_block(blocks[IND_BLOCK]);
		if(block == NULL) return -1;

		return block[i - (D_BLOCK + 1)];
	} else return -1;
}

// Return NULL if fail
struct wfs_dentry *find_dentry(struct wfs_inode *dir_inode, const char *name, off_t *blocknumber, void **blockptr) {
	// Make sure it's a directory
	if(!(S_IFDIR & dir_inode->mode)) {
		printf("Parent is not directory\n");
		return NULL;
	}

	printf("Searching for %s\n", name);

	off_t *block_indicies = dir_inode->blocks;
	struct wfs_dentry *curr_dentry;

	// Search each data block
	for(int i = 0; i < N_BLOCKS; i++) {
		if(block_indicies[i] == -1) continue;

		curr_dentry = (struct wfs_dentry *) get_block(block_indicies[i]);
		if(curr_dentry != NULL) {
			// Search each dentry in data block
			for(int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
				if(curr_dentry[j].num != 0 && strcmp(curr_dentry[j].name, name) == 0) {
					*blocknumber = block_indicies[i];
					*blockptr = (void*) curr_dentry;
					return &curr_dentry[j];
				}
			}
		}
	}
	// No matching dentry found
	printf("No matching dentry found\n");
	return NULL;
}

// Return -1 if block failure, -ENOSPC if space failure
int alloc_dentry(struct wfs_inode* dir_inode, int num, char* name) {
	struct wfs_dentry *curr_dentry;

	// find free block
	for (int i = 0; i < D_BLOCK; i++) {
		if (dir_inode->blocks[i] == -1) continue;
		if((curr_dentry = (struct wfs_dentry *) get_block(dir_inode->blocks[i])) == NULL) return -1;

		// find free dentry in this block
		for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++) {
			if (curr_dentry[j].num == 0) {
				curr_dentry[j].num = num;
				strncpy(curr_dentry[j].name, name, strlen(name));
				dir_inode->nlinks++; 
				update_all_datablocks(dir_inode->blocks[i], curr_dentry);
				return 0;
			}
		}
	}

	// no free dentry or block found
	for (int i = 0; i < D_BLOCK; i++) {
		if (dir_inode->blocks[i] == -1) { // allocate unallocated block from before
			off_t new_block = allocate_block();
			if (new_block < 0) return -ENOSPC;

			dir_inode->blocks[i] = new_block;

			// initialize entries
			if((curr_dentry = (struct wfs_dentry *) get_block(dir_inode->blocks[i])) == NULL) return -1;
			
			curr_dentry[0].num = num;
			strncpy(curr_dentry[0].name, name, MAX_NAME);
			dir_inode->nlinks++;
			dir_inode->size += BLOCK_SIZE;
			update_all_datablocks(dir_inode->blocks[i], curr_dentry);
			return 0;
		}
	}

	return -ENOSPC;
}

// Return NULL if fail
struct wfs_inode *get_inode_from_path(const char *path) {

	struct wfs_inode *curr_inode = get_inode(0);

	if(strcmp(path, "/") == 0) {
		printf("root node: %p", (void *)curr_inode);
		return curr_inode;
	}

	char *path_copy = strdup(path);
	if(path_copy == NULL) {
		perror("strdup failure\n");
		return NULL;
	}
	char *rem_path = path_copy;
	char *tok = strtok(rem_path, "/");
	struct wfs_dentry *dentry;
	while(tok) {
		// Check if curr inode is directory
		if(!(S_IFDIR & curr_inode->mode)) {
			free(path_copy);
			printf("Not a directory\n");
			return NULL;
		}

		// Find dentry in current inode and corresponding next inode
		off_t blocknumber;
		void *blockptr;
		if((dentry = find_dentry(curr_inode, tok, &blocknumber, &blockptr)) == NULL) {
			free(path_copy);
			printf("Find dentry failed \n");
			return NULL;
		}
		(void)blocknumber;
		(void)blockptr;

		if((curr_inode = get_inode(dentry->num)) == NULL) {
			free(path_copy);
			printf("Get inode failed\n");
			return NULL;
		}

		tok = strtok(NULL, "/");
	}

	free(path_copy);
	return curr_inode;
}

// Returns -ENOENT if fail
int unlink_(struct wfs_inode *parent, char *filename) {
	if(!(S_IFDIR & parent->mode)) {
		perror("Trying to unlink from non-directory\n");
		return -ENOENT;
	}

	struct wfs_dentry *dentry;
	void *blockptr;
	off_t blocknumber;
	if((dentry = find_dentry(parent, filename, &blocknumber, &blockptr)) == NULL) {
		return -ENOENT;
	}

	struct wfs_inode *inode;
	if((inode = get_inode(dentry->num)) == NULL) {
		return -ENOENT;
	}

	if(S_IFDIR & inode->mode) {
		// ENOENT because expecting file, not directory
		return -ENOENT;
	}

	inode->nlinks--;
	if(inode->nlinks <= 0) {
		free_inode(dentry->num);
	}
	dentry->num = 0;

	update_metadata();
	update_all_datablocks(blocknumber, blockptr);
	return 0;
}

// Returns -ENOENT if fail
int separate_paths(char *path_copy1, char *path_copy2, char **parent_path, char **entry_name) {

	int last_slash_index = -1;
	for(int i = 0; i < strlen(path_copy1); i++) {
		if(path_copy1[i] == '/') last_slash_index = i;
	}

	if(last_slash_index < 0 || last_slash_index == (strlen(path_copy1) - 1)) {
		perror("Either no parent or child in path\n");
		return -ENOENT;
	}

	*parent_path = path_copy1;
	(*parent_path)[last_slash_index + 1] = '\0';
	*entry_name = path_copy2 + (last_slash_index + 1);
	return 0;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
	struct wfs_inode *inode;
	if((inode = get_inode_from_path(path)) == NULL) {
		return -ENOENT;
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

	// Make sure it doesn't exist
	if(get_inode_from_path(path) != NULL) {
		return -EEXIST;
	}

	char *path_copy1 = strdup(path);
	char *path_copy2 = strdup(path);
	if (!path_copy1 || !path_copy2) {
		free(path_copy1);
		free(path_copy2);
		return -ENOMEM;
	}

	// obtain parent path and created file name
	char *parent_path;
	char *entry_name;
	if(separate_paths(path_copy1, path_copy2, &parent_path, &entry_name) < 0) {
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}

	struct wfs_inode* parent = get_inode_from_path(parent_path);
	if (parent == NULL) {
		free(path_copy1);
		free(path_copy2);
		return -ENOMEM;
	}

	if((parent->mode & S_IFDIR) == 0) {
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}

	off_t blk = allocate_inode(mode);
	if (blk < 0) {
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}

	free(path_copy1);
	if (alloc_dentry(parent, blk, entry_name) < 0){
		int8_t *bitmap = (int8_t *)((char *)metadata + superblock->i_bitmap_ptr);
		bitmap[blk / 8] &= 1 << blk % 8;
		update_metadata();
		free(path_copy2);
		return -ENOSPC;
	} 
	free(path_copy2);
	get_inode(blk)->nlinks++;
	update_metadata();
	
	return 0;

}

static int wfs_mkdir(const char* path, mode_t mode) {
	printf("wfs_mkdir, path: (%s)\n", path);

	// Make sure it doesn't exist
	if(get_inode_from_path(path) != NULL) {
		return -EEXIST;
	}

	char *path_copy1 = strdup(path);
	char *path_copy2 = strdup(path);
	if (!path_copy1 || !path_copy2) {
		free(path_copy1);
		free(path_copy2);
		return -ENOMEM;
	}

	// obtain parent path and created directory name
	char *parent_path;
	char *entry_name;
	if(separate_paths(path_copy1, path_copy2, &parent_path, &entry_name) < 0) {
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}

	struct wfs_inode* parent = get_inode_from_path(parent_path);
	if (parent == NULL) {
		free(path_copy1);
		free(path_copy2);
		printf("Failed Here\n");
		return -ENOMEM;
	}

	if((parent->mode & S_IFDIR) == 0) {
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}

	off_t blk = allocate_inode(mode | S_IFDIR);
	if (blk < 0) {
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}

	free(path_copy1);
	if (alloc_dentry(parent, blk, entry_name) < 0){
		int8_t *bitmap = (int8_t *)((char *)metadata + superblock->i_bitmap_ptr);
		bitmap[blk / 8] &= 1 << blk % 8;
		update_metadata();
		free(path_copy2);
		return -ENOSPC;
	} 
	free(path_copy2);
	get_inode(blk)->nlinks++;
	update_metadata();
	
	return 0;
}

static int wfs_unlink(const char* path) {
	struct wfs_inode *inode_to_unlink;
	if((inode_to_unlink = get_inode_from_path(path)) == 0) {
		return -ENOENT;
	}

	char *path_copy1 = strdup(path);
	char *path_copy2 = strdup(path);
	if (!path_copy1 || !path_copy2) {
		free(path_copy1);
		free(path_copy2);
		return -ENOMEM;
	}

	// obtain parent path and created directory name
	char *parent_path;
	char *child_filename;
	if(separate_paths(path_copy1, path_copy2, &parent_path, &child_filename) < 0) {
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}

	struct wfs_inode *parent;
	if((parent = get_inode_from_path(parent_path)) == NULL) {
		free(path_copy1);
		free(path_copy2);
		return -ENOENT;
	}

	unlink_(parent, child_filename);
	free(path_copy1);
	free(path_copy2);
	update_metadata();
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

	// obtain parent path and created directory name
	char *parent_path;
	char *entry_name;
	if(separate_paths(path_copy1, path_copy2, &parent_path, &entry_name) < 0) return -ENOENT;

	struct wfs_inode* parent = get_inode_from_path(parent_path);
	if (parent == NULL) {
		free(path_copy1);
		free(path_copy2);
		return -ENOMEM;
	}

	struct wfs_inode* rem_dir = get_inode_from_path(path);
	if (rem_dir == NULL) {
		free(path_copy1);
		free(path_copy2);
		return -ENOMEM;
	}

	free(path_copy1);

	// check if actually a directory
	if (!(S_IFDIR & rem_dir->mode)){
		free(path_copy2);
		return -ENOTDIR;
	}

	// make sure directory empty
	struct wfs_dentry *curr_dentry;
	for (int i = 0; i < N_BLOCKS; i++) {
		if (rem_dir->blocks[i] != -1) {
			if ((curr_dentry = (struct wfs_dentry*) get_block(rem_dir->blocks[i])) == NULL) return -1;
			
			for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
				if (curr_dentry[j].num != 0) {
					free(path_copy2);
					return -ENOTEMPTY; // dentry found, directory not empty
				} 
		}
	}

	off_t blk_index; 
	void *blk_ptr;

	struct wfs_dentry *dentry_to_clear = find_dentry(parent, entry_name, &blk_index, &blk_ptr);
	free(path_copy2);
	if(dentry_to_clear == NULL) return -ENOENT;

	dentry_to_clear->num = 0;
	update_all_datablocks(blk_index, blk_ptr);

	// update parent metadata
	parent->nlinks--;
	parent->size -= sizeof(struct wfs_dentry);

	// update meta data
	update_metadata();
	// free directory inode + all blocks
	free_inode(rem_dir->num);
	return 0;

}

static int wfs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
	(void)fi;
	struct wfs_inode *inode;
	if((inode = get_inode_from_path(path)) == 0) {
		return -ENOENT;
	}

	size_t curr_position = offset;
	size_t read = 0;
	size_t to_read;

	int curr_block_index;
	char *curr_block;
	while((read < size) && (curr_position < inode->size)) {
		// Retrieve block to read from
		// get_block handles all raid 1v block selection on a per block basis if disk argument is -1
		curr_block_index = get_datablock_index_from_inode(curr_position / BLOCK_SIZE, inode->blocks);
		if(curr_block_index <= 0) {
			perror("Inode does not contain this block\n");
			return -ENOENT;
		}
		if((curr_block = get_block(curr_block_index)) == NULL) return -ENOENT;

		// Calcuate size to read
		to_read = BLOCK_SIZE - (curr_position % BLOCK_SIZE);
		if(inode->size - curr_position < to_read) to_read = inode->size - curr_position;

		// Perform Read Operation
		memcpy(buf + read, curr_block + (curr_position % BLOCK_SIZE), to_read);
		
		// Update indexing variables
		read += to_read;
		curr_position += to_read;
	}

	return read;
}

static int wfs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
	(void)fi;
	struct wfs_inode *inode;
	if((inode = get_inode_from_path(path)) == NULL) {
		perror("write:file from path DNE\n");
		return -ENOENT;
	}

	printf("Writing %d to %s\n", (int)size, path);

	size_t written = 0;
	size_t to_write;
	size_t curr_position = offset;

	int curr_block_index;
	char *curr_block;
	while(written < size) {
		// Find index in data block array of inode
		curr_block_index = get_datablock_index_from_inode(curr_position / BLOCK_SIZE, inode->blocks);
		
		printf("Writing to block %d\n", (int)curr_position / BLOCK_SIZE);

		// Make sure there is an existing entry, alloc if not
		if(curr_block_index == -1) {
			printf("Block DNE, allocating\n");
			if((curr_block_index = allocate_block()) < 0) {
				perror("write:Allocate block failed\n");
				return -ENOSPC;
			}

			if(curr_position / BLOCK_SIZE >= 0 && curr_position / BLOCK_SIZE <= D_BLOCK) {
				// If index is one of the D blocks
				inode->blocks[curr_position / BLOCK_SIZE] = curr_block_index;
			} else if(curr_position / BLOCK_SIZE <= D_BLOCK + BLOCK_SIZE / sizeof(off_t)) {
				// If index is in the IND block
				printf("Indirect block\n");

				// Create new ind block if needed
				if(inode->blocks[IND_BLOCK] == -1) {
					printf("Allocating Indirect block\n");
					off_t ind_block;
					if((ind_block = allocate_block()) < 0) return -ENOSPC;
					off_t *block = get_block(ind_block);
					inode->blocks[IND_BLOCK] = ind_block;
					for(int i = 0; i < BLOCK_SIZE / sizeof(off_t); i++) {
						block[i] = -1;
					}
				}

				printf("Inserting block index %d into ind block\n", curr_block_index);
				// Insert block pointer into ind block
				off_t *block = get_block(inode->blocks[IND_BLOCK]);
				if(block == NULL) {
					perror("write:getblock failed\n");
					return -ENOENT;
				}
				block[(curr_position / BLOCK_SIZE) - (D_BLOCK + 1)] = curr_block_index;
				update_all_datablocks(inode->blocks[IND_BLOCK], block);
			} else {
				// Index out of bounds
				return -ENOSPC;
			}
			
			// file size reflects the written data and isn't just increased by block size
			inode->size = (offset + written > inode->size) ? offset + written : inode->size;
			update_metadata();
		}

		// Calculate size to write
		to_write = BLOCK_SIZE - (curr_position % BLOCK_SIZE);
		if(size - written < to_write) to_write = size - written;
		

		// Retrieve corresponding data block in memory
		if((curr_block = get_block(curr_block_index)) == NULL) {
			perror("write:get_block failed 1\n");
			return -ENOENT;
		}

		// Perform write Operation
		memcpy(curr_block + (curr_position % BLOCK_SIZE), buf + written, to_write);

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
	if((inode = get_inode_from_path(path)) == NULL || !(S_IFDIR & inode->mode)) {
		return -ENOENT;
	}

	struct wfs_dentry *block;
	for(int i = 0; i < N_BLOCKS; i++) {
		if(inode->blocks[i] != -1) {
			if((block = get_block(inode->blocks[i])) == NULL) {
				return -ENOENT;
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
  .read	= wfs_read,
  .write   = wfs_write,
  .readdir = wfs_readdir,
};


int main(int argc, char *argv[]) {
	int fd[MAX_DISK];
	void *tmp_region[MAX_DISK];
	struct stat stats;
	int unique_run_id = -1;
	
	// Count disks
	for(int i = 1; i < argc; i++) {
		if(argv[i][0] == '-') break;
		disk_count++;
	}

	if(disk_count < 2) {
		perror("Not enough disks, need at least 2\n");
		exit(1);
	}

	// Open all disks and map them to regions array
	for(int i = 0; i < disk_count; i++) {
		if((fd[i] = open(argv[i + 1], O_RDWR)) <= 0) {
			printf("open failed on %s\n", argv[i + 1]);
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
	metadata = malloc(superblock->d_blocks_ptr);
	if(!metadata) {
		return -ENOMEM;
	}
	memcpy(metadata, regions[0], superblock->d_blocks_ptr);

	for (int i = 1; i < disk_count; i++)
		if (memcmp((char *)regions[0] + superblock->i_bitmap_ptr, (char *)regions[i] + superblock->i_bitmap_ptr, superblock->d_blocks_ptr - superblock->i_bitmap_ptr) != 0) {
			perror("meta data doesn't match across disks\n");
			exit(-1);
		}

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
	free(metadata);

	return fuse_out;
}
