# PD-OS - A Completely Custom Operating System

## 🌟 About

**A completely open source brand new operating system, custom bootloader, kernel, CLI and GUI.**

PD-OS is **NOT** Linux-based, **NOT** Windows-based, **NOT** Mac-based. This is a completely custom operating system built from the ground up.

- ⚙️ **Custom Bootloader** - No GRUB, we built our own (PD-Bootloader)
- 🔧 **Custom Kernel** - Not the Linux kernel, not any existing kernel (PD-Kernel)
- 💻 **Custom CLI & GUI** - No Linux desktop environments, everything is original
- 🤝 **Community Driven** - Like Linux in spirit, but completely independent in code

**We are hoping the dev communities will reach out to help with this project!** This is an ambitious undertaking to create a truly new operating system from scratch. All contributions, ideas, and collaboration are welcome.

---

## 🎯 Project Goals

- ✅ Custom bootloader (no GRUB dependency)
- ✅ 32-bit x86 kernel
- ⬜ Command-line interface (CLI) with scripting
- ⬜ Process management and multitasking
- ⬜ Filesystem support
- ⬜ Graphical desktop environment (future)

---

## 📋 Current Status

**Phase 2 - PD-Bootloader Stage 1: COMPLETE ✓**

### Completed
- ✅ Project directory structure created
- ✅ PD-Bootloader Stage 1 (512-byte MBR)
- ✅ Build system (Makefile)
- ✅ Toolchain setup documentation
- ✅ Bootloader displays welcome message
- ✅ Boots successfully in QEMU

### In Progress
- 🔄 Toolchain installation (NASM, i686-elf-gcc)

### Next Steps (Phase 3)
- ⬜ PD-Bootloader Stage 2 (A20, GDT, protected mode)
- ⬜ Load kernel from disk
- ⬜ Switch to 32-bit protected mode

---

## 🚀 Quick Start

### 1. Install Required Tools

**Required toolchain:**
- NASM (assembler)
- i686-elf-gcc (cross-compiler)
- QEMU (emulator) ✓ Already installed
- Make (build system)

**📖 See detailed installation instructions:** [tools/setup.md](tools/setup.md)

**Quick setup verification:**
```bash
# Check what's installed
make setup-check
# or
./build.sh setup-check
```

### 2. Build the Bootloader

Once toolchain is installed:

```bash
# Build everything
make all

# Or build just the bootloader
make bootloader
```

### 3. Run in QEMU

```bash
# Boot PD-OS in emulator
make run

# Run with debugging output
make run-debug
```

**Expected output:**
```
PD-Bootloader v0.1 - Stage 1
Booting PD-OS...
Loading Stage 2...
[Phase 2] Halting (Stage 2 not implemented yet)
```

---

## 📁 Project Structure

```
Custom-Bootloader-Kernel-CLI-GUI/
│
├── bootloader/              # PD-Bootloader source code
│   ├── stage1.asm          # ✅ MBR bootloader (512 bytes)
│   ├── stage2.asm          # ⬜ Extended bootloader (Phase 3)
│   ├── gdt.asm             # ⬜ Global Descriptor Table setup
│   ├── a20.asm             # ⬜ A20 line enabling
│   └── README.md           # Bootloader documentation
│
├── kernel/                  # PD-Kernel source code
│   ├── arch/x86/           # x86-specific code
│   ├── core/               # Kernel core (Phase 4+)
│   ├── drivers/            # Device drivers (Phase 5+)
│   ├── mm/                 # Memory management (Phase 6)
│   ├── fs/                 # Filesystem (Phase 7)
│   ├── process/            # Process management (Phase 8)
│   ├── shell/              # CLI shell (Phase 9)
│   ├── include/            # Header files
│   └── libc/               # Minimal C library
│
├── tools/                   # Build tools and scripts
│   ├── setup.md            # ✅ Toolchain installation guide (Linux)
│   ├── autosetup.sh        # ✅ Automated setup script
│   ├── create-image.sh     # ✅ Disk image builder
│   ├── simple-setup.sh     # ✅ Interactive step-by-step setup
│   └── quick-setup.sh      # ✅ Quick toolchain status checker
│
├── build/                   # Build output (generated)
│   ├── bootloader.bin      # Compiled bootloader
│   ├── kernel.bin          # Compiled kernel (Phase 4+)
│   └── pd-os.img           # Bootable disk image
│
├── docs/                    # Documentation
│
├── Makefile                 # ✅ Root build configuration
├── PD-OS-plan.md           # ✅ Complete implementation plan
└── README.md               # This file
```

---

## 🛠️ Build System

### Make Targets

```bash
make all          # Build complete OS (bootloader + kernel + disk image)
make bootloader   # Build only bootloader
make kernel       # Build only kernel (Phase 4+)
make run          # Build and run in QEMU
make run-debug    # Run in QEMU with debugging
make clean        # Remove all build artifacts
make setup-check  # Verify toolchain installation
make help         # Show all available targets
```

### Manual Build Process

If Make is not available:

```bash
# 1. Assemble bootloader
nasm -f bin bootloader/stage1.asm -o build/bootloader.bin

# 2. Verify size (must be exactly 512 bytes)
wc -c < build/bootloader.bin

# 3. Create disk image
./tools/create-image.sh

# 4. Run in QEMU
qemu-system-i386 -drive format=raw,file=build/pd-os.img -m 128M
```

Or simply:
```bash
./build.sh run
```

---

## 📚 Documentation

- **[PD-OS-plan.md](PD-OS-plan.md)** - Complete 12-phase implementation plan
- **[tools/setup.md](tools/setup.md)** - Toolchain setup guide
- **[bootloader/README.md](bootloader/README.md)** - Bootloader technical docs

---

## 🗺️ Development Roadmap

### Phase 1: Environment Setup ✅ COMPLETE
- ✅ Directory structure
- ✅ Build system
- ✅ Toolchain guide

### Phase 2: Bootloader Stage 1 ✅ COMPLETE
- ✅ 512-byte MBR
- ✅ BIOS interrupts for display
- ✅ Boot signature

### Phase 3: Bootloader Stage 2 (Week 2-3)
- A20 line enabling
- GDT setup
- Protected mode transition
- Kernel loading

### Phase 4: Kernel Foundation (Week 4)
- Kernel entry point
- VGA text mode driver
- printf() implementation
- Basic kernel infrastructure

### Phase 5: Interrupts & Exceptions (Week 5)
- IDT setup
- Exception handlers
- Timer and keyboard interrupts

### Phase 6: Memory Management (Week 6)
- Physical memory allocator
- Paging
- Virtual memory
- Kernel heap (kmalloc/kfree)

### Phase 7: Filesystem (Week 7-8)
- ATA disk driver
- FAT12 or simple custom FS
- VFS layer
- File and directory operations

### Phase 8: Process Management (Week 9)
- Process Control Blocks
- Context switching
- Round-robin scheduler
- User mode separation

### Phase 9: CLI Shell (Week 10-11)
- Command parser
- Built-in commands (ls, cat, echo, etc.)
- Simple scripting
- Tab completion and history

### Phase 10: Integration & Polish (Week 12)
- Boot optimization
- Error handling
- Documentation
- Testing and bug fixes

### Future: GUI (Post 3-months)
- VESA graphics mode
- Window manager
- GUI toolkit
- Desktop environment

---

## 🧪 Testing

### QEMU (Recommended)
Fast iteration for development:
```bash
make run              # Normal boot
make run-debug        # With debug output
```

### Real Hardware (Advanced)
After thorough emulator testing, boot from USB:
```bash
# WARNING: Double-check device name!
# Linux:
sudo dd if=build/pd-os.img of=/dev/sdX bs=4M
sync

# Windows: Use Rufus utility in DD mode
```

---

## 🐛 Troubleshooting

### "Command not found" errors
- Tools not in PATH
- See [tools/setup.md](tools/setup.md) for installation
- Restart terminal after modifying PATH

### Build fails with "nasm: command not found"
```powershell
# Check installation
make setup-check

# Install NASM (see tools/setup.md)
```

### Bootloader wrong size
```bash
# Must be exactly 512 bytes
# Check current size:
wc -c build/bootloader.bin   # Git Bash
(Get-Item build/bootloader.bin).Length  # PowerShell
```

### QEMU won't start
- Check QEMU is installed: `qemu-system-i386 --version`
- Verify disk image exists: `ls build/pd-os.img`
- Try manual command: `qemu-system-i386 -drive format=raw,file=build/pd-os.img`

### Nothing appears on screen in QEMU
- Bootloader might not be running
- Check boot signature (bytes 510-511 should be 0xAA55)
- Verify disk image creation succeeded

---

## 📖 Learning Resources

### Essential Reading
1. [OSDev Wiki](https://wiki.osdev.org/) - Comprehensive OS development guide
2. [BIOS Interrupt List](http://www.ctyme.com/intr/int.htm) - Reference for BIOS functions
3. Intel x86 Manuals - Architecture reference
4. NASM Documentation - Assembly syntax

### Recommended Books
- "Operating Systems: Three Easy Pieces" by Remzi H. Arpaci-Dusseau
- "Modern Operating Systems" by Andrew S. Tanenbaum
- "Writing a Simple Operating System from Scratch" by Nick Blundell

### Tutorials
- [OSDev Bare Bones](https://wiki.osdev.org/Bare_Bones)
- [Writing a Bootloader](https://wiki.osdev.org/Rolling_Your_Own_Bootloader)
- [Protected Mode](https://wiki.osdev.org/Protected_Mode)

---

## 🤝 Contributing

**We Need Your Help!** This is an ambitious project to build a completely new operating system from scratch, and we're actively seeking contributors from the developer community.

### How You Can Help

**🔧 Development Areas:**
- Bootloader enhancements (Stage 2, disk loading, memory detection)
- Kernel development (memory management, process scheduling, drivers)
- Filesystem implementation
- CLI shell and commands
- GUI framework (future)
- Device drivers (keyboard, mouse, storage, graphics)

**📚 Documentation:**
- Improve setup guides for different platforms
- Write tutorials and how-to guides
- Create architecture documentation
- Comment and explain complex code sections

**🧪 Testing:**
- Test on different hardware configurations
- Report bugs and issues
- Validate build process on various systems
- Performance testing and optimization

**🎨 Design:**
- UI/UX design for future GUI
- CLI interface improvements
- Visual identity and branding

### Getting Started

1. **Fork the repository** on GitHub
2. **Clone your fork**: `git clone https://github.com/YOUR-USERNAME/pd-os.git`
3. **Create a branch**: `git checkout -b feature/your-feature-name`
4. **Make your changes** and test thoroughly
5. **Commit**: `git commit -m "Description of changes"`
6. **Push**: `git push origin feature/your-feature-name`
7. **Create a Pull Request** on GitHub

### Code Style

- Assembly: NASM syntax, clear comments explaining what and why
- C (future): K&R style, descriptive variable names
- Keep code readable - we're building something from scratch, clarity matters!

### Communication

- Open issues for bugs, features, or questions
- Discuss major changes before implementing
- Be respectful and collaborative - we're all learning!

### What We're Looking For

- **Experienced OS developers** - Architecture guidance and code reviews
- **Low-level programmers** - Assembly, C, system programming
- **Hardware enthusiasts** - Driver development, hardware debugging
- **Educators** - Help make this a great learning resource
- **Beginners** - Everyone starts somewhere! Documentation and testing are valuable

**Together, we can build something amazing!** 🚀

---

## 📝 License

This project is open source and available for educational purposes.

---

## 🎓 About

**Project**: PD-OS  
**Version**: 0.1.0 (Phase 2 Complete)  
**Started**: April 2026  
**Target**: Custom 32-bit x86 operating system  
**Learning Focus**: Low-level programming, OS architecture, systems development

---

## ✅ Next Steps for You

1. **Install missing tools** (NASM and i686-elf-gcc)
   - Follow the guide: [tools/setup.md](tools/setup.md)
   
2. **Build the bootloader**
   ```bash
   make all
   ```

3. **Test in QEMU**
   ```bash
   make run
   ```

4. **Begin Phase 3** (Bootloader Stage 2)
   - See [PD-OS-plan.md](PD-OS-plan.md) for detailed steps

---

**Status**: Phase 2 Complete ✓ | Ready for Phase 3  
**Last Updated**: April 11, 2026
