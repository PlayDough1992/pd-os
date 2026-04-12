# PD-OS Development Toolchain Setup Guide (Linux)

This guide covers setting up the complete development environment for building PD-OS on Linux.

## Required Tools

1. **NASM** - Netwide Assembler (for bootloader and low-level kernel code)
2. **i686-elf-gcc** - GCC cross-compiler targeting bare-metal 32-bit x86
3. **QEMU** - x86 system emulator for testing
4. **Make** - Build automation tool
5. **dd** - Disk image creation (standard Linux utility)

---

## Quick Install (Automated)

Run the automated setup script — it detects your distro and installs everything:

```bash
./tools/autosetup.sh
```

Or follow the manual steps below.

---

## Step 1: Install NASM

NASM is the assembler used for bootloader and CPU-specific assembly code.

### Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install nasm
```

### Fedora / RHEL / CentOS:
```bash
sudo dnf install nasm
```

### Arch Linux / Manjaro:
```bash
sudo pacman -S nasm
```

### Verify Installation:
```bash
nasm -v
```
**Expected output**: `NASM version 2.xx.xx` or similar

---

## Step 2: Install i686-elf Cross-Compiler (Phase 4+ only)

A bare-metal cross-compiler is needed for kernel development (not required for Phases 1–3).

### Ubuntu / Debian (Linux-GNU target, quick option):
```bash
sudo apt-get install gcc-i686-linux-gnu binutils-i686-linux-gnu
```
> Note: This installs `i686-linux-gnu-gcc`. For a true freestanding `i686-elf-gcc`, build from source (see [tools/MANUAL_INSTALL.md](MANUAL_INSTALL.md)).

### Arch Linux (via community/AUR):
```bash
sudo pacman -S cross-i686-elf-gcc cross-i686-elf-binutils
```

### Build from Source (all distros — recommended for OS development):
Follow the OSDev guide: https://wiki.osdev.org/GCC_Cross-Compiler

### Verify Installation:
```bash
i686-elf-gcc --version
i686-elf-ld --version
```
**Expected output**: `i686-elf-gcc (GCC) x.x.x`

---

## Step 3: Install QEMU

QEMU lets you test PD-OS without rebooting or using real hardware.

### Ubuntu / Debian:
```bash
sudo apt-get install qemu-system-x86
```

### Fedora / RHEL:
```bash
sudo dnf install qemu-system-x86
```

### Arch Linux:
```bash
sudo pacman -S qemu-arch-extra
```

### Verify Installation:
```bash
qemu-system-i386 --version
```
**Expected output**: `QEMU emulator version x.x.x`

---

## Step 4: Install Make

Make is the primary build tool (already present on most Linux systems).

### Ubuntu / Debian:
```bash
sudo apt-get install make
```

### Fedora / RHEL:
```bash
sudo dnf install make
```

### Arch Linux:
```bash
sudo pacman -S make
```

### Verify Installation:
```bash
make --version
```
**Expected output**: `GNU Make x.x`

---

## Step 5: Verify Complete Setup

Run the built-in toolchain check:

```bash
make setup-check
# or
./build.sh setup-check
```

Expected output:
```
=== Verifying PD-OS Toolchain Setup ===
Checking nasm...          ✓ Found
Checking i686-elf-gcc...  ✓ Found  (or ✗ Not found — OK until Phase 4)
Checking i686-elf-ld...   ✓ Found
Checking QEMU...          ✓ Found
```

---

## Troubleshooting

### "command not found" errors
- Confirm the package is installed: `which nasm`, `which qemu-system-i386`
- Make sure your package manager install succeeded (no errors)

### QEMU display issues
- Add `-display sdl` or `-display gtk` to the QEMU command if the window doesn't appear
- For headless environments: `qemu-system-i386 ... -nographic`

### Cross-compiler linking errors
- Ensure you're using `i686-elf-gcc`, not regular `gcc`
- Use `-ffreestanding -nostdlib -nostartfiles` flags
- Do not link against libc

### Make errors about tabs
- Makefile rules must use **tab** characters for indentation, not spaces

However, this guide assumes native Windows tools as requested.

---

## Next Steps

Once all tools are installed and verified:
1. Navigate to project root: `cd Custom-Bootloader-Kernel-CLI-GUI`
2. Test build system: `make help` (after Makefile is created)
3. Start Phase 2: Building PD-Bootloader

---

## Quick Reference

| Tool | Command | Purpose |
|------|---------|---------|
| NASM | `nasm -f bin file.asm -o file.bin` | Assemble bootloader |
| GCC | `i686-elf-gcc -c file.c -o file.o` | Compile kernel C code |
| LD | `i686-elf-ld -T linker.ld -o kernel.elf` | Link kernel |
| QEMU | `qemu-system-i386 -drive format=raw,file=os.img` | Test OS |
| Make | `make all` | Build entire project |
| dd | `dd if=boot.bin of=os.img` | Create disk image |

---

## Additional Resources

- NASM Manual: https://www.nasm.us/doc/
- OSDev Wiki: https://wiki.osdev.org/
- GCC Cross-Compiler: https://wiki.osdev.org/GCC_Cross-Compiler
- QEMU Documentation: https://www.qemu.org/docs/master/

---

**Status**: ✓ Setup guide complete  
**Next**: Create root Makefile and begin bootloader development
