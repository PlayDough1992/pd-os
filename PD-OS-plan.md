# PD-OS Implementation Plan

## Project Overview
Custom 32-bit operating system with custom bootloader (PD-Bootloader), kernel (PD-Kernel), CLI, and eventual GUI. Beginner-friendly phased approach with focus on CLI first.

**Target**: 32-bit x86 (i686)  (will be building a 64-bit x86 version later)
**Timeline**: 2-3 months for bootloader + kernel + CLI  
**Environment**: Linux (Ubuntu/Debian/Arch/Fedora) with native GCC cross-compiler toolchain
**Testing**: QEMU emulator

---

## Phase 1: Environment Setup & Toolchain (Week 1) ✅ COMPLETE

**Status**: Completed April 11, 2026

**Original Plan:**
1. Install NASM (assembler) for Windows
2. Install MinGW-w64 with i686-elf cross-compiler
3. Install QEMU for Windows (x86 system emulator)
4. Install Make (via MinGW or standalone)
5. Create initial project directory structure
6. Create basic Makefile for build automation
7. Test toolchain with "Hello World" bootloader

**What Actually Happened:**
1. ✅ Created complete project directory structure
2. ✅ QEMU was already installed (version 10.1.0)
3. ✅ NASM installed via package manager (`nasm` in apt/dnf/pacman)
4. ✅ Created bash build scripts as the primary build system
5. ✅ Created automated setup scripts for future use
6. ⚠️ **i686-elf-gcc not yet needed** - Deferred to Phase 4 (kernel development)

**Key Solutions:**
- Used **system NASM** installed via package manager
- Created `build.sh` bash script and `Makefile` for builds
- Created three Linux setup scripts:
  - `tools/autosetup.sh` - Fully automated setup (detects distro)
  - `tools/simple-setup.sh` - Interactive step-by-step guide
  - `tools/quick-setup.sh` - Quick toolchain status checker

**Relevant files:**
- `tools/setup.md` - Comprehensive toolchain installation guide (Linux)
- `tools/MANUAL_INSTALL.md` - Manual installation instructions (Linux)
- `tools/autosetup.sh` - Automated setup script
- `build.sh` - Bash build system
- `Makefile` - Traditional Make build configuration
- `bootloader/stage1.asm` - Bootloader source

**Verification:**
1. ✅ QEMU: `qemu-system-i386 --version` → version 10.1.0
2. ✅ NASM: installed via package manager
3. ⏸️ i686-elf-gcc: Deferred to Phase 4 (kernel development)
4. ✅ Make: available on Linux by default

**Lessons Learned:**
- Linux toolchain setup is straightforward via package managers (apt/dnf/pacman)
- `Makefile` + `build.sh` provide a clean dual build system
- Cross-compiler is only needed starting at Phase 4 — no need to install it early
- `dd` and `nasm` are the only tools needed for Phase 1–3

---

## Phase 2: PD-Bootloader - Stage 1 (Week 2) ✅ COMPLETE

**Status**: Completed April 11, 2026 - **BOOTLOADER SUCCESSFULLY BOOTED!**

**Implementation Steps:**
1. ✅ Created 512-byte MBR bootloader in assembly
2. ✅ Set up basic 16-bit real mode code
3. ✅ Initialized segment registers (DS, ES, SS) and stack pointer
4. ✅ Implemented BIOS INT 10h teletype output for boot messages
5. ✅ Added boot signature (0xAA55) at bytes 510-511
6. ✅ Created PowerShell disk image builder
7. ✅ Tested successfully in QEMU - **IT WORKS!**

**Build Process:**
```bash
# Install NASM (if not yet installed)
sudo apt-get install nasm   # Ubuntu/Debian
# sudo dnf install nasm     # Fedora
# sudo pacman -S nasm       # Arch

# Build bootloader
nasm -f bin bootloader/stage1.asm -o build/bootloader.bin

# Verify size (must be exactly 512 bytes)
wc -c < build/bootloader.bin   # Output: 512

# Create 1.44MB blank floppy image
dd if=/dev/zero of=build/pd-os.img bs=512 count=2880

# Write bootloader to first sector
dd if=build/bootloader.bin of=build/pd-os.img conv=notrunc bs=512 count=1

# Run in QEMU
qemu-system-i386 -drive format=raw,file=build/pd-os.img -m 128M
```

**Or simply:**
```bash
./build.sh run
# or
make run
```

**Relevant files:**
- `bootloader/stage1.asm` - MBR bootloader source (exactly 512 bytes) ✅
- `build/bootloader.bin` - Compiled bootloader binary ✅
- `build/pd-os.img` - Bootable 1.44MB disk image ✅
- `build.sh` - Bash build script ✅
- `tools/create-image.sh` - Disk image creation script ✅
- `bootloader/README.md` - Comprehensive documentation ✅

**Boot Output:**
```
PD-Bootloader v0.1 - Stage 1
Booting PD-OS...
Loading Stage 2...
[Phase 2] Halting (Stage 2 not implemented yet)
```

**Verification:**
1. Bootloader binary is exactly 512 bytes
2. QEMU boots and shows Stage 1 message
3. Successfully reads Stage 2 from disk

---

## Phase 3: PD-Bootloader - Stage 2 (Week 2-3)

**Steps:**
1. Create extended bootloader (Stage 2) (*depends on Phase 2*)
2. Enable A20 line (access memory >1MB)
3. Set up Global Descriptor Table (GDT) for protected mode
4. Switch CPU from 16-bit real mode to 32-bit protected mode
5. Detect available memory using BIOS INT 15h
6. Load kernel binary from disk into memory (0x100000)
7. Jump to kernel entry point
8. Create linker script for bootloader

**Relevant files:**
- `bootloader/stage2.asm` - protected mode transition
- `bootloader/gdt.asm` - GDT setup
- `bootloader/a20.asm` - A20 line enabling
- `bootloader/disk.asm` - disk read routines
- `bootloader/linker.ld` - memory layout

**Verification:**
1. CPU successfully enters 32-bit protected mode
2. Memory detection works (printed values)
3. Kernel loaded at correct address (0x100000)
4. Control transfers to kernel entry point

---

## Phase 4: PD-Kernel Foundation (Week 4)

**Steps:**
1. Create kernel entry point in assembly (*parallel with finalizing Stage 2*)
2. Set up kernel stack
3. Call kernel_main() from assembly
4. Implement VGA text mode driver (80x25 console)
5. Create basic print functions (putchar, puts, printf)
6. Initialize GDT for kernel context
7. Create basic panic/halt handler
8. Create kernel linker script
9. Build system to combine bootloader + kernel into disk image

**Relevant files:**
- `kernel/arch/x86/entry.asm` - kernel entry (_start)
- `kernel/core/kernel.c` - kernel_main() function
- `kernel/drivers/vga.c` - VGA text mode driver
- `kernel/core/io.c` - basic I/O functions
- `kernel/core/panic.c` - error handling
- `kernel/arch/x86/gdt.c` - kernel GDT
- `kernel/include/kernel.h` - main kernel header
- `kernel/linker.ld` - kernel memory layout

**Verification:**
1. Kernel boots and prints "PD-OS Kernel Initialized"
2. VGA driver: text appears on screen in color
3. printf() works correctly
4. Panic handler triggers on test error

---

## Phase 5: Interrupt & Exception Handling (Week 5)

**Steps:**
1. Create Interrupt Descriptor Table (IDT) (*depends on Phase 4*)
2. Write assembly interrupt stubs (256 entries)
3. Implement exception handlers (divide by zero, page fault, etc.)
4. Set up PIC (Programmable Interrupt Controller)
5. Enable hardware interrupts
6. Implement timer interrupt (PIT - Programmable Interval Timer)
7. Implement keyboard interrupt handler
8. Create interrupt-safe console output

**Relevant files:**
- `kernel/arch/x86/idt.c` - IDT setup
- `kernel/arch/x86/idt.asm` - interrupt stubs
- `kernel/arch/x86/exceptions.c` - exception handlers
- `kernel/arch/x86/pic.c` - PIC driver
- `kernel/drivers/pit.c` - timer driver
- `kernel/drivers/keyboard.c` - keyboard IRQ handler

**Verification:**
1. IDT loaded correctly (LIDT instruction)
2. Exception handler catches divide-by-zero test
3. Timer interrupt fires (counter increments)
4. Keyboard interrupt receives keypress events
5. No interrupt storm or triple fault

---

## Phase 6: Memory Management (Week 6)

**Steps:**
1. Physical memory allocator (bitmap or stack-based) (*depends on Phase 5*)
2. Parse memory map from bootloader
3. Implement page frame allocator
4. Set up paging structures (page directory + page tables)
5. Enable paging (CR3 register)
6. Virtual memory manager
7. Kernel heap allocator (kmalloc/kfree)
8. Memory debugging functions

**Relevant files:**
- `kernel/mm/pmm.c` - physical memory manager
- `kernel/mm/paging.c` - page table management
- `kernel/mm/vmm.c` - virtual memory manager
- `kernel/mm/heap.c` - kernel heap (kmalloc)
- `kernel/include/mm.h` - memory management headers

**Verification:**
1. Memory map correctly parsed
2. Page frames allocated/freed correctly
3. Paging enabled without crash
4. kmalloc()/kfree() work correctly
5. No memory leaks in test loops

---

## Phase 7: Storage & Filesystem (Week 7-8)

**Steps:**
1. ATA PIO disk driver (*depends on Phase 5 for interrupts*)
2. Read/write disk sectors
3. Partition table parsing (MBR)
4. Implement simple filesystem (FAT12 or custom simple FS)
5. VFS (Virtual File System) layer
6. File operations: open, read, write, close
7. Directory operations: list, create, delete
8. Path resolution

**Relevant files:**
- `kernel/drivers/ata.c` - ATA disk driver
- `kernel/fs/vfs.c` - virtual filesystem layer
- `kernel/fs/fat12.c` - FAT12 filesystem (or simple custom FS)
- `kernel/fs/file.c` - file operations
- `kernel/include/fs.h` - filesystem headers

**Verification:**
1. Disk sectors read/written correctly
2. Filesystem mounted
3. Files can be created, read, written
4. Directory listing works
5. Path resolution handles / correctly

---

## Phase 8: Process Management (Week 9)

**Steps:**
1. Process Control Block (PCB) structure (*depends on Phase 6 for memory*)
2. Task state management
3. Context switching (save/restore registers)
4. Simple round-robin scheduler
5. Process creation (fork/exec equivalent)
6. Process termination
7. Basic IPC (inter-process communication)
8. User mode vs kernel mode separation

**Relevant files:**
- `kernel/process/process.c` - process management
- `kernel/process/scheduler.c` - task scheduler
- `kernel/process/context.asm` - context switch
- `kernel/include/process.h` - process structures

**Verification:**
1. Multiple processes can be created
2. Scheduler switches between processes
3. Processes don't corrupt each other's memory
4. Process termination cleans up resources
5. Timer-based preemption works

---

## Phase 9: CLI Shell (Week 10-11)

**Steps:**
1. Shell main loop (*depends on Phase 7 for FS, Phase 8 for processes*)
2. Command input handling (keyboard buffer)
3. Command parsing (tokenization, arguments)
4. Built-in commands:
   - `help` - show available commands
   - `clear` - clear screen
   - `echo` - print text
   - `ls` - list directory
   - `cat` - display file contents
   - `touch` - create file
   - `mkdir` - create directory
   - `rm` - delete file
   - `cp` - copy file
   - `mv` - move/rename file
   - `ps` - list processes
   - `kill` - terminate process
   - `shutdown` - halt system
5. Simple scripting (read commands from file)
6. Tab completion (basic)
7. Command history (up/down arrows)

**Relevant files:**
- `kernel/shell/shell.c` - main shell loop
- `kernel/shell/parser.c` - command parsing
- `kernel/shell/commands/` - individual command implementations
- `kernel/shell/builtins.c` - built-in command registry

**Verification:**
1. Shell prompt appears after boot
2. All built-in commands work correctly
3. File operations modify filesystem
4. Process commands manage tasks
5. Scripts execute line by line
6. Tab completion suggests files
7. Command history works

---

## Phase 10: Integration & Polish (Week 12)

**Steps:**
1. Boot sequence optimization (*depends on all phases*)
2. Error handling improvements
3. Help documentation for commands
4. Boot configuration file
5. System call interface cleanup
6. Performance testing
7. Bug fixes
8. User documentation (README, usage guide)

**Verification:**
1. System boots reliably in <5 seconds
2. All features work together
3. Error messages are helpful
4. Documentation is complete
5. No known critical bugs

---

## Future: GUI Foundation (Post-CLI, 3+ months)

**Steps (high-level overview):**
1. VESA/VBE graphics mode (*depends on bootloader enhancements*)
2. Framebuffer driver
3. Basic graphics primitives (pixels, lines, rectangles)
4. Mouse driver (PS/2)
5. Window manager foundation
6. Event system
7. Simple GUI toolkit (buttons, text boxes)
8. Desktop environment shell
9. Basic applications (calculator, text editor, file manager)

**Deferred to later:** This phase is intentionally scoped out for after CLI is complete and stable.

---

## Decisions & Scope

**Included:**
- Custom bootloader (no GRUB dependency)
- 32-bit x86 architecture
- Monolithic kernel design (simpler for learning)
- FAT12 or simple custom filesystem
- Basic multitasking (round-robin)
- Full CLI with scripting support
- Windows-native development environment

**Excluded (for now):**
- 64-bit support (future)
- Network stack
- USB support
- SMP (multicore)
- Advanced filesystems (ext2/3/4)
- Security features (users/permissions) - minimal
- GUI (deferred to post-CLI)

**Assumptions:**
- Using QEMU for testing (no real hardware initially)
- Monolithic kernel (easier for beginners)
- Single-core CPU support
- No audio support initially

---

## Critical Resources & Learning Path

**For beginners - recommended reading order:**
1. OSDev Wiki (osdev.org) - bootloader basics
2. Intel x86 manuals (protected mode, paging)
3. "Writing a Simple Operating System from Scratch" by Nick Blundell
4. NASM documentation
5. GCC cross-compiler guide

**Testing strategy:**
- QEMU for all development
- Bochs for debugging (has better debug interface)
- Real hardware testing (USB boot) once stable

**Build system:**
- Makefile orchestrates: bootloader → kernel → disk image
- Disk image creation: `dd` via MinGW/Cygwin
- Automated QEMU launch for quick test cycles