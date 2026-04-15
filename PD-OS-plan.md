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

## Phase 9b: Shell Quality of Life ✅ COMPLETE

**Completed**: April 12, 2026  
**Goal**: Make the shell feel like a real terminal

### Command History
- `g_hist[32][512]` — circular ring buffer; `hist_push()` / `hist_get()`
- ↑/↓ arrows navigate history during readline; `hist_saved` slot preserves the partially-typed line when scrolling up and restores it on ↓ past the end
- Duplicate suppression — identical consecutive entries silently dropped

### Tab Completion
- TAB fires `tab_complete()` on the current token
- First token (no slash): prefix-searches `cmd_name_list[]`
- Argument tokens or tokens containing `/`: resolve directory via `normalize_path`, enumerate entries via `pdfs_stat_dir`
- Unique match → suffix inserted (directories gain a trailing `/`)
- Multiple matches → columnar list printed below the prompt, readline redrawn

### Live Suggestion Menu
Fires on every printable keypress while editing the **first** token (command name):

```
pd@pd-shell:/> c_
┌─────────────────┐
│ clear           │  ← highlighted: black text on cyan background
│ color           │  ← unselected: dark-grey text on black
│ copy            │
│ cat             │
│ cd              │
│ chmod           │
│ chown           │
│ cls             │
└─────────────────┘
```

- `sug_update()` rebuilds the match list (up to 8 entries from `cmd_name_list[]`) on each keypress
- `sug_draw_menu()` renders the menu directly below the input line; flips above the line when near the bottom of the 25-row screen
- ↑/↓ arrows navigate the menu when it is active (bypasses history navigation)
- **Space** confirms the highlighted entry: replaces typed prefix with full command name + a trailing space
- Menu dismisses on: Enter, Escape, Backspace-to-empty, or any key that clears the first token

### Readline Improvements
- **Scroll anchor tracking** — `g_scroll_count` in VGA driver; per-keypress delta adjusts `anchor_row` when forced scrolls occur so the cursor position stays correct after the screen shifts
- **Word-wrap** — `ww_draw()` helper performs soft look-ahead; whole words are moved to the next line instead of splitting mid-character at column 79
- **`vga_clear_chars(col, row, n)`** — erases N characters from the VGA buffer without triggering a scroll; used by history navigation and backspace redraw
- Input buffer `SHELL_BUF_SIZE` bumped 256 → 512 bytes

### Verification
1. ↑ recalls the previous command; ↓ returns to the partially-typed line
2. TAB on `wri` completes to `write`; TAB on `sdi` completes to `sdir`
3. TAB on an ambiguous prefix (e.g. `s`) prints a columnar match list
4. Typing `c` immediately shows the suggestion menu listing `clear`, `color`, `copy`, `cat`, `cd`, `chmod`, `chown`, `cls`
5. ↑/↓ in the menu selects entries; Space inserts the selected command name
6. History persists for the full session; duplicates are suppressed

---

## Phase 10: Process Management ✅ COMPLETE

**Completed**: April 13, 2026  
**Goal**: Preemptive multitasking — PCB table, IRQ0 context switch, `ps`/`kill`

**What shipped:**
1. `g_procs[16]` PCB table, `g_current`, `g_next_pid` — `kernel/core/process.c`
2. `proc_init()` — registers boot thread as pid 0 (`kernel/shell`, PROC_RUNNING)
3. `proc_create(name, entry)` — kmalloc 8 KB stack, builds fake iret frame so the new task starts via `iret`
4. `irq0_preempt` ISR stub (`kernel/arch/x86/sched_entry.asm`) — dedicated IRQ0 handler, not routed through `isr_common`
5. `sched_irq(esp)` — ACKs PIC, calls `pit_handler`, saves ESP, round-robin on quantum expiry (10 ticks), returns new ESP
6. `proc_kill(pid)` — marks DEAD; pid 0 protected
7. Idle task (`static void idle_task`) — `hlt` loop, created as pid 1 before `sti`
8. `ps` shell command — color-coded table (RUNNING=green, DEAD=dark-grey)
9. `kill <pid>` shell command
10. **Bug fix**: `proc_init`/`proc_create` moved before `sti` — fixes cold-boot triple-fault that caused Stage 2 menu to reappear on first Enter

**Deferred to future phase:**
- `fork()` / `exec()` — load flat binary from PDFS
- User mode (ring 3) — TSS, `int 0x80` syscall gate
- Page-level process isolation

**Verification:**
1. `ps` shows pid 0 (shell, RUNNING) and pid 1 (idle, RUNNING)
2. PIT preempts at 100 Hz — idle task switches in when shell is blocked on keyboard
3. `kill 1` marks idle DEAD; `ps` shows it grey
4. Boot goes directly to login on first Enter (triple-fault regression fixed)

---

## Phase 11: PDFS v3 + VFS Population + Desktop Environment Loader

**Goal**: Replace the fixed-layout PDFS v2 with a fully dynamic, journaled filesystem; populate the Debian-compatible + PD-OS-specific directory tree; and implement the `/de` desktop environment launcher.

---

### Part A — PDFS v3 (Dynamic, Journaled, ext4-grade Metadata)

#### Why v2 is insufficient
- Root dir and every subdir are hard-limited to 32 entries (4 sectors, fixed)
- Monotonic `next_free_lba` allocator never reclaims deleted space
- Journal only protects one dir-sector write at a time (no multi-op atomicity)
- Dirent carries no modification/access timestamps beyond `ctime`
- No inode concept — metadata and data location are merged in the dirent

#### PDFS v3 Design

**On-disk layout** (base LBA 200, same anchor):
```
LBA 200          Superblock v3 (1 sector)
LBA 201          Journal — 8 sectors (4 KB ring log)
LBA 209          Inode bitmap — 1 sector   (tracks 4096 inodes)
LBA 210          Block bitmap — 4 sectors  (tracks 32768 × 512-byte blocks)
LBA 214          Inode table  — 256 sectors (4096 inodes × 32 bytes each)
LBA 470+         Data blocks  — rest of volume
```

**Inode (32 bytes)**:
```
flags       uint16  — IN_USE | IS_DIR | IS_SYMLINK
uid/gid     uint8   — owner/group
mode        uint16  — rwxrwxrwx (low 9 bits, Unix layout)
link_count  uint16  — hard link count
size        uint32  — file size in bytes (0 for dirs)
ctime       uint32  — creation time (PIT ticks)
mtime       uint32  — modification time
atime       uint32  — access time
blocks[4]   uint32  — direct block pointers (covers up to 2 KB per file for now)
```
*(Single indirect block pointer added in v3.1 when needed.)*

**Directory block** (one 512-byte sector per dir block):
- Each dir block holds up to **16 variable-length entries** (`name` up to 27 chars + 5-byte fixed header = 32 bytes minimum, padded to alignment)
- Dirs start with 1 block; grow by appending an extra block when full (inode `blocks[]` extended)
- No fixed cap per directory — limited only by available blocks

**Block allocator**:
- Bitmap-based free list (block bitmap at LBA 210)
- `alloc_block()` / `free_block()` — O(n) bitmap scan; blocks are reused after `free_block`
- Replaces the monotonic `next_free_lba` pointer (end of v2 wasted space on delete)

**Journal (8 sectors, ring log)**:
- Write-ahead log for all metadata mutations (inode writes, dir block writes, bitmap updates)
- Log entry header: `{ type, target_lba, len, checksum }`  
- Types: `JE_INODE`, `JE_DIRBLK`, `JE_BITMAP`, `JE_COMMIT`  
- A transaction: write log entries → write `JE_COMMIT` → apply to disk → clear log head  
- On mount: scan log; if uncommitted entries exist, discard them (redo not needed — write-intent log); if `JE_COMMIT` present, replay the transaction to disk

**Permissions** (same as v2, kept identical to ext4 semantics):
- `pdfs_set_context(caller, elevated)` before every mutating op
- Check: `caller.uid == inode.uid` OR `USER_FLAG_ROOT` OR `g_elevated` OR `(mode & WOTH)`
- Sticky bit (mode bit 9) on directories: only owner can unlink entries

**Version migration**:
- On mount: if superblock magic matches but version == 2 → mount read-only, print warning `"PDFS v2 disk — run mkpdfs to upgrade"`
- `mkpdfs` shell command reformats to v3 (destructive, requires `elev`)

#### Steps — Part A
1. Define new `pdfs_superblock_v3_t`, `pdfs_inode_t`, `pdfs_direntry_t` in `kernel/include/pdfs.h` — keep v2 structs for migration detection
2. Implement block bitmap allocator (`alloc_block` / `free_block`) in `kernel/fs/pdfs.c`
3. Implement inode bitmap allocator (`alloc_inode` / `free_inode`)
4. Implement journal: `jrnl_begin()`, `jrnl_log(type, lba, data)`, `jrnl_commit()`, `jrnl_replay()` (called from `pdfs_mount`)
5. Rewrite `pdfs_mkdir`, `pdfs_create`, `pdfs_unlink` to use inode + block allocators + journal
6. Rewrite `pdfs_read`, `pdfs_write` to use inode block pointers instead of `start_lba`
7. Rewrite `pdfs_stat_dir` to enumerate variable-length dir entries via inode → block pointers
8. Update `pdfs_format` to write v3 superblock, clear bitmaps, initialise inode table, create root dir inode (inode 2, matching ext2/ext4 convention)
9. Update `mkpdfs` shell command; update `diskinfo` to show v3 stats (free blocks, inode usage)
10. Update `build.sh` PDFS init Python script to write a v3 superblock + bitmaps

---

### Part B — VFS Population (Debian hierarchy + PD-OS extensions)

Populated automatically on first boot if the directory does not exist:

```
/                   (PDFS root, inode 2)
├── bin/            uid=0 gid=0 mode=755
├── sbin/           uid=0 gid=0 mode=755
├── lib/            uid=0 gid=0 mode=755
├── lib32/          uid=0 gid=0 mode=755
├── etc/            uid=0 gid=0 mode=755
│   └── pd-os/      uid=0 gid=0 mode=755
│       └── version (file: "PD-OS 0.1.0\n")
├── home/           uid=0 gid=0 mode=755
│   ├── root/       uid=0 gid=0 mode=700
│   └── pd/         uid=1 gid=1 mode=755
├── root/           uid=0 gid=0 mode=700
├── dev/            uid=0 gid=0 mode=755
├── proc/           uid=0 gid=0 mode=755
├── sys/            uid=0 gid=0 mode=755
├── tmp/            uid=0 gid=0 mode=1777  (sticky)
├── var/            uid=0 gid=0 mode=755
│   ├── log/        uid=0 gid=0 mode=755
│   └── tmp/        uid=0 gid=0 mode=1777  (sticky)
├── usr/            uid=0 gid=0 mode=755
│   ├── bin/        uid=0 gid=0 mode=755
│   ├── sbin/       uid=0 gid=0 mode=755
│   ├── lib/        uid=0 gid=0 mode=755
│   └── share/      uid=0 gid=0 mode=755
├── mnt/            uid=0 gid=0 mode=755
│   ├── fat/        (VFS FAT32 mount point)
│   ├── ext2/       (VFS ext2 mount point)
│   └── ntfs/       (VFS NTFS mount point)
├── pdsys/          uid=0 gid=0 mode=755
│   ├── kernel/     uid=0 gid=0 mode=755
│   ├── drivers/    uid=0 gid=0 mode=755
│   └── version     (file: "PD-OS 0.1.0\n")
├── pdapps/         uid=0 gid=0 mode=755
│   ├── system/     uid=0 gid=0 mode=755
│   └── user/       uid=0 gid=1 mode=775
└── de/             uid=0 gid=0 mode=755   ← PD-OS specific (see Part C)
```

**`/etc/passwd`** — plain text, one line per user:
```
root:x:0:0:root:/root:/bin/shell
pd:x:1:1:pd:/home/pd:/bin/shell
```
- Parsed at boot into the existing `g_users[]` table; replaces the hard-coded array
- `useradd <name> <password>` — appends entry, creates `/home/<name>/` with correct uid
- `userdel <name>` — removes entry (does not delete home dir — use `rm` separately)
- `whoami` and `id` read from parsed table

#### Steps — Part B
1. `fs_populate()` — called from `kernel_main` after `pdfs_mount`; checks existence via `vfs_open` before creating anything (idempotent — safe on every boot)
2. Create all directories above with correct uid/gid/mode using `pdfs_mkdir`
3. Write `/etc/pd-os/version` and `/pdsys/version` text files
4. Write initial `/etc/passwd` file if not present
5. `passwd_load()` — parse `/etc/passwd` at boot into `g_users[]`
6. `passwd_save()` — write `g_users[]` back to `/etc/passwd`
7. `useradd` / `userdel` shell commands

---

### Part C — `/de` Desktop Environment Loader

#### Directory convention
Each installed DE is a subdirectory of `/de/`. **Desktop environments are entirely optional** — if `/de/` is empty or absent, PD-OS boots straight to PD-Shell with no prompts. DEs are fully open: anyone can create their own by placing a `launch` binary inside a new `/de/<name>/` directory, with no installer, registry, or system modification required. The DE scanner discovers them automatically at login time.

```
/de/
├── default         (text file: name of default DE, e.g. "pdwm\n") — optional
├── pdwm/           first-party PD-OS window manager (future)
│   └── launch      executable binary (flat ELF, loaded by kernel loader — Phase 12+)
└── <user-created>/ ← any user can drop a DE here; auto-discovered on next login
    └── launch
```

#### Boot-time DE selection logic (runs after login, before dropping to shell)
```
1. Scan /de/ for subdirs that contain a "launch" file → build DE list
2. If DE list is empty  → skip, drop straight to PD-Shell
3. If DE list has exactly 1 entry → auto-launch it (no prompt)
4. If /de/default exists and its content matches a valid DE name → auto-launch it
5. Otherwise (multiple DEs, no valid default) → show prompt:

   ┌──────────────────────────────────────────────────────────────────────────┐
   │  Desktop environments available:                                         │
   │    [1] pdwm                                                              │
   │    [2] <other>                                                           │
   │    [S] Skip — continue to PD-Shell                                      │
   │                                                                          │
   │  Select [1/2.../S]:                                                      │
   └──────────────────────────────────────────────────────────────────────────┘

   - Timeout 10 seconds → falls through to PD-Shell
   - User types number → launch that DE
   - User types S / Enter on empty / timeout → drop to PD-Shell
```

#### DE launcher (`de_launch`)
- Reads `/de/<name>/launch` binary into memory
- Hands off to process manager: `proc_create_from_binary(name, ptr, size)` (Phase 12 kernel loader)
- Shell process blocks (waits for DE proc to exit) then returns to login screen on DE exit
- For Phase 11: `de_launch` is stubbed — prints `"Launching <name>... (DE runtime not yet implemented)"` and returns to shell. The selection logic and `/de/` directory structure are fully implemented and tested.

#### Steps — Part C
1. `de_scan(names[], max)` — enumerates `/de/` subdirs that have a `launch` child; returns count
2. `de_read_default(out)` — reads `/de/default` into `out`; returns 0 if file exists and is valid
3. `de_select_and_launch()` — implements the selection logic above; called from login flow in `kernel.c` after successful authentication
4. Stub `de_launch(name)` — prints banner, returns 0
5. Update `kernel_main` login flow: after `login_run()` succeeds, call `de_select_and_launch()` before entering shell loop

---

### Verification
1. Fresh `mkpdfs` formats v3; `diskinfo` shows inode table, free block count
2. Create 100 files in one directory — succeeds (no 32-entry cap)
3. Delete 50 files — `diskinfo` shows blocks freed (allocator reclaimed them)
4. Simulate power-off mid-write by aborting QEMU mid-journal-transaction; remount → journal replay restores consistency
5. All Debian-hierarchy dirs present after first boot: `ls /home/pd`, `cat /etc/pd-os/version`, `cat /etc/passwd`
6. `useradd testuser password` → entry in `/etc/passwd`, `/home/testuser/` created
7. `/de/` empty → boot drops straight to PD-Shell
8. Create `/de/pdwm/launch` → next boot auto-launches (stub prints banner, returns to shell)
9. Create two DE dirs with no `/de/default` → selection prompt appears with 10-second timeout

---

## Phase 12: Physical Hardware Readiness + pdwm Desktop Environment

**Goal**: Produce a production-quality 32-bit OS capable of booting on real legacy hardware.  Deliver **pdwm**, the default PD-OS window manager — a Windows ME-inspired GUI with modern rounded-corner windows, a green persistent taskbar, and an integrated terminal emulator.

---

### Physical Hardware Readiness Checklist

| Area | Status | Notes |
|---|---|---|
| A20 gate (3 methods) | ✅ done | BIOS 2401h / KBC / Fast A20 |
| INT 13h LBA disk read | ✅ done | 160-sector split read |
| E820 memory map | ✅ done | ACPI 3.0, 32-entry cap |
| Real-mode VBE probe + set | 🔄 Phase 12a | Before protected mode switch |
| PS/2 keyboard (IRQ1) | ✅ done | scancode set 1, circular buffer |
| PS/2 mouse (IRQ12) | 🔄 Phase 12a | 3-byte packet stream |
| RTC real-time clock | 🔄 Phase 12a | CMOS ports 0x70/0x71 |
| ACPI shutdown / reboot | Deferred | Add `shutdown` shell command post-12 |
| Serial debug port (COM1) | Deferred | Optional logging on real HW |
| Physical USB boot image | 🔄 Phase 12a | Raw disk image → Rufus DD mode |

---

### Part A — VESA VBE Graphics Foundation

#### Stage 2 additions (real-mode, before protected mode)
1. Probe VBE controller info (`INT 10h AX=4F00h`) — verify "VESA" signature
2. Scan mode list: prefer **800×600×32bpp**, then ×24, ×16; 1024×768 fallback; 640×480 last resort
3. For each candidate: get mode info (`INT 10h AX=4F01h`), require LFB attribute (bit 7) + packed/direct color memory model
4. Set best mode (`INT 10h AX=4F02h`, bit 14 = linear framebuffer)
5. Write `boot_params_t` struct to physical address **0x5300** (safe above E820 block):
   - `fb_addr` (uint32) — physical linear framebuffer address (from VBE `PhysBasePtr`)
   - `fb_width`, `fb_height`, `fb_pitch` (uint16) — resolution and bytes/line
   - `fb_bpp` (uint8) — bits per pixel
   - `fb_ok` (uint8) — 1 = VBE active, 0 = fallback text mode
6. If VBE probe fails: `fb_ok=0`, kernel falls back to VGA text mode gracefully

#### Kernel page-table extension (PSE)
- Enable CR4.PSE (4 MB pages) during `paging_init()`
- Add `paging_map_frame(uint32_t phys)` — inserts a PSE 4 MB PDE to identity-map the chunk containing `phys`
- Called immediately after `paging_init()`, if `boot_params->fb_ok`, to map the framebuffer before any pixel write

#### GFX driver (`kernel/drivers/gfx.c`)
- `gfx_init(boot_params_t *)` — stores globals, calls `paging_map_frame`, sets output routing
- Primitives: `gfx_pixel`, `gfx_fill_rect`, `gfx_draw_rect`, `gfx_hline`, `gfx_vline`
- Font: embedded 8×8 VGA ROM bitmap font (ASCII 32–127, 768 bytes)
- `gfx_char(x, y, c, fg, bg)`, `gfx_text(x, y, str, fg, bg)`, `gfx_putchar(c)` (CRT-style cursor)
- `gfx_rounded_rect(x,y,w,h,r,color)` — filled with pixel-masked corners (4px radius)
- `kprint_redirect(gfx_putchar)` — all `kprintf` output routes to the framebuffer after `gfx_init()`

#### PS/2 Mouse driver (`kernel/drivers/mouse.c`)
- Enable aux port (KBC command 0xA8), set remote/stream mode, request data reports
- IRQ12: read 3-byte packet (buttons, dx, dy sign-extended), accumulate absolute position
- `mouse_get_event(mouse_event_t *)` — non-blocking dequeue

#### RTC driver (`kernel/drivers/rtc.c`)
- Read BCD hours/minutes/seconds from CMOS (port 0x70/0x71 registers 0x00, 0x02, 0x04)
- `rtc_read(uint8_t *h, uint8_t *m, uint8_t *s)` — converts BCD → binary

---

### Part B — pdwm Window Manager

#### Visual design — Windows ME heritage, modern edge
```
┌──────────────────────────────────────────────────────────┐
│  Desktop: teal  #008080                                  │
│                                                          │
│  ╭─────────────────────────────────── _ □ ╳  ╮           │
│  │ Window title bar (blue gradient)          │           │
│  │                  white client area        │           │
│  │   rounded 4px corners                     │           │
│  ╰───────────────────────────────────────────╯           │
│                                                          │
╰──────────────────────────────────────────────────────────╯
│ ⊞ Start  │  [Terminal]  │             │  12:34  │
└──────────────────────────────────────────────────────────┘
             taskbar: silver  #D4D0C8   H=28px
```

**Color palette:**
| Element | RGB | Notes |
|---|---|---|
| Desktop | 0, 128, 128 | Classic teal |
| Taskbar | 212, 208, 200 | Win ME silver |
| Title bar (active) | 0..16, 0..80, 168..208 | Blue gradient (3 bands) |
| Title bar (inactive) | 128, 128, 128 | Gray |
| Window border | 212, 208, 200 | Same as taskbar |
| Client area | 255, 255, 255 | White |
| Start button | 92, 175, 57 | Modern green |
| Close button | 200, 48, 48 | Red on hover |

#### Window manager core
- `wm_window_t`: x, y, w, h, title, focused, minimized, z_order, content-draw callback
- Max 8 simultaneous windows
- `wm_create(x,y,w,h,title,cb)` / `wm_close(id)` / `wm_focus(id)`
- `wm_hit_test(mx, my)` — find topmost window at mouse coords
- `wm_drag_begin(id, mx, my)` / `wm_drag_update(mx, my)` / `wm_drag_end()` — window drag
- `wm_redraw_all()` — redraw desktop → windows bottom-to-top → taskbar
- `wm_draw_window(id)` — title bar (gradient + rounded corners), border, client area call
- Caption buttons drawn as 14×12 rounded rectangles: `_` (minimize), `□` (maximize, disabled initially), `×` (close, red bg)

#### pdwm Event loop (`kernel/de/pdwm/pdwm.c`)
```
pdwm_main()
├── gfx_init(boot_params)
├── mouse_enable()
├── rtc_init()
├── wm_create terminal window (80×25 chars, titled "Terminal")
├── Start event loop:
│     poll keyboard_getchar() → route to focused window on_key()
│     poll mouse_get_event() → hit-test → drag or button handling
│     every ~18 ticks → redraw clock in taskbar
│     every frame → wm_redraw_all() if dirty flag set
└── Never returns (DE is the process for this session)
```

#### Terminal window
- 80-column × 24-row visible area (8×8 font → 640×192 pixels of content)
- Separate scroll-back buffer (200 lines)
- On each `kprintf` character: appended to terminal line buffer, dirty flag set
- On Enter: parse & run shell command (calls existing `shell_dispatch(cmd)`)
- Scrollbar on right side; PgUp/PgDn scroll the view
- Tab-completion and history work identically to CLI shell

#### Start menu (Phase 12b, stub in Phase 12a)
- Start button opens a 160×120 popover above taskbar
- Entries: Applications, Settings (stub), Shutdown, Restart
- Shutdown: halts CPU with CLI/HLT; Restart: pulses KBC 0xFE (same as stage2 reset)

---

### Part C — Physical Hardware Testing Matrix

| Hardware type | Boot method | Status |
|---|---|---|
| Any BIOS PC (2000–2010) | Rufus DD-mode USB | Target |
| QEMU `-accel whpx` | Existing build.ps1 | Verified |
| QEMU `-accel tcg` | WSL build.sh run | CI |
| CD-ROM boot | El Torito ISO (Phase 12b) | Deferred |

**Creating a bootable USB (Windows):**
```
# Write image to USB in Rufus: DD mode (not ISO mode)
# Or from Linux/WSL:
dd if=build/pd-os.img of=/dev/sdX bs=512 status=progress
```

**Kernel size budget**: current 74 KB → Phase 12 target ≤ 160 KB (fits in 160-sector read)

---

### Steps — Phase 12
1. Stage 2: VBE probe + mode set + write to 0x5300
2. `kernel/include/boot_params.h` — struct definition
3. `kernel/mm/paging.c/h` — PSE enable + `paging_map_frame()`
4. `kernel/core/io.c/h` — `kprint_redirect()` function pointer
5. `kernel/drivers/gfx.c/h` — framebuffer driver + 8×8 font + `gfx_putchar`
6. `kernel/drivers/mouse.c/h` — PS/2 mouse IRQ12
7. `kernel/drivers/rtc.c/h` — CMOS RTC
8. `kernel/de/pdwm/pdwm.c/h` — window manager + pdwm DE
9. Wire into `kernel.c` (paging_map_frame → gfx_init → kprint_redirect → mouse_init → rtc_init)
10. Wire IRQ12 in `exceptions.c` → `mouse_handler()`
11. `de_launch()` in `fs_init.c` calls `pdwm_main(user)` instead of printing stub
12. `build.sh` — compile + link all new files
13. Test on QEMU; test on physical hardware (USB boot)

---

### Verification
1. `build.sh all` succeeds, kernel ≤ 160 KB
2. QEMU: boot → VBE mode set → teal desktop + taskbar visible within 3 seconds of kernel entry
3. Mouse cursor moves smoothly around screen
4. Clock in taskbar updates every second (RTC)
5. Terminal window accepts input, runs shell commands, displays output in window
6. Multiple windows can be created (e.g., `open terminal` command opens second terminal)
7. Window drag works with mouse
8. Close button × removes window
9. Physical USB boot: reaches desktop on at least one tested legacy PC
10. `/de/pdwm/launch` file present → `de_select_and_launch` auto-launches pdwm normally

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
