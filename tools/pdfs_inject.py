#!/usr/bin/env python3
"""
pdfs_inject.py  —  PD-OS PDFS v3 host-side file injector
==========================================================
Writes (or overwrites) a single file into a PDFS v3 partition inside a
raw disk image WITHOUT running the kernel.

Usage:
    pdfs_inject.py <disk.img> <pdfs_start_sector> <dest_path> <src_file>

    disk.img           Raw disk image (e.g. build/pd-os-gde.img)
    pdfs_start_sector  Absolute LBA where PDFS superblock lives (usually 1024)
    dest_path          Absolute PDFS path (e.g. /sys/de/example_de.bin)
                       Intermediate directories are created if absent.
    src_file           Host file whose contents to write, OR "-" for stdin.

The tool speaks the PDFS v3 wire format as defined in kernel/include/pdfs.h:
  Superblock: 512 bytes at base_lba+0
  Root dir:   chain of 512-byte sectors, each holding 8×64-byte dirents
  Slot 7 of each dir sector = chain-link to next sector (PDFS_FLAG_CHAIN=0x04)
  Data sectors allocated from superblock.next_free_lba (monotonic bump allocator)

Dirent layout (64 bytes, little-endian):
  name[28]          NUL-terminated filename
  start_lba  u32    first data sector
  size       u32    file size in bytes
  alloc_sec  u32    allocated sectors
  flags      u8     0x01=USED 0x02=DIR 0x04=CHAIN
  uid        u8
  gid        u8
  mode       u16    low 9 bits Unix style
  ctime      u32
  dir_sec    u32    (dirs: sector count)
  reserved   u32[2]

Superblock layout (512 bytes):
  magic      u32    0x50444653 ("PDFS")
  version    u32    3
  dir_lba    u32    root dir LBA
  dir_sec    u32    root dir sector count
  data_lba   u32    data start LBA
  free_lba   u32    next free LBA (monotonic allocator)
  jrnl_lba   u32    journal LBA
  reserved   u32×121
"""

import sys
import struct
import os
import math

SECTOR_SIZE      = 512
PDFS_MAGIC       = 0x50444653
PDFS_VERSION     = 3
PDFS_NAME_LEN    = 28
PDFS_CHAIN_SLOTS = 7     # usable slots per dir sector (slots 0-6)
PDFS_CHAIN_LINK  = 7     # slot index of chain link
PDFS_FLAG_USED   = 0x01
PDFS_FLAG_DIR    = 0x02
PDFS_FLAG_CHAIN  = 0x04
PDFS_MODE_DEFAULT= 0x1B4  # rw-r--r--
PDFS_MODE_DIR    = 0x1ED  # rwxr-xr-x

# Struct formats (little-endian)
SB_FMT     = "<IIIIIII121I"   # superblock
DIRENT_FMT = "<28sIIIBBBHII2I" # name+fields, then reserved[2]


# ---- low-level disk I/O ---------------------------------------------------

def read_sector(f, lba):
    f.seek(lba * SECTOR_SIZE)
    return bytearray(f.read(SECTOR_SIZE))

def write_sector(f, lba, data):
    assert len(data) == SECTOR_SIZE
    f.seek(lba * SECTOR_SIZE)
    f.write(bytes(data))


# ---- superblock -----------------------------------------------------------

def read_sb(f, base_lba):
    raw = read_sector(f, base_lba)
    fields = struct.unpack_from(SB_FMT, raw, 0)
    return {
        'magic':    fields[0],
        'version':  fields[1],
        'dir_lba':  fields[2],
        'dir_sec':  fields[3],
        'data_lba': fields[4],
        'free_lba': fields[5],
        'jrnl_lba': fields[6],
    }

def write_sb(f, base_lba, sb):
    raw = struct.pack(SB_FMT,
        sb['magic'], sb['version'], sb['dir_lba'], sb['dir_sec'],
        sb['data_lba'], sb['free_lba'], sb['jrnl_lba'],
        *([0] * 121))
    write_sector(f, base_lba, bytearray(raw))


# ---- dirent helpers -------------------------------------------------------

DIRENT_SIZE = 64

def parse_dirent(raw, offset=0):
    """Parse one 64-byte dirent from raw bytes at offset."""
    # name[28], start_lba, size, alloc_sectors, flags(B), uid(B), gid(B), mode(H),
    # ctime, dir_sectors, reserved[0], reserved[1]
    name_b = raw[offset:offset+28]
    name   = name_b.split(b'\x00')[0].decode('ascii', errors='replace')
    start_lba, size, alloc_sec, flags, uid, gid, mode, ctime, dir_sec, r0, r1 = \
        struct.unpack_from("<IIIBBBHII2I"[0:], raw, offset+28)
    # re-unpack cleanly
    (start_lba,) = struct.unpack_from("<I", raw, offset+28)
    (size,)      = struct.unpack_from("<I", raw, offset+32)
    (alloc_sec,) = struct.unpack_from("<I", raw, offset+36)
    flags  = raw[offset+40]
    uid    = raw[offset+41]
    gid    = raw[offset+42]
    (mode,)= struct.unpack_from("<H", raw, offset+43)
    (ctime,)    = struct.unpack_from("<I", raw, offset+45)
    (dir_sec,)  = struct.unpack_from("<I", raw, offset+49)
    return {
        'name': name, 'start_lba': start_lba, 'size': size,
        'alloc_sec': alloc_sec, 'flags': flags, 'uid': uid, 'gid': gid,
        'mode': mode, 'ctime': ctime, 'dir_sec': dir_sec,
    }

def pack_dirent(d):
    """Pack a dirent dict into 64 bytes."""
    name_b = d['name'].encode('ascii')[:27].ljust(28, b'\x00')
    raw = bytearray(64)
    raw[0:28] = name_b
    struct.pack_into("<I", raw, 28, d['start_lba'])
    struct.pack_into("<I", raw, 32, d['size'])
    struct.pack_into("<I", raw, 36, d['alloc_sec'])
    raw[40] = d['flags']
    raw[41] = d['uid']
    raw[42] = d['gid']
    struct.pack_into("<H", raw, 43, d['mode'])
    struct.pack_into("<I", raw, 45, d['ctime'])
    struct.pack_into("<I", raw, 49, d['dir_sec'])
    # reserved[0..1] at 53, 57 — leave as zero
    return raw

def read_dir_sector(f, lba):
    """Read 8 dirents from a dir sector."""
    raw = read_sector(f, lba)
    dirents = []
    for i in range(8):
        dirents.append(parse_dirent(raw, i * 64))
    return dirents

def write_dir_sector(f, lba, dirents):
    assert len(dirents) == 8
    raw = bytearray(512)
    for i, d in enumerate(dirents):
        raw[i*64:(i+1)*64] = pack_dirent(d)
    write_sector(f, lba, raw)


# ---- directory traversal --------------------------------------------------

def dir_find(f, first_lba, name):
    """Search a dir chain for `name`. Returns (sector_lba, slot_idx, dirent) or None."""
    lba = first_lba
    while lba:
        dirents = read_dir_sector(f, lba)
        for i in range(PDFS_CHAIN_SLOTS):
            d = dirents[i]
            if (d['flags'] & PDFS_FLAG_USED) and not (d['flags'] & PDFS_FLAG_CHAIN):
                if d['name'] == name:
                    return (lba, i, d)
        # Follow chain link
        link = dirents[PDFS_CHAIN_LINK]
        lba = link['start_lba'] if (link['flags'] & PDFS_FLAG_CHAIN) else 0
    return None

def dir_alloc_slot(f, sb, base_lba, first_lba):
    """Find or create a free slot in the dir chain. Returns (sector_lba, slot_idx)."""
    lba = first_lba
    prev_lba = None
    while lba:
        dirents = read_dir_sector(f, lba)
        for i in range(PDFS_CHAIN_SLOTS):
            if not (dirents[i]['flags'] & PDFS_FLAG_USED):
                return (lba, i)
        link = dirents[PDFS_CHAIN_LINK]
        if link['flags'] & PDFS_FLAG_CHAIN:
            prev_lba = lba
            lba = link['start_lba']
        else:
            prev_lba = lba
            lba = 0

    # Allocate a new dir sector
    new_lba = sb['free_lba']
    sb['free_lba'] += 1
    write_sb(f, base_lba, sb)

    # Zero out the new sector
    write_sector(f, new_lba, bytearray(512))

    # Link from prev_lba slot 7
    dirents = read_dir_sector(f, prev_lba)
    dirents[PDFS_CHAIN_LINK] = {
        'name': '', 'start_lba': new_lba, 'size': 0, 'alloc_sec': 0,
        'flags': PDFS_FLAG_CHAIN, 'uid': 0, 'gid': 0,
        'mode': 0, 'ctime': 0, 'dir_sec': 0,
    }
    write_dir_sector(f, prev_lba, dirents)

    return (new_lba, 0)

def ensure_dir(f, sb, base_lba, parent_dir_lba, name):
    """Ensure a subdirectory `name` exists under parent. Returns its dir LBA."""
    hit = dir_find(f, parent_dir_lba, name)
    if hit:
        d = hit[2]
        if d['flags'] & PDFS_FLAG_DIR:
            return d['start_lba']
        else:
            raise RuntimeError(f"'{name}' exists but is a file, not a dir")

    # Create the directory
    new_dir_lba = sb['free_lba']
    sb['free_lba'] += 1
    write_sb(f, base_lba, sb)
    write_sector(f, new_dir_lba, bytearray(512))   # empty dir sector

    # Add dirent in parent
    sec_lba, slot = dir_alloc_slot(f, sb, base_lba, parent_dir_lba)
    dirents = read_dir_sector(f, sec_lba)
    dirents[slot] = {
        'name': name, 'start_lba': new_dir_lba, 'size': 0, 'alloc_sec': 1,
        'flags': PDFS_FLAG_USED | PDFS_FLAG_DIR, 'uid': 0, 'gid': 0,
        'mode': PDFS_MODE_DIR, 'ctime': 0, 'dir_sec': 1,
    }
    write_dir_sector(f, sec_lba, dirents)
    print(f"  [mkdir] /{name}")
    return new_dir_lba


# ---- file write -----------------------------------------------------------

def write_file(f, sb, base_lba, dir_lba, name, data):
    """Write `data` bytes as file `name` in the directory at dir_lba."""
    n_sectors = max(1, math.ceil(len(data) / SECTOR_SIZE))

    # Allocate data sectors
    data_lba = sb['free_lba']
    old_data_lba = None

    # Check if file already exists — overwrite in place if same size fits,
    # otherwise free old sectors and reallocate (simple approach: always realloc)
    hit = dir_find(f, dir_lba, name)
    if hit:
        sec_lba, slot, old_d = hit
        old_data_lba = old_d['start_lba']
        # For simplicity we just allocate new sectors (PDFS uses monotonic alloc)
        # Old sectors become "lost" space — that's fine for an install tool.
    
    # Allocate new sectors from free pool
    data_lba = sb['free_lba']
    sb['free_lba'] += n_sectors
    write_sb(f, base_lba, sb)

    # Write data sectors (pad last with zeros)
    padded = data.ljust(n_sectors * SECTOR_SIZE, b'\x00')
    for i in range(n_sectors):
        chunk = bytearray(padded[i*SECTOR_SIZE:(i+1)*SECTOR_SIZE])
        write_sector(f, data_lba + i, chunk)

    # Write or update dirent
    if hit:
        sec_lba, slot, _ = hit
        dirents = read_dir_sector(f, sec_lba)
        dirents[slot]['start_lba'] = data_lba
        dirents[slot]['size']      = len(data)
        dirents[slot]['alloc_sec'] = n_sectors
        write_dir_sector(f, sec_lba, dirents)
    else:
        sec_lba, slot = dir_alloc_slot(f, sb, base_lba, dir_lba)
        dirents = read_dir_sector(f, sec_lba)
        dirents[slot] = {
            'name': name, 'start_lba': data_lba, 'size': len(data),
            'alloc_sec': n_sectors, 'flags': PDFS_FLAG_USED,
            'uid': 0, 'gid': 0, 'mode': PDFS_MODE_DEFAULT,
            'ctime': 0, 'dir_sec': 0,
        }
        write_dir_sector(f, sec_lba, dirents)


# ---- main -----------------------------------------------------------------

def main():
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(1)

    disk_path   = sys.argv[1]
    base_lba    = int(sys.argv[2])
    dest_path   = sys.argv[3]
    src_path    = sys.argv[4]

    # Read source data
    if src_path == '-':
        data = sys.stdin.buffer.read()
    else:
        with open(src_path, 'rb') as sf:
            data = sf.read()

    with open(disk_path, 'r+b') as f:
        sb = read_sb(f, base_lba)

        if sb['magic'] != PDFS_MAGIC:
            print(f"ERROR: PDFS magic not found at sector {base_lba}.")
            print(f"       Expected 0x{PDFS_MAGIC:08X}, got 0x{sb['magic']:08X}")
            print("       Run ./build_gde.sh all and boot once to format PDFS.")
            sys.exit(1)

        if sb['version'] != PDFS_VERSION:
            print(f"ERROR: PDFS version {sb['version']}, expected {PDFS_VERSION}")
            sys.exit(1)

        # Parse path components
        parts = [p for p in dest_path.strip('/').split('/') if p]
        if not parts:
            print("ERROR: destination path is empty")
            sys.exit(1)

        filename  = parts[-1]
        dir_parts = parts[:-1]

        # Traverse / create intermediate directories
        cur_dir_lba = sb['dir_lba']
        for d in dir_parts:
            cur_dir_lba = ensure_dir(f, sb, base_lba, cur_dir_lba, d)

        # Write the file
        write_file(f, sb, base_lba, cur_dir_lba, filename, data)
        print(f"  [write] {dest_path}  ({len(data)} bytes)")

    print(f"Done.")


if __name__ == '__main__':
    main()
