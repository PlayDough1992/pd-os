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
DISK_IMAGE = $(BUILD_DIR)/pd-os.img
BOOTLOADER_BIN = $(BUILD_DIR)/bootloader.bin
KERNEL_BIN = $(BUILD_DIR)/kernel.bin

# Tools - adjust paths if needed
ASM = nasm
CC = i686-elf-gcc
LD = i686-elf-ld
OBJCOPY = i686-elf-objcopy

# Assembler flags
ASMFLAGS = -f bin

# Compiler flags for kernel (will be used later)
CFLAGS = -m32 -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
         -nostartfiles -nodefaultlibs -Wall -Wextra -Werror -c

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
$(DISK_IMAGE): $(BOOTLOADER_BIN) | $(BUILD_DIR)
	@echo "Creating disk image..."
	
	# Create a blank disk image (1.44MB floppy)
	dd if=/dev/zero of=$(DISK_IMAGE) bs=512 count=$(DISK_SIZE) 2>/dev/null || \
		(echo "Error: dd command failed. Ensure Git Bash or MinGW is installed." && exit 1)
	
	# Write bootloader to first sector
	dd if=$(BOOTLOADER_BIN) of=$(DISK_IMAGE) conv=notrunc bs=512 count=1 2>/dev/null
	
	@echo "✓ Disk image created: $(DISK_IMAGE)"

# ============================================================================
# Bootloader build (Stage 1)
# ============================================================================
bootloader: $(BOOTLOADER_BIN)

$(BOOTLOADER_BIN): $(BOOTLOADER_DIR)/stage1.asm | $(BUILD_DIR)
	@echo "Building PD-Bootloader Stage 1..."
	$(ASM) $(ASMFLAGS) $(BOOTLOADER_DIR)/stage1.asm -o $(BOOTLOADER_BIN)
	
	# Verify bootloader is exactly 512 bytes
	@if [ $$(wc -c < $(BOOTLOADER_BIN)) -ne 512 ]; then \
		echo "ERROR: Bootloader must be exactly 512 bytes!"; \
		echo "Current size: $$(wc -c < $(BOOTLOADER_BIN)) bytes"; \
		exit 1; \
	fi
	
	@echo "✓ Bootloader built: $(BOOTLOADER_BIN) (512 bytes)"

# ============================================================================
# Kernel build (Phase 4+)
# ============================================================================
kernel: $(KERNEL_BIN)

$(KERNEL_BIN):
	@echo "Kernel build not yet implemented (Phase 4)"
	@echo "Creating placeholder kernel..."
	@mkdir -p $(BUILD_DIR)
	@echo -n "KERNEL" > $(KERNEL_BIN)

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
	
	@echo -n "Checking i686-elf-gcc... "
	@which $(CC) > /dev/null 2>&1 && echo "✓ Found" || echo "✗ Not found"
	
	@echo -n "Checking i686-elf-ld... "
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
