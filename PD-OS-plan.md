# PD-OS Implementation Plan

## Project Overview
Custom 32-bit operating system with custom bootloader (PD-Bootloader), kernel (PD-Kernel), CLI, and eventual GUI.

**Target**: 32-bit x86 (i686) — 64-bit version planned for the future  
**Environment**: Linux (Debian/Ubuntu) with `i686-linux-gnu-gcc` cross-compiler  
**Build**: `bash build.sh` → `qemu-system-i386 -drive format=raw,file=build/pd-os.img -m 128M -display sdl`  
**Branches**: `linux-build-env` (work) → `main` (stable, synced via `commit.sh`)

---

## PD-OS Virtual Filesystem Structure

This is the directory layout visible to the user inside PDFS. It follows the Debian/Ubuntu standard hierarchy close enough to support running Debian-based software natively in the future, while using PD-OS-unique names for directories we fully own.

```
/ (PDFS root)
│
├── bin/               # Essential user binaries (Debian compat — future)
├── sbin/              # System administration binaries (Debian compat — future)
├── lib/               # Shared libraries (Debian compat — future)
├── lib32/             # 32-bit libraries (Debian compat — future)
│
├── etc/               # System-wide configuration files (standard)
│   ├── pd-os/         # PD-OS specific config (hostname, version, etc.)
│   └── passwd         # User account database (future — currently in-kernel)
│
├── home/              # User home directories (standard — needed now)
│   └── <username>/    # Per-user home (e.g. /home/pd, /home/root)
│
├── root/              # Root user home directory (standard)
│
├── dev/               # Device files (standard — future devfs)
│
├── proc/              # Process information (standard — future procfs)
│
├── sys/               # Kernel/hardware info (standard — future sysfs)
│
├── tmp/               # Temporary files (standard)
│
├── var/               # Variable data — logs, caches, spool (standard)
│   ├── log/
│   └── tmp/
│
├── usr/               # Secondary hierarchy (Debian compat — future)
│   ├── bin/           # Non-essential user binaries
│   ├── sbin/          # Non-essential system binaries
│   ├── lib/           # Libraries for /usr/bin and /usr/sbin
│   └── share/         # Architecture-independent data
│
├── mnt/               # Mount points for other filesystems (standard)
│   ├── fat/           # FAT32 volume (LBA 2048)
│   ├── ext2/          # ext2 volume (LBA 4096)
│   └── ntfs/          # NTFS volume (LBA 69632)
│
├── pdsys/             # PD-OS core system files (unique to PD-OS)
│   ├── kernel/        # Kernel modules and extensions (future)
│   ├── drivers/       # Loadable driver binaries (future)
│   └── version        # PD-OS version string
│
└── pdapps/            # PD-OS native applications (unique to PD-OS)
    ├── system/        # Built-in system apps (text editor, etc.)
    └── user/          # User-installed PD-OS apps
```

**Design rationale:**
- `bin/`, `sbin/`, `lib/`, `usr/`, `etc/`, `home/`, `dev/`, `proc/`, `sys/`, `var/` — kept exactly as Debian expects so that future Debian binary compatibility works without path remapping
- `pdsys/` — PD-OS owned system directory, no conflict with Debian packages
- `pdapps/` — PD-OS native app store / install target, separate from Debian package paths
- `/etc/pd-os/` — PD-OS config lives under the standard `/etc` tree but in its own subdir

---

## Phase 1: Environment Setup ✅ COMPLETE

**Completed**: April 11, 2026

- Project directory structure created
- `build.sh` — unified Linux build script (assembles, compiles, links, builds disk image with all 4 filesystems)
- `commit.sh` — commits to `linux-build-env`, merges to `main`, pushes both
- `Makefile` — thin wrapper around build.sh
- `tools/setup.md`, `tools/autosetup.sh` — toolchain guides
- **Toolchain**: NASM + `i686-linux-gnu-gcc` + QEMU + Python 3

---

## Phase 2: PD-Bootloader Stage 1 (MBR) ✅ COMPLETE

**Completed**: April 11, 2026

- 512-byte MBR (`bootloader/stage1.asm`)
- Initializes segments and stack in 16-bit real mode
- Loads Stage 2 (5 sectors from LBA 1–5) via BIOS INT 13h
- Boot signature 0xAA55 at bytes 510–511
- Boots successfully in QEMU

---

## Phase 3: PD-Bootloader Stage 2 ✅ COMPLETE

**Completed**: April 11, 2026

- A20 line enable (keyboard controller method)
- GDT setup (null + 32-bit code + 32-bit data descriptors)
- 32-bit protected mode transition (`CR0.PE = 1`)
- E820 BIOS memory map query — results stored at well-known address for kernel
- INT 13h Extended Read (AH=0x42) — loads 128 sectors (64 KB) from LBA 6 to 0x10000
- Copies kernel: 0x10000 → 0x100000 (above 1 MB, using 32-bit copy loop)
- TUI boot menu with 8-second countdown, keyboard UP/DOWN/ENTER/1/2 selection
- "Boot PD-OS" (default) and "Enter BIOS Setup" options
- Far jump to kernel entry point at 0x100000

---

## Phase 4: PD-Kernel Foundation ✅ COMPLETE

**Completed**: April 11, 2026

- Kernel entry point (`kernel/arch/x86/entry.asm`) — sets up 32-bit stack, calls `kernel_main`
- VGA text driver (`kernel/drivers/vga.c`) — 80×25 color text, hardware cursor, scroll, color API
- `kprintf` — `%s`, `%u`, `%x`, `%c`, `%d` format support
- Kernel panic handler — prints `PANIC:` + message, halts

---

## Phase 5: Interrupts & Input ✅ COMPLETE

**Completed**: April 11, 2026

- IDT — 256 gates (`kernel/arch/x86/idt.asm` LIDT macro + C setup)
- 8259A dual-PIC remapping — IRQs 0–15 → vectors 0x20–0x2F
- PIT at 100 Hz — `pit_get_ticks()` for uptime and timestamps
- PS/2 keyboard driver (IRQ1, Set 1 scancodes) — shift, caps lock, backspace, arrows
- Readline — mid-line editing: left/right move cursor, Home/End, Delete, Insert-mode overwrite
- PS/2 output buffer drain on `keyboard_init` (fixes keyboard-dead-after-reboot bug)

---

## Phase 6: User System + PD-Shell Tier 1 ✅ COMPLETE

**Completed**: April 11, 2026

- Login screen — 3-attempt lockout, then 30-second timeout
- FNV-1a 32-bit password hashing — plaintext zeroed from RAM after hashing
- User table: `root` (uid 0, `USER_FLAG_ROOT`), `pd` (uid 1)
- Session state: `g_session_user`, `g_elevated`
- PD-Shell readline loop with color prompt
- Built-ins: `help`, `clear`, `echo`, `version`, `uptime`, `color`, `whoami`, `logout`, `reboot`, `shutdown`

---

## Phase 7: Memory Management ✅ COMPLETE

**Completed**: April 11, 2026

- E820 memory map parsed from bootloader-provided address at boot
- Bitmap PMM — 4 KB page frames, `pmm_alloc_page()` / `pmm_free_page()`
- Kernel-space paging — identity map 0–4 MB + VGA MMIO region
- Kernel heap (`kernel/mm/kheap.c`) — `kmalloc` / `kfree` with split and merge
- Shell commands: `memmap` (E820 table), `meminfo` (page counts), `heap` (heap stats)

---

## Phase 8: Storage & Filesystem ✅ COMPLETE

**Completed**: April 11–12, 2026

### ATA/IDE PIO Driver (`kernel/drivers/ata.c`)
- 28-bit LBA PIO mode, primary channel, master drive, polling (no DMA)
- Single-sector reads (`ata_read_sectors`) and writes (`ata_write_sectors`)
- `ATA_CMD_CACHE_FLUSH` (0xE7) sent after every write sector
- **Safety guard**: `ATA_RESERVED_LBA = 200` — any write to LBA < 200 returns error, protecting the bootloader and kernel image from accidental overwrite

### VFS Layer (`kernel/fs/vfs.c`)
- Driver registry (up to 8 filesystem drivers)
- Mount table — longest-prefix path dispatch
- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_create`, `vfs_unlink`, `vfs_readdir`

### PDFS v2 — Native R/W Filesystem (`kernel/fs/pdfs.c`)
**On-disk layout** (mounted at LBA 200):
```
LBA 200       Superblock (512 B)  — magic, version, dir_lba, next_free_lba, jrnl_lba
LBA 201       Reserved            — (journal slot, currently unused)
LBA 202–205   Root directory      — 4 sectors × 8 dirents = 32 entries × 64 bytes each
LBA 206+      File / subdir data  — contiguous, sector-aligned, monotonic allocator
```
- 64-byte dirents: name[28], start_lba, size, alloc_sectors, flags, uid, gid, mode, ctime, dir_sectors, reserved[2]
- Subdirectory support — each dir gets its own 4-sector table
- Unix rwxrwxrwx permissions (uid/gid + 9-bit mode)
- Monotonic `next_free_lba` allocator
- `flush_dir_slot` — targeted single-sector write (only the sector containing the changed dirent)
- `pdfs_stat_dir(path, idx, out)` — enumerate any directory by path
- Permission context API: `pdfs_set_context(caller, elevated)`
- v1 disks mount read-only; v2 disks read/write

### Read-Only Drivers
- FAT32 (`kernel/fs/fat32.c`) — `/mnt/fat` (LBA 2048)
- ext2 (`kernel/fs/ext2.c`) — `/mnt/ext2` (LBA 4096)
- NTFS (`kernel/fs/ntfs.c`) — `/mnt/ntfs` (LBA 69632)

### Shell Commands Added
`ls`, `cat`, `write`, `rm`, `mkdir`, `mkpdfs`, `setp` (chmod), `seto` (chown), `elev`, `diskinfo`

---

## Phase 9a: PD-Shell Tier 2 ✅ COMPLETE

**Completed**: April 12, 2026

### CWD & Path Resolution
- `g_cwd[128]` — session state, initialized to `/` at login
- `normalize_path(input, out)`:
  - `~` → `/home/<username>`
  - `~/sub` → `/home/<username>/sub`
  - Handles `..`, `.`, `/absolute`, and `relative` (prepends CWD)
- Every file/dir command resolves through `normalize_path` before acting

### Commands Added
| Command | Description |
|---------|-------------|
| `sdir [path]` | Change directory (`~`, `..`, `/abs`, `relative`) |
| `copy <src> <dst>` | Copy a file |
| `move <src> <dst>` | Move / rename a file |

### Prompt
`username@pd-shell:/cwd> ` — CWD shown in cyan

---

## Phase 9b: Shell Quality of Life (Next)

**Goal**: Make the shell feel like a real terminal

- **Command history** — circular buffer (e.g. 32 entries), ↑/↓ arrows to navigate. `readline` already has stub branches for `KEY_UP` / `KEY_DOWN`.
- **Tab completion** — on `TAB` keypress, enumerate `g_cwd` via `pdfs_stat_dir`, find entries matching the current word prefix, complete if unique, list if ambiguous.

**Verification:**
1. ↑ shows previous command; ↓ moves forward through history
2. TAB completes `wri` → `write`, `sdi` → `sdir`
3. TAB with ambiguous prefix lists matches
4. History survives across commands in the same session

---

## Phase 10: Process Management

**Goal**: True multitasking — multiple processes, context switching, ring 3

**Steps:**
1. Process Control Block (PCB) — pid, state, registers, stack, page dir
2. Assembly context switch — save/restore all GPRs + EFLAGS + ESP
3. Round-robin scheduler — timer IRQ (PIT) triggers preemption
4. `fork()` equivalent — clone current process (copy-on-write optional)
5. `exec()` equivalent — load flat binary from PDFS into new process
6. Process termination — reclaim pages, remove from scheduler queue
7. `ps` shell command — list running processes
8. `kill <pid>` shell command — terminate a process
9. User mode (ring 3) separation — TSS, syscall gate or `int 0x80`

**Verification:**
1. Two processes run concurrently (e.g. two spinning counters)
2. PIT preempts at 100 Hz
3. Process creation and death clean up pages
4. Ring 3 process cannot access kernel memory (GPF on attempt)

---

## Phase 11: PD-OS Virtual FS Population

**Goal**: Populate the PDFS directory structure defined above; link shell commands to paths

**Steps:**
1. On boot, if `/home` doesn't exist, auto-create `/home/<username>` for each user
2. Create `/etc/pd-os/` with version file
3. Create `/pdsys/` and `/pdapps/` skeleton directories
4. Shell `sdir` and `ls` already work — `~` resolves to `/home/<username>`
5. Move `g_passwd` user table to `/etc/passwd` (text format, parsed at boot)
6. `useradd` / `userdel` shell commands — modify `/etc/passwd` on disk

**Verification:**
1. Fresh boot has `/home/pd/`, `/home/root/`, `/etc/pd-os/version`, `/pdsys/`, `/pdapps/`
2. `cat /etc/pd-os/version` prints PD-OS version string
3. `/etc/passwd` parsed correctly; new users added with `useradd`

---

## Phase 12: Integration & Polish

**Goal**: Make PD-OS feel solid and well-documented for contributors

**Steps:**
1. Boot time optimization (avoid redundant ATA reads on mount)
2. Consistent error messages across all shell commands
3. `man <command>` or `help <command>` — per-command usage detail
4. Boot splash / version banner improvements
5. Kernel build size optimization
6. Stress testing — create/delete 32 files, fill disk, reboot cycles
7. Full documentation pass: README, PROJECT_STATUS, architecture notes

---

## Future: GUI / VESA (Post-CLI)

**Goal**: Graphical desktop environment

**High-level steps:**
1. VESA/VBE graphics mode via INT 10h in Stage 2 (before protected mode)
2. Linear framebuffer driver — plot pixels, draw rectangles, blit fonts
3. PS/2 mouse driver (IRQ12)
4. Window manager — window structs, z-order, drag/resize
5. Event system — keyboard + mouse events dispatched to focused window
6. GUI toolkit — button, label, text input, scrollview widgets
7. Built-in apps: terminal emulator (run PD-Shell in a window), file manager, text editor
8. Desktop environment shell — taskbar, clock, start menu

**Deferred until Phase 12 is complete.**

---

## Decisions & Scope

**In scope:**
- 32-bit x86 monolithic kernel
- Custom bootloader (no GRUB)
- PDFS v2 native filesystem + FAT32/ext2/NTFS read-only
- Full CLI with 26+ commands
- Unix-style permissions and multi-user
- Future Debian binary compatibility (filesystem layout + `/bin`, `/lib`, `/usr` hierarchy)

**Out of scope for now:**
- 64-bit (planned for future separate version)
- Network stack
- USB support
- SMP / multi-core
- Audio
- Security hardening (SELinux-style)

---

## Build Quick Reference

```bash
# Build everything
bash build.sh

# Run
qemu-system-i386 -drive format=raw,file=build/pd-os.img -m 128M -display sdl

# Commit to both branches
./commit.sh "your message"
```

**Default credentials**: `pd / pd`  |  `root / root`
