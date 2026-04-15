# ============================================================================
# PD-OS Build Script (Windows PowerShell - WSL wrapper)
# ============================================================================
# Usage: .\build.ps1 [all|run|run-debug|clean|setup-check]
# Delegates all compilation to WSL (Ubuntu) using build.sh
# ============================================================================

param(
    [string]$Target = "all"
)

$ErrorActionPreference = "Stop"

$DISK_IMAGE = "build/pd-os.img"

function Write-Success {
    param([string]$Message)
    Write-Host $Message -ForegroundColor Green
}

function Write-Info {
    param([string]$Message)
    Write-Host $Message -ForegroundColor Cyan
}

function Assert-WSL {
    $wsl = Get-Command wsl -ErrorAction SilentlyContinue
    if (-not $wsl) {
        Write-Host "WSL is not installed. Please install WSL2 with Ubuntu." -ForegroundColor Red
        Write-Host "Run: wsl --install" -ForegroundColor Yellow
        exit 1
    }
}

function ConvertTo-WslPath {
    param([string]$WinPath)
    $abs = (Resolve-Path $WinPath).Path
    $drive = $abs.Substring(0,1).ToLower()
    $rest  = $abs.Substring(2).Replace('\', '/')
    return "/mnt/$drive$rest"
}

Assert-WSL

$wslProjectPath = ConvertTo-WslPath (Get-Location).Path

switch ($Target.ToLower()) {
    "clean" {
        Write-Info "Cleaning build directory..."
        wsl -e bash -c "cd '$wslProjectPath' && bash build.sh clean"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    "setup-check" {
        Write-Info "=== Verifying PD-OS Toolchain Setup (WSL) ==="
        wsl -e bash -c "cd '$wslProjectPath' && bash build.sh setup-check"
    }
    "run" {
        Write-Info "Building PD-OS via WSL..."
        wsl -e bash -c "cd '$wslProjectPath' && bash build.sh all"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Success "=== Build complete! ==="
        Write-Info "Starting QEMU..."
        Write-Info "Press Ctrl+Alt+G to release mouse, Ctrl+C to quit"
        & "C:\msys64\mingw64\bin\qemu-system-i386.exe" -drive format=raw,file=$DISK_IMAGE -m 128M
    }
    "run-debug" {
        Write-Info "Building PD-OS via WSL (debug)..."
        wsl -e bash -c "cd '$wslProjectPath' && bash build.sh all"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Success "=== Build complete! ==="
        Write-Info "Starting QEMU (debug mode)..."
        & "C:\msys64\mingw64\bin\qemu-system-i386.exe" -drive format=raw,file=$DISK_IMAGE -m 128M -serial stdio -no-reboot -d cpu_reset
    }
    "all" {
        Write-Info "Building PD-OS via WSL..."
        wsl -e bash -c "cd '$wslProjectPath' && bash build.sh all"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Success "=== Build complete! ==="
        Write-Info "Run with: .\build.ps1 run"
    }
    default {
        Write-Info "PD-OS Build System (Windows PowerShell - WSL wrapper)"
        Write-Info "Usage: .\build.ps1 [all|run|run-debug|clean|setup-check]"
    }
}