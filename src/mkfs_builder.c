#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <stddef.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
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
_Static_assert(sizeof(superblock_t) == 116, "superblock_t must be 116 bytes");

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

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    // Create full block with superblock structure
    uint8_t tmp[BS];
    memset(tmp, 0, BS);
    memcpy(tmp, sb, sizeof(superblock_t));
    memset(tmp + offsetof(superblock_t, checksum), 0, 4); // Zero out checksum field
    uint32_t s = crc32(tmp, BS - 4);
    sb->checksum = s;
    return s;
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

void print_usage(const char *program_name) {
    printf("Usage: %s --image <output_image> --size-kib <180..4096> --inodes <128..512>\n", program_name);
    printf("  --image: output image file name\n");
    printf("  --size-kib: total size in KB (must be multiple of 4, range: 180-4096)\n");
    printf("  --inodes: number of inodes (range: 128-512)\n");
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    char *output_image = NULL;
    uint32_t size_kib = 0;
    uint32_t inode_count = 0;
    
    int opt;
    static struct option long_options[] = {
        {"image", required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes", required_argument, 0, 'n'},
        {0, 0, 0, 0}
    };
    
    while ((opt = getopt_long(argc, argv, "i:s:n:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                output_image = optarg;
                break;
            case 's':
                size_kib = atoi(optarg);
                break;
            case 'n':
                inode_count = atoi(optarg);
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    // Validate parameters
    if (!output_image || size_kib < 180 || size_kib > 4096 || size_kib % 4 != 0 || 
        inode_count < 128 || inode_count > 512) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Calculate file system layout
    uint64_t total_blocks = (uint64_t)size_kib * 1024 / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    uint64_t data_region_start = 3 + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;
    
    // Validate layout
    if (data_region_blocks == 0) {
        fprintf(stderr, "Error: File system too small for given parameters\n");
        return 1;
    }
    if (inode_table_blocks < 1 || data_region_start >= total_blocks) {
        fprintf(stderr, "Error: Invalid file system layout\n");
        return 1;
    }
    
    time_t now = time(NULL);
    
    // Initialize superblock
    superblock_t superblock = {0};
    superblock.magic = 0x4D565346;  // "MVFS" in little-endian
    superblock.version = 1;
    superblock.block_size = BS;
    superblock.total_blocks = total_blocks;
    superblock.inode_count = inode_count;
    superblock.inode_bitmap_start = 1;
    superblock.inode_bitmap_blocks = 1;
    superblock.data_bitmap_start = 2;
    superblock.data_bitmap_blocks = 1;
    superblock.inode_table_start = 3;
    superblock.inode_table_blocks = inode_table_blocks;
    superblock.data_region_start = data_region_start;
    superblock.data_region_blocks = data_region_blocks;
    superblock.root_inode = ROOT_INO;
    superblock.mtime_epoch = (uint64_t)now;
    superblock.flags = 0;
    
    // Initialize root inode
    inode_t root_inode = {0};
    root_inode.mode = 0040000;  // Directory mode
    root_inode.links = 2;       // . and .. entries
    root_inode.uid = 0;
    root_inode.gid = 0;
    root_inode.size_bytes = 2u * (uint64_t)sizeof(dirent64_t); // Root directory size (. and .. entries)
    root_inode.atime = (uint64_t)now;
    root_inode.mtime = (uint64_t)now;
    root_inode.ctime = (uint64_t)now;
    root_inode.direct[0] = (uint32_t)data_region_start;  // First data block
    root_inode.proj_id = GROUP_ID;  // Your group ID
    
    // Create root directory entries
    dirent64_t dot_entry = {0};
    dot_entry.inode_no = ROOT_INO;
    dot_entry.type = 2;  // Directory
    strncpy(dot_entry.name, ".", sizeof(dot_entry.name)-1);
    dot_entry.name[sizeof(dot_entry.name)-1] = '\0';
    
    dirent64_t dotdot_entry = {0};
    dotdot_entry.inode_no = ROOT_INO;
    dotdot_entry.type = 2;  // Directory
    strncpy(dotdot_entry.name, "..", sizeof(dotdot_entry.name)-1);
    dotdot_entry.name[sizeof(dotdot_entry.name)-1] = '\0';
    
    // Calculate checksums
    superblock_crc_finalize(&superblock);
    inode_crc_finalize(&root_inode);
    dirent_checksum_finalize(&dot_entry);
    dirent_checksum_finalize(&dotdot_entry);
    
    // Initialize bitmaps
    uint8_t inode_bitmap[BS];
    memset(inode_bitmap, 0, BS);
    inode_bitmap[0] = 0x01;  // Mark inode 1 (root) as allocated
    
    uint8_t data_bitmap[BS];
    memset(data_bitmap, 0, BS);
    data_bitmap[0] = 0x01;  // Mark first data block as allocated
    
    // Initialize inode table
    uint8_t inode_table[BS];
    memset(inode_table, 0, BS);
    memcpy(inode_table, &root_inode, INODE_SIZE);
    
    // Initialize root directory data
    uint8_t root_data[BS];
    memset(root_data, 0, BS);
    memcpy(root_data, &dot_entry, sizeof(dirent64_t));
    memcpy(root_data + sizeof(dirent64_t), &dotdot_entry, sizeof(dirent64_t));
    
    // Create output file
    FILE *fp = fopen(output_image, "wb");
    if (!fp) {
        perror("Failed to open output file");
        return 1;
    }
    
    // Write superblock block (with padding)
    uint8_t superblock_block[BS];
    memset(superblock_block, 0, BS);
    memcpy(superblock_block, &superblock, sizeof(superblock));
    if (fwrite(superblock_block, BS, 1, fp) != 1) {
        perror("Failed to write superblock");
        fclose(fp);
        return 1;
    }
    
    // Write inode bitmap
    if (fwrite(inode_bitmap, BS, 1, fp) != 1) {
        perror("Failed to write inode bitmap");
        fclose(fp);
        return 1;
    }
    
    // Write data bitmap
    if (fwrite(data_bitmap, BS, 1, fp) != 1) {
        perror("Failed to write data bitmap");
        fclose(fp);
        return 1;
    }
    
    // Write inode table
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        if (i == 0) {
            if (fwrite(inode_table, BS, 1, fp) != 1) {
                perror("Failed to write inode table");
                fclose(fp);
                return 1;
            }
        } else {
            uint8_t zero_block[BS];
            memset(zero_block, 0, BS);
            if (fwrite(zero_block, BS, 1, fp) != 1) {
                perror("Failed to write zero block");
                fclose(fp);
                return 1;
            }
        }
    }
    
    // Write root directory data
    if (fwrite(root_data, BS, 1, fp) != 1) {
        perror("Failed to write root directory data");
        fclose(fp);
        return 1;
    }
    
    // Write remaining zero blocks
    uint64_t written_blocks = 1 + 1 + 1 + inode_table_blocks + 1;
    uint64_t remaining_blocks = total_blocks - written_blocks;
    uint8_t zero_block[BS];
    memset(zero_block, 0, BS);
    for (uint64_t i = 0; i < remaining_blocks; i++) {
        if (fwrite(zero_block, BS, 1, fp) != 1) {
            perror("Failed to write zero block");
            fclose(fp);
            return 1;
        }
    }
    
    fclose(fp);
    
    printf("File system image created successfully: %s\n", output_image);
    printf("Total blocks: %lu, Inodes: %u, Inode table blocks: %lu, Data blocks: %lu\n", 
           (unsigned long)total_blocks, inode_count, (unsigned long)inode_table_blocks, (unsigned long)data_region_blocks);
    
    return 0;
}
