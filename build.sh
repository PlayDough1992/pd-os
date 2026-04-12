#!/bin/bash
# ============================================================================
# PD-OS Build Script (Linux)
# ============================================================================
# Usage: ./build.sh [all|run|run-debug|clean|setup-check]
# ============================================================================

set -e

TARGET="${1:-all}"

BUILD_DIR="build"

# Bootloader
BOOTLOADER_SRC="bootloader/stage1.asm"
BOOTLOADER_BIN="$BUILD_DIR/bootloader.bin"
STAGE2_SRC="bootloader/stage2.asm"
STAGE2_BIN="$BUILD_DIR/stage2.bin"

# Kernel
KERNEL_DIR="kernel"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
KERNEL_BIN="$BUILD_DIR/kernel.bin"

# Disk image
DISK_IMAGE="$BUILD_DIR/pd-os.img"
DISK_SIZE=131072  # 131072 * 512 = 64 MB

# Cross-toolchain (i686-linux-gnu provided by gcc-i686-linux-gnu package)
CROSS_CC="i686-linux-gnu-gcc"
CROSS_LD="i686-linux-gnu-ld"
CROSS_OBJCOPY="i686-linux-gnu-objcopy"

# Compiler / linker flags
CFLAGS="-m32 -ffreestanding -nostdlib -nostdinc -fno-builtin \
        -fno-stack-protector -nostartfiles -nodefaultlibs \
        -fno-pic -fno-pie \
        -Wall -Wextra -c"
IFLAGS="-I$KERNEL_DIR/include"
LDFLAGS="-m elf_i386 -nostdlib"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ---- build ----------------------------------------------------------------
build() {
    echo -e "${CYAN}Building PD-OS...${NC}"
    mkdir -p "$BUILD_DIR"

    # --- Stage 1 ---
    echo -e "${CYAN}  [1] Assembling Stage 1 bootloader...${NC}"
    nasm -f bin "$BOOTLOADER_SRC" -o "$BOOTLOADER_BIN"
    SIZE=$(wc -c < "$BOOTLOADER_BIN")
    if [ "$SIZE" -ne 512 ]; then
        echo -e "${RED}[FAIL] Stage 1 must be exactly 512 bytes (got $SIZE)${NC}"
        exit 1
    fi
    echo -e "${GREEN}  [OK] Stage 1: $SIZE bytes${NC}"

    # --- Stage 2 ---
    echo -e "${CYAN}  [2] Assembling Stage 2 bootloader...${NC}"
    nasm -f bin "$STAGE2_SRC" -o "$STAGE2_BIN"
    S2SIZE=$(wc -c < "$STAGE2_BIN")
    echo -e "${GREEN}  [OK] Stage 2: $S2SIZE bytes${NC}"

    # --- Kernel ---
    echo -e "${CYAN}  [3] Compiling kernel...${NC}"

    nasm -f elf32 "$KERNEL_DIR/arch/x86/entry.asm" -o "$BUILD_DIR/entry.o"
    nasm -f elf32 "$KERNEL_DIR/arch/x86/idt.asm"   -o "$BUILD_DIR/idt_stubs.o"

    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/vga.c"          -o "$BUILD_DIR/vga.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/io.c"              -o "$BUILD_DIR/io.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/panic.c"           -o "$BUILD_DIR/panic.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/idt.c"         -o "$BUILD_DIR/idt.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/pic.c"         -o "$BUILD_DIR/pic.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/exceptions.c"  -o "$BUILD_DIR/exceptions.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/pit.c"          -o "$BUILD_DIR/pit.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/keyboard.c"     -o "$BUILD_DIR/keyboard.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/ata.c"           -o "$BUILD_DIR/ata.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/users.c"           -o "$BUILD_DIR/users.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/login.c"           -o "$BUILD_DIR/login.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/e820.c"              -o "$BUILD_DIR/e820.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/pmm.c"               -o "$BUILD_DIR/pmm.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/paging.c"            -o "$BUILD_DIR/paging.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/kheap.c"             -o "$BUILD_DIR/kheap.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/vfs.c"               -o "$BUILD_DIR/vfs.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/pdfs.c"              -o "$BUILD_DIR/pdfs.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/fat32.c"             -o "$BUILD_DIR/fat32.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/ext2.c"              -o "$BUILD_DIR/ext2.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/ntfs.c"              -o "$BUILD_DIR/ntfs.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/shell.c"            -o "$BUILD_DIR/shell.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/kernel.c"          -o "$BUILD_DIR/kernel_main.o"

    echo -e "${CYAN}  [4] Linking kernel...${NC}"
    $CROSS_LD $LDFLAGS -T "$KERNEL_DIR/linker.ld" \
        "$BUILD_DIR/entry.o" \
        "$BUILD_DIR/idt_stubs.o" \
        "$BUILD_DIR/vga.o" \
        "$BUILD_DIR/io.o" \
        "$BUILD_DIR/panic.o" \
        "$BUILD_DIR/idt.o" \
        "$BUILD_DIR/pic.o" \
        "$BUILD_DIR/exceptions.o" \
        "$BUILD_DIR/pit.o" \
        "$BUILD_DIR/keyboard.o" \
        "$BUILD_DIR/ata.o" \
        "$BUILD_DIR/users.o" \
        "$BUILD_DIR/login.o" \
        "$BUILD_DIR/e820.o" \
        "$BUILD_DIR/pmm.o" \
        "$BUILD_DIR/paging.o" \
        "$BUILD_DIR/kheap.o" \
        "$BUILD_DIR/vfs.o" \
        "$BUILD_DIR/pdfs.o" \
        "$BUILD_DIR/fat32.o" \
        "$BUILD_DIR/ext2.o" \
        "$BUILD_DIR/ntfs.o" \
        "$BUILD_DIR/shell.o" \
        "$BUILD_DIR/kernel_main.o" \
        -o "$KERNEL_ELF"

    $CROSS_OBJCOPY -O binary "$KERNEL_ELF" "$KERNEL_BIN"
    KSIZE=$(wc -c < "$KERNEL_BIN")
    echo -e "${GREEN}  [OK] Kernel: $KSIZE bytes${NC}"

    # --- Disk image ---
    echo -e "${CYAN}  [5] Creating disk image...${NC}"
    rm -f "$DISK_IMAGE"
    truncate -s $((DISK_SIZE * 512)) "$DISK_IMAGE"
    dd if="$BOOTLOADER_BIN"  of="$DISK_IMAGE" conv=notrunc bs=512 seek=0 count=1  2>/dev/null
    dd if="$STAGE2_BIN"      of="$DISK_IMAGE" conv=notrunc bs=512 seek=1          2>/dev/null
    dd if="$KERNEL_BIN"      of="$DISK_IMAGE" conv=notrunc bs=512 seek=6          2>/dev/null
    echo -e "${GREEN}  [OK] Disk image: $DISK_IMAGE (64 MB)${NC}"

    # --- PDFS v2 init at LBA 200 ---
    # Layout:  LBA 200 = superblock (v2, 8 uint32 fields before reserved[119])
    #          LBA 201 = journal sector (clean, reserved[0..1]=0)
    #          LBA 202-205 = root dir (4 sectors, 32×64B dirents, all zeroed)
    #          LBA 206+ = data
    # Superblock fields: magic, version, dir_lba, dir_sectors, data_lba,
    #                    next_free_lba, jrnl_lba   (7 × uint32)
    # reserved[0..1] used as journal dirty+target → written as 0 here
    echo -e "${CYAN}  [6] Initialising PDFS v2 at LBA 200...${NC}"
    python3 -c "
import struct, sys
BASE = 200
JRNL_LBA = BASE + 1              # 201
DIR_LBA  = BASE + 2              # 202
DIR_SECS = 4
DATA_LBA = DIR_LBA + DIR_SECS   # 206
# 7 uint32 header fields
sb = struct.pack('<IIIIIII', 0x50444653, 2, DIR_LBA, DIR_SECS, DATA_LBA, DATA_LBA, JRNL_LBA)
sb += b'\\x00' * (512 - len(sb))   # pad to 512 bytes (reserved[] all zero)
journal = b'\\x00' * 512           # clean journal sector
rootdir = b'\\x00' * (512 * DIR_SECS)  # 4 sectors, all dirents free
sys.stdout.buffer.write(sb + journal + rootdir)
" | dd of="$DISK_IMAGE" conv=notrunc bs=512 seek=200 2>/dev/null
    echo -e "${GREEN}  [OK] PDFS v2 ready  (SB=200 jrnl=201 dir=202-205 data=206+)${NC}"
    # --- FAT32 init at LBA 2048 ---
    # Volume:  129024 sectors (LBA 2048-131071)
    # Layout:  32 reserved | FAT1(1000) | FAT2(1000) | data(127024)
    # Cluster: 1 sector/cluster (512 B), root dir = cluster 2
    echo -e "${CYAN}  [7] Formatting FAT32 at LBA 2048...${NC}"
    python3 -c "
import struct, sys
TOTAL = 129024; RES = 32; FATC = 2; FATSZ = 1000; SPC = 1
VOL_LBA = 2048; ROOT_CLUS = 2
# BPB
bpb = bytearray(512)
bpb[0:3]   = b'\\xEB\\x58\\x90'
bpb[3:11]  = b'MSDOS5.0'
struct.pack_into('<H', bpb, 11, 512)
bpb[13] = SPC
struct.pack_into('<H', bpb, 14, RES)
bpb[16] = FATC
struct.pack_into('<H', bpb, 17, 0)
struct.pack_into('<H', bpb, 19, 0)
bpb[21] = 0xF8
struct.pack_into('<H', bpb, 22, 0)
struct.pack_into('<H', bpb, 24, 63)
struct.pack_into('<H', bpb, 26, 255)
struct.pack_into('<I', bpb, 28, VOL_LBA)
struct.pack_into('<I', bpb, 32, TOTAL)
struct.pack_into('<I', bpb, 36, FATSZ)
struct.pack_into('<H', bpb, 40, 0)
struct.pack_into('<H', bpb, 42, 0)
struct.pack_into('<I', bpb, 44, ROOT_CLUS)
struct.pack_into('<H', bpb, 48, 1)
struct.pack_into('<H', bpb, 50, 6)
bpb[52:64] = b'\\x00' * 12
bpb[64]  = 0x80
bpb[65]  = 0x00
bpb[66]  = 0x29
struct.pack_into('<I', bpb, 67, 0x50444F53)
bpb[71:82] = b'PD-OS      '
bpb[82:90] = b'FAT32   '
struct.pack_into('<H', bpb, 510, 0xAA55)
# FSInfo
fsi = bytearray(512)
struct.pack_into('<I', fsi,   0, 0x41615252)
struct.pack_into('<I', fsi, 484, 0x61417272)
struct.pack_into('<I', fsi, 488, 0xFFFFFFFF)
struct.pack_into('<I', fsi, 492, 0xFFFFFFFF)
struct.pack_into('<H', fsi, 510, 0xAA55)
# FAT (both copies): reserve entries 0-1, root dir at cluster 2 = EOC
fat = bytearray(FATSZ * 512)
struct.pack_into('<I', fat,  0, 0x0FFFFFF8)
struct.pack_into('<I', fat,  4, 0x0FFFFFFF)
struct.pack_into('<I', fat,  8, 0x0FFFFFFF)
# Assemble: BPB + FSInfo + reserved[2..31] + FAT1 + FAT2 + root-dir cluster
out = bpb + fsi + bytes((RES-2)*512) + bytes(fat) + bytes(fat) + bytes(SPC*512)
sys.stdout.buffer.write(out)
" | dd of="$DISK_IMAGE" conv=notrunc bs=512 seek=2048 2>/dev/null
    echo -e "${GREEN}  [OK] FAT32 ready (LBA 2048-131071, root cluster at LBA 4080)${NC}"

    # --- ext2 at LBA 4096 ---
    # Volume: 65536 sectors (32 MB), 1 KB blocks, 1 block group
    # Layout: boot(2) | superblock(2) | BGD(2) | block-bm(2) | inode-bm(2)
    #         | inode-table(N) | data blocks
    # inodes_per_group=1024, inode_size=128
    echo -e "${CYAN}  [8] Formatting ext2 at LBA 4096 (32 MB)...${NC}"
    python3 - <<'PYEOF'
import struct, sys

BASE_SECTOR = 4096
TOTAL_SECTORS = 65536     # 32 MB
TOTAL_BLOCKS  = TOTAL_SECTORS // 2   # 1 block = 2 sectors = 1024 bytes
INODES_PER_GROUP = 1024
INODE_SIZE = 128
BLOCK_SIZE = 1024
INODE_TABLE_BLOCKS = (INODES_PER_GROUP * INODE_SIZE) // BLOCK_SIZE  # 128
# Fixed block assignments (all absolute block numbers, first_data_block=1):
# block 0 = boot block (unused)
# block 1 = superblock
# block 2 = BGD
# block 3 = block bitmap
# block 4 = inode bitmap
# blocks 5..132 = inode table (128 blocks)
# blocks 133.. = data
FIRST_DATA_BLOCK = 133
RESERVED = 10  # reserved blocks count
FREE_BLOCKS = TOTAL_BLOCKS - FIRST_DATA_BLOCK
FREE_INODES = INODES_PER_GROUP - 11  # inodes 1-10 reserved by ext2

def pack_sb():
    sb = bytearray(1024)
    def w32(off, v): struct.pack_into('<I', sb, off, v)
    def w16(off, v): struct.pack_into('<H', sb, off, v)
    w32(0,  INODES_PER_GROUP)   # s_inodes_count
    w32(4,  TOTAL_BLOCKS)       # s_blocks_count
    w32(8,  RESERVED)           # s_r_blocks_count
    w32(12, FREE_BLOCKS)        # s_free_blocks_count
    w32(16, FREE_INODES)        # s_free_inodes_count
    w32(20, 1)                  # s_first_data_block
    w32(24, 0)                  # s_log_block_size (0 = 1 KB)
    w32(28, 0)                  # s_log_frag_size
    w32(32, TOTAL_BLOCKS)       # s_blocks_per_group
    w32(36, TOTAL_BLOCKS)       # s_frags_per_group
    w32(40, INODES_PER_GROUP)   # s_inodes_per_group
    w16(54, 0xEF53)             # s_magic
    w16(56, 1)                  # s_state = clean
    w16(58, 1)                  # s_errors = continue
    w32(76, 1)                  # s_rev_level = 1 (dynamic)
    w32(84, 11)                 # s_first_ino
    w16(88, INODE_SIZE)         # s_inode_size
    w16(90, 0)                  # s_block_group_nr
    sb[72:76] = b'PD-2'        # volume name prefix in last_mounted
    return bytes(sb)

def pack_bgd():
    bgd = bytearray(1024)
    def w32(off, v): struct.pack_into('<I', bgd, off, v)
    def w16(off, v): struct.pack_into('<H', bgd, off, v)
    w32(0, 3)                      # bg_block_bitmap = block 3
    w32(4, 4)                      # bg_inode_bitmap = block 4
    w32(8, 5)                      # bg_inode_table  = block 5
    w16(12, FREE_BLOCKS)           # bg_free_blocks_count
    w16(14, FREE_INODES)           # bg_free_inodes_count
    w16(16, 1)                     # bg_used_dirs_count (root dir)
    return bytes(bgd)

def pack_block_bitmap():
    bm = bytearray(BLOCK_SIZE)
    # Mark blocks 0..FIRST_DATA_BLOCK-1 as used
    for i in range(FIRST_DATA_BLOCK):
        bm[i // 8] |= 1 << (i % 8)
    return bytes(bm)

def pack_inode_bitmap():
    bm = bytearray(BLOCK_SIZE)
    # Inodes 1-10 reserved (bits 0-9), inode 2 = root dir
    for i in range(11):
        bm[i // 8] |= 1 << (i % 8)
    return bytes(bm)

def pack_inode_table():
    table = bytearray(INODE_TABLE_BLOCKS * BLOCK_SIZE)
    # Inode 2 = root directory
    ROOT_OFF = (2 - 1) * INODE_SIZE  # 0-indexed
    def w32(off, v): struct.pack_into('<I', table, ROOT_OFF + off, v)
    def w16(off, v): struct.pack_into('<H', table, ROOT_OFF + off, v)
    w16(0,  0x41ED)    # i_mode: directory, rwxr-xr-x
    w32(4,  BLOCK_SIZE)# i_size
    w16(24, 2)         # i_links_count (. and ..)
    w32(28, 2)         # i_blocks (in 512-byte units)
    w32(40, FIRST_DATA_BLOCK)  # i_block[0] = first data block
    return bytes(table)

def pack_root_dir():
    """Root directory block: . and .. entries only."""
    blk = bytearray(BLOCK_SIZE)
    off = 0
    # Entry: . (inode 2)
    struct.pack_into('<I', blk, off,    2)       # inode
    struct.pack_into('<H', blk, off+4,  12)      # rec_len
    blk[off+6] = 1  # name_len
    blk[off+7] = 2  # file_type DIR
    blk[off+8] = ord('.')
    off += 12
    # Entry: .. (inode 2 — single group, parent of root is root)
    struct.pack_into('<I', blk, off,    2)
    struct.pack_into('<H', blk, off+4,  BLOCK_SIZE - 12)  # rec_len fills rest
    blk[off+6] = 2  # name_len
    blk[off+7] = 2  # file_type DIR
    blk[off+8] = ord('.')
    blk[off+9] = ord('.')
    return bytes(blk)

# Assemble: boot block + superblock + BGD + bitmaps + inode table + root dir
blocks  = bytes(BLOCK_SIZE)       # block 0: boot block
blocks += pack_sb()               # block 1: superblock
blocks += pack_bgd()              # block 2: BGD
blocks += pack_block_bitmap()     # block 3: block bitmap
blocks += pack_inode_bitmap()     # block 4: inode bitmap
blocks += pack_inode_table()      # blocks 5-132: inode table
blocks += pack_root_dir()         # block 133: root dir data
# Pad to 512-byte sector boundary
if len(blocks) % 512:
    blocks += bytes(512 - len(blocks) % 512)

import subprocess
dd = subprocess.Popen(
    ['dd', 'of=build/pd-os.img', 'conv=notrunc', 'bs=512',
     'seek=' + str(BASE_SECTOR)],
    stdin=subprocess.PIPE, stderr=subprocess.DEVNULL
)
dd.communicate(input=blocks)
PYEOF
    echo -e "${GREEN}  [OK] ext2 ready (LBA 4096, 32 MB, root inode 2)${NC}"

    # --- NTFS at LBA 69632 ---
    # Volume: 16384 sectors (8 MB), cluster=512 B (1 sec/cluster)
    # MFT at cluster 4 (LBA 69636), 1024-byte records, 6 system records + 1 test file
    echo -e "${CYAN}  [9] Formatting NTFS at LBA 69632 (8 MB)...${NC}"
    python3 - <<'NTFSEOF'
import struct, sys, subprocess

BASE_LBA   = 69632
TOTAL_SECS = 16384       # 8 MB
BPS        = 512
SPC        = 1           # sectors per cluster (1 cluster = 512 bytes)
MFT_LCN    = 4           # cluster 4 => LBA 69636
MFT_REC    = 1024        # bytes per MFT record
MFT_SECS   = MFT_REC // BPS   # 2 sectors per record

def vbr():
    b = bytearray(512)
    b[0:3]   = b'\xEB\x52\x90'
    b[3:11]  = b'NTFS    '
    struct.pack_into('<H', b, 11, BPS)
    b[13]    = SPC
    struct.pack_into('<H', b, 14, 0)   # reserved sectors
    b[16]    = 0                        # FAT count = 0
    struct.pack_into('<H', b, 17, 0)
    struct.pack_into('<H', b, 19, 0)
    b[21]    = 0xF8
    struct.pack_into('<H', b, 22, 0)
    struct.pack_into('<H', b, 24, 63)
    struct.pack_into('<H', b, 26, 255)
    struct.pack_into('<I', b, 28, 0)
    struct.pack_into('<I', b, 32, 0)
    struct.pack_into('<I', b, 40, 0)
    # NTFS-specific (offset 40+)
    struct.pack_into('<Q', b, 40, TOTAL_SECS - 1)
    struct.pack_into('<Q', b, 48, MFT_LCN)
    struct.pack_into('<Q', b, 56, MFT_LCN + 1)   # MFTMirr
    b[64]    = 0xF6   # clusters_per_mft_record = -10 => 2^10 = 1024 bytes
    b[68]    = 1      # clusters_per_idx_buf
    struct.pack_into('<Q', b, 72, 0x50445F4E544653)  # volume serial
    struct.pack_into('<H', b, 510, 0xAA55)
    return bytes(b)

def fixup(rec, magic):
    """Apply NTFS update-sequence fixup to a 1024-byte record."""
    r = bytearray(rec)
    seq = 0x0001
    struct.pack_into('<H', r, 0x28, seq)  # update sequence number
    # fixup array starts at offset 0x2A, 3 entries: [usn, sec0-end, sec1-end]
    struct.pack_into('<H', r, 0x2A, seq)  # array[0] = usn
    # save & replace last 2 bytes of each 512-byte sector
    for i in range(2):
        off = (i+1)*512 - 2
        orig_lo = r[off]; orig_hi = r[off+1]
        struct.pack_into('<H', r, 0x2C + i*2, (orig_hi << 8) | orig_lo)
        struct.pack_into('<H', r, off, seq)
    return bytes(r)

def mft_hdr(rec_no, flags, attr_offset, used_size):
    h = bytearray(48)
    h[0:4]   = b'FILE'
    struct.pack_into('<H', h, 4,  0x28)   # update_seq_offset
    struct.pack_into('<H', h, 6,  3)      # update_seq_count (usn + 2 sectors)
    struct.pack_into('<Q', h, 8,  0)      # LSN
    struct.pack_into('<H', h, 16, 1)      # sequence_number
    struct.pack_into('<H', h, 18, 1)      # link_count
    struct.pack_into('<H', h, 20, attr_offset)
    struct.pack_into('<H', h, 22, flags)
    struct.pack_into('<I', h, 24, used_size)
    struct.pack_into('<I', h, 28, MFT_REC)
    struct.pack_into('<Q', h, 32, 0)      # base_record_ref
    struct.pack_into('<H', h, 40, 0)
    return bytes(h)

HDR_SZ    = 48   # sizeof mft_hdr output
ATTR_OFF  = 0x38  # first attribute at offset 56 (leave room for USN array)

def attr_resident(atype, value):
    """Build a resident attribute record."""
    name_len    = 0
    hdr_sz      = 24    # ntfs_attr_resident_t
    val_off     = hdr_sz
    val_len     = len(value)
    total       = hdr_sz + val_len
    total       = (total + 7) & ~7   # align to 8 bytes
    a = bytearray(total)
    struct.pack_into('<I', a, 0,  atype)
    struct.pack_into('<I', a, 4,  total)
    a[8]  = 0           # resident
    a[9]  = name_len
    struct.pack_into('<H', a, 10, 18)   # name_offset (not used if name_len=0)
    struct.pack_into('<H', a, 12, 0)    # flags
    struct.pack_into('<H', a, 14, 0)    # attr_id
    struct.pack_into('<I', a, 16, val_len)
    struct.pack_into('<H', a, 20, val_off)
    struct.pack_into('<H', a, 22, 0)    # resident_flags
    a[hdr_sz:hdr_sz+val_len] = value
    return bytes(a)

def attr_end():
    a = bytearray(8)
    struct.pack_into('<I', a, 0, 0xFFFFFFFF)
    struct.pack_into('<I', a, 4, 8)
    return bytes(a)

def file_name_attr(name, parent_ref=5, namespace=3):
    """Build a $FILE_NAME attribute value."""
    utf16 = name.encode('utf-16-le')
    nlen  = len(name)
    # Fixed part: 66 bytes (parent_ref + 4 timestamps + alloc/data sizes +
    #             file_flags + reparse_tag + name_len + namespace)
    v     = bytearray(66 + len(utf16))
    struct.pack_into('<Q', v,  0, parent_ref | (1 << 48))  # parent MFT ref
    # ctime/atime/mtime/rtime at offsets 8,16,24,32 = 0
    struct.pack_into('<Q', v, 40, 0)   # alloc_size
    struct.pack_into('<Q', v, 48, 0)   # data_size
    # file_flags at 56, reparse_tag at 60 = 0
    v[64]  = nlen
    v[65]  = namespace
    v[66:66+len(utf16)] = utf16
    return bytes(v)

def std_info():
    return bytes(48)   # all zeros (timestamps, flags = 0)

def build_record(rec_no, flags, attrs):
    """Assemble a 1024-byte MFT record from a list of (type, value) tuples."""
    rec = bytearray(MFT_REC)
    body = bytearray()
    for (atype, val) in attrs:
        body += attr_resident(atype, val)
    body += attr_end()
    used  = ATTR_OFF + len(body)
    hdr   = mft_hdr(rec_no, flags, ATTR_OFF, used)
    rec[0:len(hdr)]         = hdr
    rec[ATTR_OFF:ATTR_OFF+len(body)] = body
    # USN fixup placeholder at offset 0x28 (3 entries × 2 bytes)
    rec[0x28:0x2E] = b'\x00' * 6
    return fixup(bytes(rec), b'FILE')

# ---- build index entry for root directory $INDEX_ROOT ---
def index_entry(name, mft_ref):
    """One $INDEX_ROOT entry for a file named `name` at `mft_ref`."""
    fn_val = file_name_attr(name, parent_ref=5)
    key_len = len(fn_val)
    # entry: ntfs_index_entry_t (16 bytes) + key
    entry_len = (16 + key_len + 7) & ~7
    e = bytearray(entry_len)
    struct.pack_into('<Q', e, 0, mft_ref | (1 << 48))
    struct.pack_into('<H', e, 8, entry_len)
    struct.pack_into('<H', e, 10, key_len)
    struct.pack_into('<I', e, 12, 0)   # flags: not last
    e[16:16+key_len] = fn_val
    return bytes(e)

def end_entry():
    """Last index entry marker."""
    e = bytearray(16)
    struct.pack_into('<Q', e, 0, 0)
    struct.pack_into('<H', e, 8, 16)
    struct.pack_into('<H', e, 10, 0)
    struct.pack_into('<I', e, 12, 2)   # NTFS_IDX_ENTRY_END
    return bytes(e)

def index_root_value(entries_bytes):
    """Build $INDEX_ROOT value: index_root_t + index_block_hdr_t + entries."""
    # ntfs_index_root_t: 16 bytes
    ir = bytearray(16)
    struct.pack_into('<I', ir, 0, 0x30)  # indexed_attr_type = $FILE_NAME
    struct.pack_into('<I', ir, 4, 1)     # collation_rule = NTFS_COLLATION_FILE_NAME
    struct.pack_into('<I', ir, 8, MFT_REC)
    ir[12] = 1   # clusters_per_block
    # ntfs_index_block_hdr_t: 16 bytes  (entries_offset relative to ibh start)
    entries_offset = 16   # entries start right after ibh
    used = entries_offset + len(entries_bytes)
    ibh = bytearray(16)
    struct.pack_into('<I', ibh, 0, entries_offset)
    struct.pack_into('<I', ibh, 4, used)
    struct.pack_into('<I', ibh, 8, used)
    ibh[12] = 0   # flags: no sub-nodes
    return bytes(ir) + bytes(ibh) + entries_bytes

# ---- Assemble records ------

# MFT record 0: $MFT itself  (minimal — no $DATA runs needed for our driver)
rec0 = build_record(0, 1, [
    (0x10, std_info()),
    (0x30, file_name_attr('$MFT', parent_ref=5)),
])

# MFT record 1: $MFTMirr
rec1 = build_record(1, 1, [
    (0x10, std_info()),
    (0x30, file_name_attr('$MFTMirr', parent_ref=5)),
])

# MFT record 2: $LogFile
rec2 = build_record(2, 1, [
    (0x10, std_info()),
    (0x30, file_name_attr('$LogFile', parent_ref=5)),
])

# MFT record 3: $Volume
rec3 = build_record(3, 1, [
    (0x10, std_info()),
    (0x30, file_name_attr('$Volume', parent_ref=5)),
])

# MFT record 4: $AttrDef
rec4 = build_record(4, 1, [
    (0x10, std_info()),
    (0x30, file_name_attr('$AttrDef', parent_ref=5)),
])

# MFT record 6: test file "hello.txt" with resident $DATA
HELLO_DATA = b'Hello from NTFS!\n'
rec6 = build_record(6, 1, [
    (0x10, std_info()),
    (0x30, file_name_attr('hello.txt', parent_ref=5)),
    (0x80, HELLO_DATA),
])

# MFT record 5: root directory with $INDEX_ROOT pointing at record 6
idx_entries = index_entry('hello.txt', 6) + end_entry()
irv = index_root_value(idx_entries)
rec5 = build_record(5, 3, [   # flags=3: IN_USE | IS_DIR
    (0x10, std_info()),
    (0x30, file_name_attr('.', parent_ref=5)),
    (0x90, irv),
])

# Pack all 7 records (0-6) into MFT, located at cluster MFT_LCN = 4
mft_data = rec0 + rec1 + rec2 + rec3 + rec4 + rec5 + rec6
# MFT starts at LBA BASE_LBA + MFT_LCN * SPC
mft_offset_secs = MFT_LCN * SPC

# Build the full sector buffer: VBR at sec 0, MFT at sec mft_offset_secs
total_buf = bytearray(TOTAL_SECS * BPS)
total_buf[0:BPS]            = vbr()
mft_off   = mft_offset_secs * BPS
total_buf[mft_off:mft_off + len(mft_data)] = mft_data

dd = subprocess.Popen(
    ['dd', 'of=build/pd-os.img', 'conv=notrunc', 'bs=512',
     'seek=' + str(BASE_LBA)],
    stdin=subprocess.PIPE, stderr=subprocess.DEVNULL
)
dd.communicate(input=bytes(total_buf))
NTFSEOF
    echo -e "${GREEN}  [OK] NTFS ready (LBA 69632, 8 MB, hello.txt in root)${NC}"
}

case "$TARGET" in
    all)
        build
        echo -e "${GREEN}=== Build complete! ===${NC}"
        echo "Run with: ./build.sh run"
        ;;
    run)
        build
        echo -e "${CYAN}Starting QEMU...${NC}"
        echo "Press Ctrl+Alt+G to release mouse, Ctrl+C to quit"
        qemu-system-i386 -drive format=raw,file="$DISK_IMAGE" -m 128M
        ;;
    run-debug)
        build
        echo -e "${CYAN}Starting QEMU (debug mode)...${NC}"
        qemu-system-i386 -drive format=raw,file="$DISK_IMAGE" -m 128M \
            -serial stdio -no-reboot -d cpu_reset
        ;;
    clean)
        if [ -d "$BUILD_DIR" ]; then
            rm -rf "$BUILD_DIR"
            echo -e "${GREEN}[OK] Cleaned${NC}"
        else
            echo "Nothing to clean."
        fi
        ;;
    setup-check)
        echo "=== Verifying PD-OS Toolchain Setup ==="
        echo ""
        for tool in nasm i686-linux-gnu-gcc i686-linux-gnu-ld i686-linux-gnu-objcopy qemu-system-i386; do
            printf "Checking %-30s " "$tool..."
            if command -v "$tool" > /dev/null 2>&1; then
                echo -e "${GREEN}OK${NC}"
            else
                echo -e "${RED}NOT FOUND${NC}"
            fi
        done
        echo ""
        echo "If any tools are missing, run: ./tools/autosetup.sh"
        ;;
    *)
        echo "PD-OS Build System (Linux)"
        echo "Usage: ./build.sh [all|run|run-debug|clean|setup-check]"
        ;;
esac
