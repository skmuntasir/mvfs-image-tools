# MVFS Image Tools

Two small C command-line tools that create and populate a disk image for a simple, custom
Unix-style file system (**MVFS**). Built as an Operating Systems course project to practice
on-disk layout, inodes, bitmaps, and checksums.

| Tool | What it does |
|---|---|
| `mkfs_builder` | Creates an empty, formatted MVFS image (like `mkfs`) |
| `mkfs_adder`   | Copies a host file into the root directory of an existing image |
| `tools/inspect_fs.py` | Python verifier: checks every checksum/bitmap, lists and extracts files |

## Build

```bash
make          # builds mkfs_builder and mkfs_adder (needs gcc; Linux, macOS, or MinGW on Windows)
make test     # end-to-end tests (needs python3)
```

## Usage

```bash
# 1. Create a 1 MiB image with 256 inodes
./mkfs_builder --image disk.img --size-kib 1024 --inodes 256

# 2. Add files (the input image is never modified; a new image is written)
./mkfs_adder --input disk.img  --output disk1.img --file notes.txt
./mkfs_adder --input disk1.img --output disk2.img --file photo.jpg

# 3. Inspect / verify / extract
python3 tools/inspect_fs.py disk2.img
python3 tools/inspect_fs.py disk2.img --extract notes.txt notes_copy.txt
```

Builder limits: `--size-kib` 180–4096 (multiple of 4), `--inodes` 128–512.

## On-disk layout

Block size is 4096 bytes. All integers are little-endian and structures are packed.

| Block | Contents |
|---|---|
| 0 | Superblock (116 bytes used, rest zero) |
| 1 | Inode bitmap (bit 0 = inode 1) |
| 2 | Data bitmap (bit 0 = first block of the data region) |
| 3 … | Inode table, 128 bytes per inode |
| after the table | Data region; its first block holds the root directory |

**Superblock** – magic `0x4D565346` ("MVFS"), version, block size, total blocks, inode count,
start/length of each region, root inode number, mtime, flags, and a CRC32 over the whole block
(with the checksum field zeroed, last 4 bytes of the block excluded).

**Inode (128 B)** – mode, link count, uid/gid, size, atime/mtime/ctime, **12 direct block
pointers**, project id, and a CRC32 over the first 120 bytes.

**Directory entry (64 B)** – inode number, type (1 = file, 2 = directory), 58-byte name field
(names up to 57 characters), and an XOR checksum of the first 63 bytes.

## Limits

- Files go in the root directory only; there are no subdirectories.
- Maximum file size is 12 × 4096 = **49,152 bytes** (direct blocks only, no indirect blocks).
- The root directory is one block, so at most 62 files (64 entries minus `.` and `..`).
- The `GROUP_ID` constant (`7`) is stored in every inode's `proj_id`; change it in both sources
  if you reuse the project.

## Notes on the implementation

- `mkfs_adder` allocates the inode and *all* required data blocks (marking each in the in-memory
  bitmap immediately) before writing anything, so a failure (disk full, duplicate name, bad
  image) never leaves a partial output image behind.
- It rejects duplicate file names, empty files get no data blocks, and the root directory's
  size is updated while its link count stays at 2.
- An earlier version marked the first data block in the bitmap only at the very end, so files
  larger than one block were assigned the same block twice and silently corrupted. The tests in
  `tests/run_tests.sh` and the block-ownership check in `tools/inspect_fs.py` guard against this.

## Project structure

```
src/mkfs_builder.c   image creator
src/mkfs_adder.c     file adder
tools/inspect_fs.py  verifier / lister / extractor
tests/run_tests.sh   end-to-end tests
Makefile
```
