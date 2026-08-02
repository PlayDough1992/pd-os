# ============================================================================
# PD-OS Makefile - Root Build Configuration
# ============================================================================
# This Makefile orchestrates the build process for:
#   - PD-Bootloader (Stage 1 & Stage 2)
#   - PD-Kernel
#   - Disk image creation
#   - QEMU testing
# ============================================================================

# Project information
PROJECT_NAME = PD-OS
VERSION = 0.1.0

# Directories
BUILD_DIR = build
BOOTLOADER_DIR = bootloader
KERNEL_DIR = kernel
TOOLS_DIR = tools

# Output files
DISK_IMAGE     = $(BUILD_DIR)/pd-os.img
BOOTLOADER_BIN = $(BUILD_DIR)/bootloader.bin
STAGE2_BIN     = $(BUILD_DIR)/stage2.bin
KERNEL_ELF     = $(BUILD_DIR)/kernel.elf
KERNEL_BIN     = $(BUILD_DIR)/kernel.bin

# Tools — use i686-linux-gnu cross-toolchain (apt: gcc-i686-linux-gnu)
ASM     = nasm
CC      = i686-linux-gnu-gcc
LD      = i686-linux-gnu-ld
OBJCOPY = i686-linux-gnu-objcopy

# Assembler flags
ASMFLAGS = -f binASMFLAGS  = -f bin
# Compiler flags for kernel (will be used later)
CFLAGS = -m32 -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
         -nostartfiles -nodefaultlibs -fno-pic -fno-pie -Wall -Wextra -Werror -c

# Linker flags
LDFLAGS = -m elf_i386 -nostdlib

# QEMU settings
QEMU = qemu-system-i386
QEMU_FLAGS = -drive format=raw,file=$(DISK_IMAGE) -m 128M

# Disk image size (in 512-byte blocks)
# 2880 blocks = 1.44MB floppy disk
DISK_SIZE = 2880

# ============================================================================
# Phony targets (don't represent actual files)
# ============================================================================
.PHONY: all clean bootloader kernel run run-debug help setup-check

# ============================================================================
# Default target
# ============================================================================
all: $(DISK_IMAGE)
	@echo "=== Build complete! ==="
	@echo "Disk image: $(DISK_IMAGE)"
	@echo "Run with: make run"

# ============================================================================
# Help menu
# ============================================================================
help:
	@echo "PD-OS Build System - Available targets:"
	@echo ""
	@echo "  make all         - Build complete OS (bootloader + kernel + disk image)"
	@echo "  make bootloader  - Build only bootloader"
	@echo "  make kernel      - Build only kernel (Phase 4+)"
	@echo "  make run         - Build and run in QEMU"
	@echo "  make run-debug   - Run in QEMU with debugging output"
	@echo "  make clean       - Remove all build artifacts"
	@echo "  make setup-check - Verify toolchain installation"
	@echo "  make help        - Show this help message"
	@echo ""

# ============================================================================
# Disk image creation
# ============================================================================
$(DISK_IMAGE): $(BOOTLOADER_BIN) $(STAGE2_BIN) $(KERNEL_BIN) | $(BUILD_DIR)
	@echo "Creating disk image..."
	dd if=/dev/zero          of=$(DISK_IMAGE) bs=512 count=$(DISK_SIZE) 2>/dev/null
	dd if=$(BOOTLOADER_BIN)  of=$(DISK_IMAGE) conv=notrunc bs=512 seek=0 count=1 2>/dev/null
	dd if=$(STAGE2_BIN)      of=$(DISK_IMAGE) conv=notrunc bs=512 seek=1 2>/dev/null
	dd if=$(KERNEL_BIN)      of=$(DISK_IMAGE) conv=notrunc bs=512 seek=6 2>/dev/null
	@echo "✓ Disk image created: $(DISK_IMAGE)"

# ============================================================================
# Bootloader build (Stage 1)
# ============================================================================
bootloader: $(BOOTLOADER_BIN)

$(BOOTLOADER_BIN): $(BOOTLOADER_DIR)/stage1.asm | $(BUILD_DIR)
	@echo "Building PD-Bootloader Stage 1..."
	$(ASM) -f bin $(BOOTLOADER_DIR)/stage1.asm -o $(BOOTLOADER_BIN)
	@if [ $$(wc -c < $(BOOTLOADER_BIN)) -ne 512 ]; then \
		echo "ERROR: Bootloader must be exactly 512 bytes!"; \
		exit 1; \
	fi
	@echo "✓ Stage 1: $(BOOTLOADER_BIN) (512 bytes)"

$(STAGE2_BIN): $(BOOTLOADER_DIR)/stage2.asm | $(BUILD_DIR)
	@echo "Building PD-Bootloader Stage 2..."
	$(ASM) -f bin $(BOOTLOADER_DIR)/stage2.asm -o $(STAGE2_BIN)
	@echo "✓ Stage 2: $(STAGE2_BIN) ($$(wc -c < $(STAGE2_BIN)) bytes)"

# ============================================================================
# Kernel build
# ============================================================================
KERNEL_SRCS_C = $(KERNEL_DIR)/drivers/vga.c \
                $(KERNEL_DIR)/core/io.c \
                $(KERNEL_DIR)/core/panic.c \
                $(KERNEL_DIR)/core/kernel.c
KERNEL_SRCS_ASM = $(KERNEL_DIR)/arch/x86/entry.asm
KERNEL_OBJS = $(BUILD_DIR)/entry.o \
              $(BUILD_DIR)/vga.o \
              $(BUILD_DIR)/io.o \
              $(BUILD_DIR)/panic.o \
              $(BUILD_DIR)/kernel_main.o

kernel: $(KERNEL_BIN)

$(BUILD_DIR)/entry.o: $(KERNEL_DIR)/arch/x86/entry.asm | $(BUILD_DIR)
	$(ASM) -f elf32 $< -o $@

$(BUILD_DIR)/vga.o: $(KERNEL_DIR)/drivers/vga.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(KERNEL_DIR)/include $< -o $@

$(BUILD_DIR)/io.o: $(KERNEL_DIR)/core/io.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(KERNEL_DIR)/include $< -o $@

$(BUILD_DIR)/panic.o: $(KERNEL_DIR)/core/panic.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(KERNEL_DIR)/include $< -o $@

$(BUILD_DIR)/kernel_main.o: $(KERNEL_DIR)/core/kernel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(KERNEL_DIR)/include $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -T $(KERNEL_DIR)/linker.ld $(KERNEL_OBJS) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@echo "✓ Kernel: $(KERNEL_BIN) ($$(wc -c < $(KERNEL_BIN)) bytes)"

# ============================================================================
# Run in QEMU
# ============================================================================
run: $(DISK_IMAGE)
	@echo "Starting PD-OS in QEMU..."
	@echo "Press Ctrl+Alt+G to release mouse, Ctrl+C to quit"
	$(QEMU) $(QEMU_FLAGS)

# Run with serial output and debugging
run-debug: $(DISK_IMAGE)
	@echo "Starting PD-OS in QEMU (debug mode)..."
	$(QEMU) $(QEMU_FLAGS) -serial stdio -no-reboot -d cpu_reset

# ============================================================================
# Clean build artifacts
# ============================================================================
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)
	@echo "✓ Clean complete"

# ============================================================================
# Setup verification
# ============================================================================
setup-check:
	@echo "=== Verifying PD-OS Toolchain Setup ==="
	@echo ""
	
	@echo -n "Checking NASM... "
	@which $(ASM) > /dev/null 2>&1 && echo "✓ Found" || echo "✗ Not found"
	
	@echo -n "Checking i686-linux-gnu-gcc... "
	@which $(CC) > /dev/null 2>&1 && echo "✓ Found" || echo "✗ Not found"
	
	@echo -n "Checking i686-linux-gnu-ld... "
	@which $(LD) > /dev/null 2>&1 && echo "✓ Found" || echo "✗ Not found"
	
	@echo -n "Checking QEMU... "
	@which $(QEMU) > /dev/null 2>&1 && echo "✓ Found" || echo "✗ Not found"
	
	@echo ""
	@echo "If any tools are missing, see tools/setup.md for installation instructions"

# ============================================================================
# Build directory creation
# ============================================================================
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ============================================================================
# Additional info
# ============================================================================
version:
	@echo "$(PROJECT_NAME) version $(VERSION)"
