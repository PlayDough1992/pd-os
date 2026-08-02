# PD-Bootloader Documentation

## Overview

PD-Bootloader is the custom bootloader for PD-OS. It's responsible for loading the operating system kernel into memory and transferring control to it.

The bootloader is split into two stages:
- **Stage 1**: Master Boot Record (512 bytes) - loaded by BIOS
- **Stage 2**: Extended bootloader - loads kernel and switches to protected mode

---

## Stage 1 - Master Boot Record

### Purpose
The MBR is the first code that runs when PD-OS boots. It's loaded by the BIOS at memory address `0x7C00` and must be exactly 512 bytes.

### What it does:
1. **Initialize CPU state**: Set up segment registers (DS, ES, SS) and stack pointer
2. **Display boot messages**: Use BIOS interrupt 0x10 to print to screen
3. **Load Stage 2**: Read additional bootloader code from disk into memory
4. **Jump to Stage 2**: Transfer control to extended bootloader

### Memory Layout (Real Mode)
```
0x0000 - 0x03FF : Interrupt Vector Table (IVT)
0x0400 - 0x04FF : BIOS Data Area (BDA)
0x0500 - 0x7BFF : Free memory (can be used for Stage 2)
0x7C00 - 0x7DFF : Stage 1 bootloader (this code, 512 bytes)
0x7E00 - 0x7FFFF : Free memory
```

### Boot Process
1. **BIOS Power-On Self Test (POST)**: Hardware initialization
2. **BIOS Boot Device Selection**: Reads first sector (512 bytes) from boot device
3. **Boot Signature Check**: BIOS verifies bytes 510-511 are `0x55 0xAA`
4. **Load at 0x7C00**: BIOS copies 512 bytes to memory address 0x7C00
5. **Jump to 0x7C00**: CPU begins executing our bootloader code
6. **Stage 1 Execution**: Our code runs in 16-bit real mode

### Code Structure

#### 1. Entry Point
```asm
[BITS 16]           ; 16-bit real mode
[ORG 0x7C00]        ; Code is loaded at 0x7C00
```

#### 2. Segment Initialization
```asm
xor ax, ax          ; AX = 0
mov ds, ax          ; Data Segment = 0
mov es, ax          ; Extra Segment = 0
mov ss, ax          ; Stack Segment = 0
mov sp, 0x7C00      ; Stack grows down from bootloader
```

**Why?** In real mode, physical address = (segment * 16) + offset. Setting segments to 0 simplifies addressing.

#### 3. BIOS Interrupts
We use BIOS interrupt `INT 0x10` (video services) to print messages:
```asm
mov ah, 0x0E        ; Teletype output function
mov al, 'A'         ; Character to print
int 0x10            ; Call BIOS
```

#### 4. Boot Signature
```asm
times 510-($-$$) db 0   ; Pad with zeros
dw 0xAA55               ; Boot signature (little-endian)
```
- `$` = current address
- `$$` = start of section
- `$-$$` = bytes used so far
- `510-($-$$)` = bytes to pad

---

## Stage 2 - Extended Bootloader (Phase 3)

**Status**: Not yet implemented (will be created in Phase 3)

### Purpose
Stage 2 performs the complex tasks that don't fit in 512 bytes:

1. **Enable A20 Line**: Access memory beyond 1MB
2. **Set up GDT**: Create Global Descriptor Table for protected mode
3. **Switch to 32-bit Protected Mode**: Leave 16-bit real mode
4. **Detect Memory**: Query BIOS for available RAM
5. **Load Kernel**: Read kernel from disk into memory (at 0x100000)
6. **Jump to Kernel**: Transfer control to kernel entry point

### Memory Map (After Stage 2)
```
0x00000000 - 0x000003FF : IVT (not used after protected mode)
0x00000500 - 0x00007BFF : Stage 2 bootloader code
0x00007C00 - 0x00007DFF : Stage 1 bootloader
0x00100000 (1MB)        : Kernel loaded here
```

### A20 Line
- In real mode, only 20 address lines are available (1MB)
- A20 gate must be enabled to access addresses above 1MB
- Required before loading kernel

### Protected Mode Transition
1. Disable interrupts (`cli`)
2. Load GDT (`lgdt`)
3. Set PE bit in CR0 register
4. Far jump to flush CPU pipeline
5. Set up protected mode segments
6. Now in 32-bit mode!

### GDT (Global Descriptor Table)
Defines memory segments for protected mode:
- **Null Descriptor**: Required by CPU (all zeros)
- **Code Segment**: Executable, readable, ring 0
- **Data Segment**: Writable, not executable, ring 0

---

## Building the Bootloader

### Using Make (Recommended)
```bash
# Build bootloader only
make bootloader

# Build full disk image
make all

# Build and run in QEMU
make run
```

### Manual Build
```bash
# Assemble Stage 1
nasm -f bin bootloader/stage1.asm -o build/bootloader.bin

# Verify size
# Should output "512 build/bootloader.bin"
wc -c build/bootloader.bin

# Create disk image (Linux/Git Bash)
dd if=/dev/zero of=build/pd-os.img bs=512 count=2880
dd if=build/bootloader.bin of=build/pd-os.img conv=notrunc

# Run in QEMU
qemu-system-i386 -drive format=raw,file=build/pd-os.img
```

### Using PowerShell Script
```powershell
# Build bootloader
make bootloader

# Create disk image (Windows native)
.\tools\create-image.ps1
```

---

## Testing

### QEMU
Best for development - fast and easy to debug:
```bash
# Normal run
make run

# With debugging
make run-debug

# Manual QEMU invocation
qemu-system-i386 -drive format=raw,file=build/pd-os.img -m 128M
```

### Bochs
Better debugging interface, slower:
```bash
bochs -f tools/bochs-config.txt
```

### Real Hardware
**Advanced**: Boot from USB (after thoroughly testing in emulator):
```bash
# Linux - BE VERY CAREFUL WITH DEVICE NAME!
sudo dd if=build/pd-os.img of=/dev/sdX bs=4M
sync

# Windows - Use Rufus utility in DD mode
```

---

## Troubleshooting

### "No bootable device" error
- Boot signature missing or incorrect
- Verify bytes 510-511 are `0x55 0xAA` (little-endian: `0xAA55`)
- Check: `xxd build/bootloader.bin | tail`

### Bootloader is wrong size
```bash
# Check size
wc -c build/bootloader.bin

# Should be exactly 512 bytes
# If not, check padding calculation in stage1.asm
```

### Nothing appears on screen
- BIOS interrupts not working
- Try adding more NOP instructions before first INT call
- Check segment registers are initialized

### Triple fault / Immediate reboot
- Usually caused by invalid instruction or stack corruption
- Use QEMU debug mode: `qemu-system-i386 -d cpu_reset ...`
- Check that stack pointer (SP) is set correctly

### "dd: command not found" (Windows)
- Install Git Bash (includes Unix tools)
- Or use PowerShell script: `.\tools\create-image.ps1`
- Or use WSL

---

## Learning Resources

### BIOS Interrupts
- **INT 0x10**: Video services (we use function 0x0E for teletype)
- **INT 0x13**: Disk services (for loading Stage 2)
- **INT 0x15**: Memory detection

Reference: [BIOS Interrupt List](http://www.ctyme.com/intr/int.htm)

### x86 Real Mode
- 16-bit mode with segmented memory
- Address calculation: `physical = (segment << 4) + offset`
- Maximum addressable: 1MB (20 address lines)

### Assembly Directives
- `[BITS 16]`: Assemble 16-bit code
- `[ORG 0x7C00]`: Set origin address
- `times N db 0`: Repeat byte N times
- `dw`: Define word (2 bytes)

---

## Next Steps

**Current Status**: Phase 2 Complete ✓
- ✅ Stage 1 bootloader implemented
- ✅ Prints welcome messages
- ✅ Boot signature verified
- ✅ Builds and runs in QEMU

**Phase 3**: Implement Stage 2
- ⬜ Load Stage 2 from disk
- ⬜ Enable A20 line
- ⬜ Set up GDT
- ⬜ Switch to protected mode
- ⬜ Load kernel at 0x100000

**Phase 4**: Kernel Foundation
- ⬜ Create kernel entry point
- ⬜ VGA text mode driver
- ⬜ printf() implementation

---

## File Locations

```
bootloader/
├── stage1.asm          - MBR bootloader (Phase 2) ✓
├── stage2.asm          - Extended bootloader (Phase 3)
├── gdt.asm             - GDT setup (Phase 3)
├── a20.asm             - A20 enabling (Phase 3)
├── disk.asm            - Disk I/O routines (Phase 3)
├── linker.ld           - Bootloader linker script (Phase 3)
└── README.md           - This file ✓
```

---

**Version**: 0.1.0 (Phase 2)  
**Last Updated**: April 2026  
**Status**: Stage 1 Complete, Stage 2 Pending
