#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include "fs.h"
#include "types.h"
#include <stdbool.h>
#include <string.h>  

#define BLK_SZ (BSIZE) // Define block size
#define CHK_BIT(bmp, addr) ((*(bmp + addr / 8)) & (bits[addr % 8])) // Macro to check if a bit is set in a bitmap

char bits[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 }; // Bitmask for checking individual bits

// Structure to represent an entry in a directory
typedef struct {
  struct dinode *in;
  int nextIdx;
} Entry;

// Structure to hold image data
typedef struct {
  uint ninodeblks;
  uint nbitmapblks;
  uint firstblk;
  struct superblock *sb;
  char *inodeblks;
  char *bitmapblks;
  char *data;
  char *map;
} img_t;

// Function to validate the type of an inode
void validate_type(struct dinode *in) {
  switch (in->type) {
  case T_FILE:
  case T_DIR:
  case T_DEV:
    break;
  default:
    fprintf(stderr, "ERROR: bad inode.\n");
    exit(1);
  }
}

// Function to check if an address is valid within the image
bool valid_addr(img_t *img, uint addr) {
  return addr > 0 && addr < img->sb->size;
}

// Function to check direct addresses in an inode
void check_direct(img_t *img, struct dinode *in) {
  for (int i = 0; i < NDIRECT; i++) {
    uint addr = in->addrs[i];
    if (addr == 0) continue;
    if (!valid_addr(img, addr)) {
      fprintf(stderr, "ERROR: bad direct address in inode.\n");
      exit(1);
    }
  }
}

// Function to check indirect addresses in an inode
void check_indirect(img_t *img, struct dinode *in) {
  uint addr = in->addrs[NDIRECT];
  if (addr == 0) return;
  if (!valid_addr(img, addr)) {
    fprintf(stderr, "ERROR: bad indirect address in inode.\n");
    exit(1);
  }

  uint indirect_addrs[NINDIRECT];
  memcpy(indirect_addrs, img->map + addr * BLK_SZ, NINDIRECT * sizeof(uint));

  for (int i = 0; i < NINDIRECT; i++) {
    addr = indirect_addrs[i];
    if (addr == 0) continue;
    if (!valid_addr(img, addr)) {
      fprintf(stderr, "ERROR: bad indirect address in inode.\n");
      exit(1);
    }
  }
}

// Function to process directory entries, checking for '.' and '..'
bool process_entries(img_t *img, uint addr, int inum, bool *dot, bool *ddot) {
  struct dirent *de = (struct dirent *)(img->map + addr * BLK_SZ);
  for (int j = 0; j < DPB; j++, de++) {
    if (strcmp(".", de->name) == 0) {
      *dot = true;
      if (de->inum != inum) {
	fprintf(stderr, "ERROR: directory not properly formatted.\n");
	exit(1);
      }
    } else if (strcmp("..", de->name) == 0) {
      *ddot = true;
      if ((inum != 1 && de->inum == inum) || (inum == 1 && de->inum != inum)) {
	fprintf(stderr, "ERROR: root directory does not exist.\n");
	exit(1);
      }
    }
  }
  return *dot && *ddot;
}

// Function to validate a directory inode
void validate_dir(img_t *img, struct dinode *in, int inum) {
  bool dot = false, ddot = false;
  for (int i = 0; i < NDIRECT; i++) {
    uint addr = in->addrs[i];
    if (addr == 0) continue;
    if (process_entries(img, addr, inum, &dot, &ddot)) break;
  }
  if (!dot || !ddot) {
    fprintf(stderr, "ERROR: directory not properly formatted.\n");
    exit(1);
  }
}

// Function to check if an address is marked in the bitmap
bool marked_in_bmp(char *bmp, uint addr) {
  return CHK_BIT(bmp, addr);
}

// Function to check indirect addresses and mark them as checked
void chk_indirect(img_t *img, uint *ind, bool *chkd) {
  for (int j = 0; j < NINDIRECT; j++, ind++) {
    uint addr = *ind;
    if (addr == 0 || chkd[addr]) continue;
    if (!marked_in_bmp(img->bitmapblks, addr)) {
      fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
      exit(1);
    }
    chkd[addr] = true;
  }
}

// Function to check addresses in the bitmap for an inode
void chk_bmp_addr(img_t *img, struct dinode *in) {
  bool chkd[img->sb->size];
  memset(chkd, 0, sizeof(chkd));

  for (int i = 0; i <= NDIRECT; i++) {
    uint addr = in->addrs[i];
    if (addr == 0 || chkd[addr]) continue;
    if (!marked_in_bmp(img->bitmapblks, addr)) {
      fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
      exit(1);
    }
    chkd[addr] = true;
    if (i == NDIRECT) {
      uint *ind = (uint*)(img->map + addr * BLK_SZ);
      chk_indirect(img, ind, chkd);
    }
  }
}

// Mark a block as used in the used_blocks array
void mark_used(int *used_blks, uint addr, uint startblk) {
  if (addr != 0) {
    used_blks[addr - startblk] = 1;
  }
}

// Mark all blocks used by an inode
void get_used(img_t *img, struct dinode *in, int *used_blks) {
  // Mark all direct blocks
  for (int i = 0; i < NDIRECT; i++) {
    mark_used(used_blks, in->addrs[i], img->firstblk);
  }

  // Mark all indirect blocks
  uint indaddr = in->addrs[NDIRECT];
  if (indaddr != 0) {
    uint *indirect = (uint *)(img->map + indaddr * BLK_SZ);
    for (int j = 0; j < NINDIRECT; j++) {
      mark_used(used_blks, indirect[j], img->firstblk);
    }
  }
}

// Mark all blocks used by all inodes
void mark_used_blks(img_t *img, int *used_blks) {
  struct dinode *in = (struct dinode*)(img->inodeblks);
  for (int i = 0; i < img->sb->ninodes; i++, in++) {
    if (in->type != 0) {
      get_used(img, in, used_blks);
    }
  }
}

// Check if block is marked in bitmap and update check list
void chk_mark_bmp(char *bmp, uint addr, bool *chklist) {
  if (!CHK_BIT(bmp, addr)) {
    fprintf(stderr, "ERROR: bitmap marks block in use but it is not in use.\n");
    exit(1);
  }
  chklist[addr] = true;
}

// Verify bitmap consistency with inode addresses
void bmp_chk(img_t *img) {
  bool chklist[img->sb->size];
  memset(chklist, 0, sizeof(chklist));

  struct dinode *in = (struct dinode*)(img->inodeblks);
  for (int i = 0; i < img->sb->ninodes; i++, in++) {
    if (in->type == 0) continue;

    // Check direct addresses
    for (int j = 0; j < NDIRECT; j++) {
      if (in->addrs[j] != 0) {
        chk_mark_bmp(img->bitmapblks, in->addrs[j], chklist);
      }
    }

    // Check indirect addresses
    uint indaddr = in->addrs[NDIRECT];
    if (indaddr != 0) {
      uint *indirect = (uint *)(img->map + indaddr * BLK_SZ);
      for (int j = 0; j < NINDIRECT; j++) {
        if (indirect[j] != 0) {
          chk_mark_bmp(img->bitmapblks, indirect[j], chklist);
        }
      }
    }
  }

  // Final check for unused blocks marked as used
  for (uint i = 0; i < img->sb->size; i++) {
    if (CHK_BIT(img->bitmapblks, i) && !chklist[i]) {
      // Error handling
    }
  }
}

// Count the usage of direct block addresses
void fill_direct(img_t *img, struct dinode *in, uint *dcounts) {
  for (int i = 0; i < NDIRECT; i++) {
    uint addr = in->addrs[i];
    if (addr == 0) continue;
    if (addr < img->firstblk || addr >= img->sb->size) {
      fprintf(stderr, "ERROR: direct block address out of bounds.\n");
      exit(1);
    }
    dcounts[addr - img->firstblk]++;
  }
}

// Count the usage of indirect block addresses
void fill_indirect(img_t *img, struct dinode *in, uint *icounts) {
  uint indaddr = in->addrs[NDIRECT];
  if (indaddr == 0) return;
  if (indaddr < img->firstblk || indaddr >= img->sb->size) {
    fprintf(stderr, "ERROR: indirect block address out of bounds.\n");
    exit(1);
  }

  uint *indirect = (uint *)(img->map + indaddr * BLK_SZ);
  for (int i = 0; i < NINDIRECT; i++, indirect++) {
    uint addr = *indirect;
    if (addr == 0) continue;
    if (addr < img->firstblk || addr >= img->sb->size) {
      fprintf(stderr, "ERROR: indirect block address out of bounds.\n");
      exit(1);
    }
    icounts[addr - img->firstblk]++;
  }
}

// Check the usage of block addresses for duplicates
void blk_usage_chk(img_t *img, uint *usage_counts, uint startblk, const char* type, uint addr) {
  if (addr == 0) return;
  uint idx = addr - startblk;
  usage_counts[idx]++;
  if (usage_counts[idx] > 1) {
    fprintf(stderr, "ERROR: %s address used more than once.\n", type);
    exit(1);
  }
}

// Validate inode addresses for proper usage
void addrs_chk(img_t *img) {
  uint usage_counts[img->sb->nblocks];
  memset(usage_counts, 0, sizeof(usage_counts));

  struct dinode *in = (struct dinode*)(img->inodeblks);
  for (int i = 0; i < img->sb->ninodes; i++, in++) {
    if (in->type == 0) continue;

    // Check direct block addresses
    for (int j = 0; j < NDIRECT; j++) {
      blk_usage_chk(img, usage_counts, img->firstblk, "direct", in->addrs[j]);
    }

    // Check indirect block addresses
    uint indaddr = in->addrs[NDIRECT];
    if (indaddr != 0) {
      uint *indirect = (uint *)(img->map + indaddr * BLK_SZ);
      for (int j = 0; j < NINDIRECT; j++) {
        blk_usage_chk(img, usage_counts, img->firstblk, "indirect", indirect[j]);
      }
    }
  }
}

// Traverse directories and increment inode map for each entry
void traverse_dirs(img_t *img, struct dinode *dir_inode, int *inodemap) {
    if (dir_inode->type != T_DIR) {
        // Exit if not a directory
        return;
    }

    // Helper function to process a block of directory entries
    void process_block(uint addr) {
        if (addr == 0) return;

        struct dirent *de = (struct dirent *)(img->map + addr * BLK_SZ);
        for (int j = 0; j < DPB; j++, de++) {
            if (de->inum == 0 || strcmp(de->name, ".") == 0 || strcmp(de->name, "..") == 0) continue;

            inodemap[de->inum]++;
            struct dinode *next_inode = ((struct dinode *)(img->inodeblks)) + de->inum;
            traverse_dirs(img, next_inode, inodemap);
        }
    }

    // Process direct addresses
    for (int i = 0; i < NDIRECT; i++) {
        process_block(dir_inode->addrs[i]);
    }

    // Process indirect addresses
    uint addr = dir_inode->addrs[NDIRECT];
    if (addr != 0) {
        uint *indirect = (uint *)(img->map + addr * BLK_SZ);
        for (int i = 0; i < NINDIRECT; i++, indirect++) {
            process_block(*indirect);
        }
    }
}

// Check if an inode marked as used is actually in use
void chk_in_use(struct dinode *in, int idx, int *inmap) {
  if (in->type != 0 && inmap[idx] == 0) {
    fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
    exit(1);
  }
}

// Check if an inode referred to in a directory is marked as free
void chk_in_free(struct dinode *in, int idx, int *inmap) {
  if (inmap[idx] > 0 && in->type == 0) {
    fprintf(stderr, "ERROR: inode referred to in directory but marked free.\n");
    exit(1);
  }
}

// Check if the reference count of a file inode matches the directory entries
void chk_ref_cnt(struct dinode *in, int idx, int *inmap) {
  if (in->type == T_FILE && in->nlink != inmap[idx]) {
    fprintf(stderr, "ERROR: bad reference count for file.\n");
    exit(1);
  }
}

// Ensure a directory inode is only referenced once
void chk_dir_once(struct dinode *in, int idx, int *inmap) {
  if (in->type == T_DIR && inmap[idx] > 1) {
    fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
    exit(1);
  }
}

// Main function to perform directory checks
void dir_chk(img_t *img) {
  int inmap[img->sb->ninodes];
  memset(inmap, 0, sizeof(int) * img->sb->ninodes);
  struct dinode *in, *root;

  // Initialize and traverse the directory structure
  in = (struct dinode *)(img->inodeblks);
  root = ++in;
  inmap[0]++;
  inmap[1]++;
  traverse_dirs(img, root, inmap);
  in++;
  for (int i = 2; i < img->sb->ninodes; i++, in++) {
    chk_in_use(in, i, inmap);
    chk_in_free(in, i, inmap);
    chk_ref_cnt(in, i, inmap);
    chk_dir_once(in, i, inmap);
  }
}

// Initialize the image structure with mmap and other details
void init_img(img_t *img, char *mmap, char *fname) {
  img->map = mmap;
  img->sb = (struct superblock *)(mmap + 1 * BLK_SZ);
  img->ninodeblks = (img->sb->ninodes / IPB) + 1;
  img->nbitmapblks = (img->sb->size / BPB) + 1;
  img->inodeblks = (char *)(mmap + 2 * BLK_SZ);
  img->bitmapblks = (char *)(img->inodeblks + img->ninodeblks * BLK_SZ);
  img->data = (char *)(img->bitmapblks + img->nbitmapblks * BLK_SZ);
  img->firstblk = img->ninodeblks + img->nbitmapblks + 2;
}

// Main function to load and check the file system image
int main(int argc, char *argv[]) {
  int fsfd;
  img_t img;
  char *mmap_img;
  struct stat fsStat;

  // Basic argument check
  if (argc < 2) {
    fprintf(stderr, "Usage: fcheck <file_system_image>\n");
    exit(1);
  }

  // Open file system image
  fsfd = open(argv[1], O_RDONLY);
  if (fsfd < 0) {
    perror(argv[1]);
    exit(1);
  }

  // Get file status
  if (fstat(fsfd, &fsStat) < 0) {
    exit(1);
  }

  // Memory-map the file system image
  mmap_img = mmap(NULL, fsStat.st_size, PROT_READ, MAP_PRIVATE, fsfd, 0);
  if (mmap_img == MAP_FAILED) {
    perror("mmap failed");
    exit(1);
  }

  // Initialize image data structure
  init_img(&img, mmap_img, argv[1]);

  // Iterate through inodes and perform various checks
  img_t *img2 = &img;
  struct dinode *in = (struct dinode *)(img2->inodeblks);
  bool *chk_blks = calloc(img2->sb->size, sizeof(bool));
  for (int i = 0; i < img2->sb->ninodes; i++, in++) {
    if (in->type == 0) continue;

    validate_type(in);
    check_direct(img2, in);
    check_indirect(img2, in);
    if (i == 1) {
      // Special case for root directory
      if (in->type != T_DIR) {
        fprintf(stderr, "ERROR: root directory does not exist.\n");
        exit(1);
      }
      validate_dir(img2, in, 1);
    } else if (in->type == T_DIR) {
      validate_dir(img2, in, i);
    }
    chk_bmp_addr(img2, in);
  }

  free(chk_blks);

  // Check bitmap and address usage
  bmp_chk(&img);
  addrs_chk(&img);
  dir_chk(&img);

  exit(0);
}

