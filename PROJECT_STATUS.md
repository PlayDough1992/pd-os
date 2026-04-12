# PD-OS Project Status

**Last Updated**: April 12, 2026
**Current Phase**: Phase 9a Complete ✅
**Version**: 0.1.0
**Kernel Size**: 59,104 bytes | Zero warnings

---

## ✅ Completed Phases

### Phase 1 — Environment Setup ✓
- Project directory structure
- `build.sh` — unified Linux build script
- `commit.sh` — commits to `linux-build-env` then syncs `main`
- `.gitignore`, toolchain guide (`tools/setup.md`)

---

### Phase 2 — PD-Bootloader Stage 1 (MBR) ✓
- 512-byte MBR (`bootloader/stage1.asm`)
- Loads Stage 2 from LBA 1–5 via BIOS INT 13h
- Boot signature 0xAA55

---

### Phase 3 — PD-Bootloader Stage 2 ✓
- A20 line enable
- GDT setup + 32-bit protected mode transition
- E820 memory map query (passed to kernel via well-known address)
- INT 13h Extended Read — loads 128 sectors (64 KB) from LBA 6
- Copies kernel from 0x10000 → 0x100000 (above 1 MB)
- TUI boot menu with countdown timer and keyboard selection
- Jumps to kernel entry point

---

### Phase 4 — Kernel Foundation ✓
- Kernel entry point (`kernel/arch/x86/entry.asm`)
- VGA text driver — 80×25 color, hardware cursor, scroll
- `kprintf` with `%s %u %x %c` format support
- Kernel panic handler (`PANIC:` halt)

---

### Phase 5 — Interrupts ✓
- IDT (256 gates, `kernel/arch/x86/idt.asm`)
- 8259A dual-PIC remapping (IRQs → vectors 0x20–0x2F)
- PIT at 100 Hz (`pit_get_ticks()`)
- PS/2 keyboard driver — IRQ1, Set 1 scancodes, shift/caps/backspace
- Readline with mid-line editing (left/right arrows, Home/End, Delete)

---

### Phase 6 — User System + PD-Shell Tier 1 ✓
- Login screen with 3-attempt lockout
- FNV-1a 32-bit password hashing (plaintext zeroed after boot)
- User table: `root` (uid 0, `USER_FLAG_ROOT`), `pd` (uid 1)
- PD-Shell: `help`, `clear`, `echo`, `version`, `uptime`, `color`, `whoami`, `logout`, `reboot`, `shutdown`

---

### Phase 7 — Memory Management ✓
- E820 map parsed at boot
- Bitmap PMM — 4 KB pages, free/used tracking
- Kernel-space paging — identity map + MMIO (VGA)
- Kernel heap — `kmalloc` / `kfree` with split/merge
- Shell commands: `memmap`, `meminfo`, `heap`

---

### Phase 8 — Storage & Filesystem ✓

#### ATA/IDE Driver (`kernel/drivers/ata.c`)
- 28-bit LBA PIO, primary channel, master drive, polling
- Single-sector reads and writes
- Cache flush (`0xE7`) after every write
- **Safety guard**: refuses writes to LBA < 200 (protects bootloader/kernel)

#### VFS Layer (`kernel/fs/vfs.c`)
- Driver registry (up to 8 drivers)
- Mount table with longest-prefix path dispatch
- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_unlink`, `vfs_readdir`

#### PDFS v2 — Native Filesystem (`kernel/fs/pdfs.c`)
- **On-disk layout** (base LBA 200):
  - LBA 200: Superblock (magic, version, dir_lba, next_free_lba…)
  - LBA 201: Reserved (journal slot, currently unused)
  - LBA 202–205: Root directory (4 sectors, 32 × 64-byte dirents)
  - LBA 206+: File and subdir data (contiguous, sector-aligned)
- 64-byte dirents: name (27 chars), start_lba, size, alloc_sectors, flags, uid, gid, mode, ctime, dir_sectors
- Subdirectory support — each dir has its own 4-sector table
- Unix rwxrwxrwx permissions (owner/group uid + mode bits)
- Monotonic `next_free_lba` allocator
- `flush_dir_slot` — writes only the single sector containing the changed dirent
- `pdfs_stat_dir(path, idx, out)` — enumerate any directory
- Permission context: `pdfs_set_context(caller, elevated)`
- v1 disks mount read-only; v2 disks read/write
- Shell commands: `ls`, `cat`, `write`, `rm`, `mkdir`, `mkpdfs`, `setp`, `seto`

#### Read-Only Drivers
- FAT32 (`kernel/fs/fat32.c`) — mounted at `/mnt/fat` (LBA 2048)
- ext2 (`kernel/fs/ext2.c`) — mounted at `/mnt/ext2` (LBA 4096)
- NTFS (`kernel/fs/ntfs.c`) — mounted at `/mnt/ntfs` (LBA 69632)

---

### Phase 9a — PD-Shell Tier 2 ✓

#### CWD & Path Resolution
- `g_cwd[128]` — session current-working-directory state
- `normalize_path(input, out)` — canonical resolver:
  - `~` → `/home/<username>`
  - `../` — walks up from CWD
  - `/absolute` — used as-is
  - `relative` — prepended with CWD
- `make_path(out, name)` — wraps `normalize_path`; used by all commands

#### New Commands
| Command | Description |
|---------|-------------|
| `sdir [path]` | Change directory (`~`, `..`, `/abs`, `relative`) |
| `copy <src> <dst>` | Copy a file |
| `move <src> <dst>` | Move / rename a file |
| `setp <file> <oct>` | Set permissions (e.g. `setp f.txt 644`) |
| `seto <file> <u>:<g>` | Set owner (e.g. `seto f.txt pd:pd`) |
| `elev <cmd>` | Run command with elevated (root) privileges |

#### Prompt
`username@pd-shell:/cwd> ` — CWD displayed in cyan

---

## 🐛 Notable Bugs Fixed

### MBR Corruption on Any Disk Write — FIXED (April 12, 2026)
**Root cause**: `sizeof(pdfs_dirent_t)` was 60 bytes (not 64 as intended). `reserved` was `uint32_t` (4B) instead of `uint32_t[2]` (8B). `g_dir[32]` = 1920 bytes. `pdfs_mount` reads 4 sectors (2048 bytes) into it → 128-byte BSS overflow → `g_base_lba` zeroed → every `flush_sb()` wrote the PDFS superblock to LBA 0 (MBR), destroying the boot signature.

**Fix**: `reserved[2]` in `pdfs_dirent_t` (struct now exactly 64B). ATA guard added (refuse writes to LBA < 200).

### Keyboard Dead After Reboot — FIXED (April 12, 2026)
**Root cause**: PS/2 controller sends 0xAA (BAT completion) byte into output buffer after system reset. `keyboard_init` unmasked IRQ1 without draining the buffer first — stale byte prevented new IRQ1 firings.

**Fix**: Drain PS/2 output buffer (`while (inb(0x64) & 0x01) inb(0x60)`) before `pic_unmask_irq(1)`.

### mkdir Corrupting Disk (Journal Overhead) — FIXED (April 12, 2026)
**Root cause**: Previous `flush_dir_at` wrote all 4 directory sectors wrapped in a journal (begin + write + commit per sector) = 17 ATA writes to overlapping LBAs per `mkdir`, overwhelming QEMU's ATA emulation.

**Fix**: Removed journal entirely. `flush_dir_slot` writes exactly one sector (the one containing the changed dirent).

---

## 📊 Progress

```
Phase 1:  ████████████████████ 100% ✅ Environment & build system
Phase 2:  ████████████████████ 100% ✅ Bootloader Stage 1 (MBR)
Phase 3:  ████████████████████ 100% ✅ Bootloader Stage 2 (PM, TUI, E820)
Phase 4:  ████████████████████ 100% ✅ Kernel foundation (VGA, kprintf)
Phase 5:  ████████████████████ 100% ✅ IDT, PIC, PIT, keyboard
Phase 6:  ████████████████████ 100% ✅ User system, PD-Shell Tier 1
Phase 7:  ████████████████████ 100% ✅ PMM, paging, heap
Phase 8:  ████████████████████ 100% ✅ ATA, VFS, PDFS v2, FAT32/ext2/NTFS
Phase 9a: ████████████████████ 100% ✅ Shell Tier 2 (sdir/copy/move/CWD)
Phase 9b: ░░░░░░░░░░░░░░░░░░░░   0%  ⬜ History, tab completion
Phase 10: ░░░░░░░░░░░░░░░░░░░░   0%  ⬜ Process management
GUI:      ░░░░░░░░░░░░░░░░░░░░   0%  ⬜ VESA + desktop (future)

Overall:  ████████████████░░░░  80%
```

### Component Status

| Component | Status | Phase |
|-----------|--------|-------|
| Build system (`build.sh`) | ✅ Complete | 1 |
| Bootloader Stage 1 | ✅ Complete | 2 |
| Bootloader Stage 2 | ✅ Complete | 3 |
| VGA text driver | ✅ Complete | 4 |
| kprintf / panic | ✅ Complete | 4 |
| IDT / exceptions | ✅ Complete | 5 |
| PIC / PIT (100 Hz) | ✅ Complete | 5 |
| PS/2 keyboard driver | ✅ Complete | 5 |
| Readline (mid-line edit) | ✅ Complete | 5 |
| User accounts / login | ✅ Complete | 6 |
| PD-Shell Tier 1 | ✅ Complete | 6 |
| E820 memory map | ✅ Complete | 7 |
| Bitmap PMM | ✅ Complete | 7 |
| Paging | ✅ Complete | 7 |
| Kernel heap (kmalloc/kfree) | ✅ Complete | 7 |
| ATA/IDE PIO driver | ✅ Complete | 8 |
| VFS layer | ✅ Complete | 8 |
| PDFS v2 (R/W + permissions) | ✅ Complete | 8 |
| FAT32 read-only driver | ✅ Complete | 8 |
| ext2 read-only driver | ✅ Complete | 8 |
| NTFS read-only driver | ✅ Complete | 8 |
| PD-Shell Tier 2 (26 cmds) | ✅ Complete | 9a |
| CWD + normalize_path | ✅ Complete | 9a |
| copy / move commands | ✅ Complete | 9a |
| setp / seto (chmod/chown) | ✅ Complete | 9a |
| Command history (↑/↓) | ⬜ Not started | 9b |
| Tab completion | ⬜ Not started | 9b |
| Process management | ⬜ Not started | 10 |
| GUI / VESA framebuffer | ⬜ Not started | Future |

---

## ⬜ Next Up — Phase 9b

- **Command history** — ring buffer for previous commands, ↑/↓ arrows to navigate (readline already stubs `KEY_UP`/`KEY_DOWN`)
- **Tab completion** — enumerate `g_cwd` via `pdfs_stat_dir` while typing, complete on unique prefix

---

_Last updated automatically after each phase completion._
