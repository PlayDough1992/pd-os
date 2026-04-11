# PD-OS Implementation Plan

## Project Overview
Custom 32-bit operating system with custom bootloader (PD-Bootloader), kernel (PD-Kernel), CLI, and eventual GUI. Beginner-friendly phased approach with focus on CLI first.

**Target**: 32-bit x86 (i686)  (will be building a 64-bit x86 version later)
**Timeline**: 2-3 months for bootloader + kernel + CLI  
**Environment**: Windows with MinGW/Cygwin toolchain  (create a linux environment later)
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
3. ⚠️ **Chocolatey not available** - Had to use alternative installation methods
4. ⚠️ **NASM installer had PATH issues** - Solved by using portable version
5. ✅ Downloaded portable NASM 2.16.03 (no installation required!)
6. ✅ Created PowerShell build scripts as alternative to Make
7. ✅ Created automated setup scripts for future use
8. ⚠️ **i686-elf-gcc download failed** (large file) - Deferred to Phase 4 (not needed yet)

**Key Solutions:**
- Used **portable NASM** instead of installer (`tools-download/nasm/nasm.exe`)
- Created `build.ps1` PowerShell script instead of Makefile
- Created three setup scripts:
  - `tools/autosetup.ps1` - Fully automated setup
  - `tools/simple-setup.ps1` - Interactive step-by-step guide
  - `tools/quick-setup.ps1` - Quick download helper

**Relevant files:**
- `tools/setup.md` - Comprehensive toolchain installation guide
- `tools/MANUAL_INSTALL.md` - Manual installation instructions
- `tools/autosetup.ps1` - Automated setup script
- `build.ps1` - PowerShell build system (uses portable NASM)
- `Makefile` - Traditional Make build configuration (for Git Bash)
- `bootloader/stage1.asm` - Bootloader source

**Verification:**
1. ✅ QEMU: `qemu-system-i386 --version` → version 10.1.0
2. ✅ NASM: Portable version at `tools-download/nasm/nasm.exe`
3. ⏸️ i686-elf-gcc: Deferred to Phase 4 (kernel development)
4. ✅ Git Bash detected (includes Make)

**Lessons Learned:**
- Windows toolchain setup is challenging - portable tools work better
- PowerShell scripts provide good alternative to Make on Windows
- Automated installation requires administrator privileges - manual is sometimes easier
- Can build and test bootloader without cross-compiler (only needed for kernel)

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
```powershell
# Download portable NASM (if not already downloaded)
Invoke-WebRequest -Uri "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip" -OutFile tools-download\nasm-portable.zip
Exp✅ Bootloader binary is exactly 512 bytes
2. ✅ Boot signature (0xAA55) present at bytes 510-511
3. ✅ QEMU boots and displays welcome message
4. ✅ Segment registers initialized correctly (DS, ES, SS = 0)
5. ✅ Stack pointer set at 0x7C00
6. ✅ BIOS INT 10h successfully prints text to screen
7. ✅ System halts cleanly after displaying messages

**Key Achievement:**
🎉 **BOOTLOADER SUCCESSFULLY BOOTS!** This is a real, working bootloader that could run on actual hardware. The system boots from power-on, loads our 512-byte code at 0x7C00, and executes it perfectly!

**Next Steps:**
Phase 3 will implement Stage 2 to load the kernel and switch to protected mode.
.\tools-download\nasm\nasm.exe -f bin bootloader\stage1.asm -o build\bootloader.bin

# Verify size (must be exactly 512 bytes)
(Get-Item build\bootloader.bin).Length  # Output: 512

# Create bootable disk image
$blank = New-Object byte[] (1474560)  # 1.44MB floppy
[IO.File]::WriteAllBytes("build\pd-os.img", $blank)
$boot = [IO.File]::ReadAllBytes("build\bootloader.bin")
$disk = [IO.File]::ReadAllBytes("build\pd-os.img")
for ($i=0; $i -lt 512; $i++) { $disk[$i] = $boot[$i] }
[IO.File]::WriteAllBytes("build\pd-os.img", $disk)

# Run in QEMU
qemu-system-i386 -drive format=raw,file=build\pd-os.img -m 128M
```

**Or simply:**
```powershell
.\build.ps1 run
```

**Relevant files:**
- `bootloader/stage1.asm` - MBR bootloader source (exactly 512 bytes) ✅
- `build/bootloader.bin` - Compiled bootloader binary ✅
- `build/pd-os.img` - Bootable 1.44MB disk image ✅
- `build.ps1` - PowerShell build script ✅
- `tools/create-image.ps1` - Disk image creation script ✅
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