#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <time.h>
#include <libgen.h>
#include <stddef.h>
#include <inttypes.h>
#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
#define GROUP_ID 7  // Course group ID stored in each inode's proj_id field
#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

void superblock_crc_finalize(superblock_t* sb) {
    uint8_t tmp[BS];
    memset(tmp, 0, BS);  // Zero the whole block first
    memcpy(tmp, sb, sizeof(superblock_t));
    *(uint32_t*)(tmp + 112) = 0;  // Zero checksum field at offset 112
    sb->checksum = crc32(tmp, BS - 4);
}

int validate_superblock_crc(uint8_t* superblock_block) {
    superblock_t* sb = (superblock_t*)superblock_block;
    uint32_t stored_checksum = sb->checksum;
    
    // Create a copy of the superblock structure for calculation
    superblock_t sb_copy;
    memcpy(&sb_copy, sb, sizeof(superblock_t));
    sb_copy.checksum = 0; // Zero out checksum field
    
    // Create full block for CRC calculation
    uint8_t tmp[BS];
    memset(tmp, 0, BS);
    memcpy(tmp, &sb_copy, sizeof(superblock_t));
    
    uint32_t calculated_checksum = crc32(tmp, BS - 4);
    return (stored_checksum == calculated_checksum);
}

void print_usage(const char *program_name) {
    printf("Usage: %s --input <input_image> --output <output_image> --file <file_to_add>\n", program_name);
    printf("  --input: input image file name\n");
    printf("  --output: output image file name\n");
    printf("  --file: file to add to the file system\n");
}


uint32_t find_free_inode(uint8_t *inode_bitmap, uint32_t inode_count) {
    // Inodes are 1-indexed, but bitmap is 0-indexed (bit 0 = inode 1)
    for (uint32_t inode_no = 1; inode_no <= inode_count; inode_no++) {
        uint32_t bit_idx = inode_no - 1; // Convert to 0-indexed bitmap position
        uint32_t byte_idx = bit_idx / 8;
        uint32_t bit_pos = bit_idx % 8;
        if (!(inode_bitmap[byte_idx] & (1 << bit_pos))) {
            return inode_no;
        }
    }
    return 0;
}


uint32_t find_free_data_block(uint8_t *data_bitmap, uint32_t data_region_blocks) {
    for (uint32_t i = 0; i < data_region_blocks; i++) {
        uint32_t byte_idx = i / 8;
        uint32_t bit_idx = i % 8;
        if (!(data_bitmap[byte_idx] & (1 << bit_idx))) {
            return i; // Return relative block number within data region
        }
    }
    return UINT32_MAX; // Sentinel value for no free blocks
}


void set_inode_allocated(uint8_t *inode_bitmap, uint32_t inode_no) {
    uint32_t bit_idx = inode_no - 1; // Convert to 0-indexed bitmap position
    uint32_t byte_idx = bit_idx / 8;
    uint32_t bit_pos = bit_idx % 8;
    inode_bitmap[byte_idx] |= (1 << bit_pos);
}


void set_data_block_allocated(uint8_t *data_bitmap, uint32_t relative_block_no) {
    uint32_t byte_idx = relative_block_no / 8;
    uint32_t bit_idx = relative_block_no % 8;
    data_bitmap[byte_idx] |= (1 << bit_idx);
}

int main(int argc, char *argv[]) {
    crc32_init();

    char *input_image = NULL;
    char *output_image = NULL;
    char *file_to_add = NULL;

    int opt;
    static struct option long_options[] = {
        {"input",  required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"file",   required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "i:o:f:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i': input_image = optarg; break;
            case 'o': output_image = optarg; break;
            case 'f': file_to_add = optarg; break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!input_image || !output_image || !file_to_add) {
        print_usage(argv[0]);
        return 1;
    }

    int rc = 1;
    FILE *file_fp = NULL, *input_fp = NULL, *output_fp = NULL;
    char *path_copy = NULL;

    // ---- Open the file to add and check its size ----
    file_fp = fopen(file_to_add, "rb");
    if (!file_fp) { perror("Failed to open file to add"); goto done; }

    fseeko(file_fp, 0, SEEK_END);
    uint64_t file_size = (uint64_t)ftello(file_fp);
    fseeko(file_fp, 0, SEEK_SET);

    if (file_size > (uint64_t)DIRECT_MAX * BS) {
        fprintf(stderr, "Error: file is too large (max %u bytes with 12 direct blocks)\n",
                (unsigned)(DIRECT_MAX * BS));
        goto done;
    }

    // basename() may modify its argument, so give it a copy
    path_copy = strdup(file_to_add);
    if (!path_copy) { perror("strdup"); goto done; }
    const char *filename = basename(path_copy);
    size_t name_len = strlen(filename);
    if (name_len == 0 || name_len > 57) {
        fprintf(stderr, "Error: filename must be 1..57 characters\n");
        goto done;
    }

    // ---- Read and validate the superblock ----
    input_fp = fopen(input_image, "rb");
    if (!input_fp) { perror("Failed to open input image"); goto done; }

    uint8_t superblock_block[BS];
    if (fread(superblock_block, BS, 1, input_fp) != 1) {
        perror("Failed to read superblock");
        goto done;
    }

    superblock_t superblock;
    memcpy(&superblock, superblock_block, sizeof(superblock));

    if (superblock.magic != 0x4D565346) {
        fprintf(stderr, "Invalid file system image (wrong magic number)\n");
        goto done;
    }
    if (!validate_superblock_crc(superblock_block)) {
        fprintf(stderr, "Invalid file system image (corrupted superblock)\n");
        goto done;
    }
    if (superblock.block_size != BS ||
        superblock.inode_count == 0 || superblock.inode_count > (uint64_t)BS * 8 ||
        superblock.data_region_blocks == 0 || superblock.data_region_blocks > (uint64_t)BS * 8 ||
        superblock.data_region_start + superblock.data_region_blocks > superblock.total_blocks) {
        fprintf(stderr, "Invalid file system image (inconsistent superblock)\n");
        goto done;
    }

    // ---- Read bitmaps, root inode and root directory ----
    uint8_t inode_bitmap[BS], data_bitmap[BS];
    fseeko(input_fp, (off_t)(superblock.inode_bitmap_start * BS), SEEK_SET);
    if (fread(inode_bitmap, BS, 1, input_fp) != 1) { perror("Failed to read inode bitmap"); goto done; }
    fseeko(input_fp, (off_t)(superblock.data_bitmap_start * BS), SEEK_SET);
    if (fread(data_bitmap, BS, 1, input_fp) != 1) { perror("Failed to read data bitmap"); goto done; }

    inode_t root_inode;
    fseeko(input_fp, (off_t)(superblock.inode_table_start * BS + (ROOT_INO - 1) * INODE_SIZE), SEEK_SET);
    if (fread(&root_inode, INODE_SIZE, 1, input_fp) != 1) { perror("Failed to read root inode"); goto done; }

    if (root_inode.direct[0] < superblock.data_region_start ||
        root_inode.direct[0] >= superblock.total_blocks) {
        fprintf(stderr, "Invalid file system image (bad root directory block)\n");
        goto done;
    }

    uint8_t root_data[BS];
    fseeko(input_fp, (off_t)((uint64_t)root_inode.direct[0] * BS), SEEK_SET);
    if (fread(root_data, BS, 1, input_fp) != 1) { perror("Failed to read root directory data"); goto done; }

    // ---- Reject duplicate names, find a free directory slot ----
    int dirent_slot = -1;
    for (size_t i = 0; i < BS / sizeof(dirent64_t); i++) {
        dirent64_t *e = (dirent64_t *)(root_data + i * sizeof(dirent64_t));
        if (e->inode_no == 0) {
            if (dirent_slot == -1) dirent_slot = (int)i;
            continue;
        }
        if (strncmp(e->name, filename, sizeof(e->name)) == 0) {
            fprintf(stderr, "Error: '%s' already exists in the root directory\n", filename);
            goto done;
        }
    }
    if (dirent_slot == -1) {
        fprintf(stderr, "No free slots in root directory\n");
        goto done;
    }

    // ---- Allocate an inode and all the data blocks BEFORE writing anything ----
    uint32_t new_inode_no = find_free_inode(inode_bitmap, (uint32_t)superblock.inode_count);
    if (new_inode_no == 0) {
        fprintf(stderr, "No free inodes available\n");
        goto done;
    }

    uint32_t blocks_needed = (uint32_t)((file_size + BS - 1) / BS);   // 0 for an empty file
    uint32_t allocated[DIRECT_MAX] = {0};
    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint32_t rel = find_free_data_block(data_bitmap, (uint32_t)superblock.data_region_blocks);
        if (rel == UINT32_MAX) {
            fprintf(stderr, "No free data blocks available (needed %u)\n", blocks_needed);
            goto done;
        }
        set_data_block_allocated(data_bitmap, rel);   // mark immediately so the next search skips it
        allocated[b] = (uint32_t)superblock.data_region_start + rel;
    }
    set_inode_allocated(inode_bitmap, new_inode_no);

    // ---- Build inode and directory entry ----
    time_t now = time(NULL);
    inode_t new_inode = {0};
    new_inode.mode = 0100000;
    new_inode.links = 1;
    new_inode.size_bytes = file_size;
    new_inode.atime = (uint64_t)now;
    new_inode.mtime = (uint64_t)now;
    new_inode.ctime = (uint64_t)now;
    for (uint32_t b = 0; b < blocks_needed; b++) new_inode.direct[b] = allocated[b];
    new_inode.proj_id = GROUP_ID;
    inode_crc_finalize(&new_inode);

    dirent64_t new_dirent = {0};
    new_dirent.inode_no = new_inode_no;
    new_dirent.type = 1;
    memcpy(new_dirent.name, filename, name_len);
    dirent_checksum_finalize(&new_dirent);
    memcpy(root_data + (size_t)dirent_slot * sizeof(dirent64_t), &new_dirent, sizeof(dirent64_t));

    // Root directory grows by one entry (link count is unchanged: only subdirectories add links)
    root_inode.size_bytes += sizeof(dirent64_t);
    root_inode.mtime = (uint64_t)now;
    root_inode.ctime = (uint64_t)now;
    inode_crc_finalize(&root_inode);

    // ---- Copy the input image to the output image ----
    output_fp = fopen(output_image, "wb");
    if (!output_fp) { perror("Failed to create output image"); goto done; }

    fseeko(input_fp, 0, SEEK_SET);
    uint8_t buffer[BS];
    for (uint64_t blk = 0; blk < superblock.total_blocks; blk++) {
        if (fread(buffer, 1, BS, input_fp) != BS) { perror("Failed to copy input image"); goto done; }
        if (fwrite(buffer, 1, BS, output_fp) != BS) { perror("Failed to write output image"); goto done; }
    }

    // ---- Patch metadata and write file data ----
    fseeko(output_fp, (off_t)(superblock.inode_table_start * BS + (ROOT_INO - 1) * INODE_SIZE), SEEK_SET);
    fwrite(&root_inode, INODE_SIZE, 1, output_fp);

    fseeko(output_fp, (off_t)((uint64_t)root_inode.direct[0] * BS), SEEK_SET);
    fwrite(root_data, BS, 1, output_fp);

    uint64_t remaining = file_size;
    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint8_t file_buffer[BS];
        memset(file_buffer, 0, BS);
        size_t n = (remaining > BS) ? BS : (size_t)remaining;
        if (fread(file_buffer, 1, n, file_fp) != n) { perror("Failed to read file data"); goto done; }
        fseeko(output_fp, (off_t)((uint64_t)allocated[b] * BS), SEEK_SET);
        if (fwrite(file_buffer, BS, 1, output_fp) != 1) { perror("Failed to write file data"); goto done; }
        remaining -= n;
    }

    fseeko(output_fp, (off_t)(superblock.inode_table_start * BS + (uint64_t)(new_inode_no - 1) * INODE_SIZE), SEEK_SET);
    fwrite(&new_inode, INODE_SIZE, 1, output_fp);

    fseeko(output_fp, (off_t)(superblock.inode_bitmap_start * BS), SEEK_SET);
    fwrite(inode_bitmap, BS, 1, output_fp);

    fseeko(output_fp, (off_t)(superblock.data_bitmap_start * BS), SEEK_SET);
    fwrite(data_bitmap, BS, 1, output_fp);

    if (fflush(output_fp) != 0) { perror("Failed to flush output image"); goto done; }

    printf("File '%s' added successfully to file system\n", file_to_add);
    printf("New inode: %u, Data blocks: %u", new_inode_no, blocks_needed);
    for (uint32_t b = 0; b < blocks_needed; b++) printf("%s%u", b ? "," : " [", allocated[b]);
    printf("%s\n", blocks_needed ? "]" : "");
    rc = 0;

done:
    if (file_fp) fclose(file_fp);
    if (input_fp) fclose(input_fp);
    if (output_fp) {
        fclose(output_fp);
        if (rc != 0) remove(output_image);   // never leave a half-written image behind
    }
    free(path_copy);
    return rc;
}
