# ============================================================================
# PD-OS FULLY AUTOMATED SETUP
# ============================================================================
# This script will automatically install EVERYTHING you need
# Just run this script and wait - it handles everything!
# ============================================================================

param(
    [switch]$SkipNASM,
    [switch]$SkipGCC
)

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  PD-OS FULLY AUTOMATED SETUP" -ForegroundColor Cyan
Write-Host "  Sit back and relax - this will take a few minutes" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

# Check if running as Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[WARNING] Not running as Administrator" -ForegroundColor Yellow
    Write-Host "Some operations may fail. Trying anyway..." -ForegroundColor Yellow
    Write-Host ""
}

# Create downloads directory
$downloadDir = ".\tools-download"
if (-not (Test-Path $downloadDir)) {
    New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
}

$totalSteps = 6
$currentStep = 0

# ============================================================================
# Step 1: Download NASM
# ============================================================================
$currentStep++
if (-not $SkipNASM) {
    Write-Host "[$currentStep/$totalSteps] Downloading NASM Assembler..." -ForegroundColor Yellow
    
    $nasmUrl = "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-installer-x64.exe"
    $nasmInstaller = "$downloadDir\nasm-installer.exe"
    
    if (Test-Path $nasmInstaller) {
        Write-Host "  [SKIP] NASM installer already downloaded" -ForegroundColor Gray
    } else {
        try {
            $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $nasmUrl -OutFile $nasmInstaller -UseBasicParsing
            Write-Host "  [OK] NASM downloaded successfully" -ForegroundColor Green
        } catch {
            Write-Host "  [ERROR] Failed to download NASM: $_" -ForegroundColor Red
            Write-Host "  Please download manually from: https://www.nasm.us/" -ForegroundColor Yellow
            exit 1
        }
    }
} else {
    Write-Host "[$currentStep/$totalSteps] Skipping NASM download" -ForegroundColor Gray
}

# ============================================================================
# Step 2: Install NASM Silently
# ============================================================================
$currentStep++
if (-not $SkipNASM) {
    Write-Host "[$currentStep/$totalSteps] Installing NASM (this may take 30 seconds)..." -ForegroundColor Yellow
    
    # Check if already installed
    $nasmCheck = Get-Command nasm -ErrorAction SilentlyContinue
    if ($nasmCheck) {
        Write-Host "  [SKIP] NASM already installed at: $($nasmCheck.Source)" -ForegroundColor Gray
    } else {
        try {
            # Install silently with /S flag
            $nasmInstaller = "$downloadDir\nasm-installer.exe"
            $process = Start-Process -FilePath $nasmInstaller -ArgumentList "/S" -Wait -PassThru
            
            if ($process.ExitCode -eq 0) {
                Write-Host "  [OK] NASM installed successfully" -ForegroundColor Green
                
                # Add to current session PATH
                $nasmPath = "C:\Program Files\NASM"
                if (Test-Path $nasmPath) {
                    $env:Path += ";$nasmPath"
                    Write-Host "  [OK] NASM added to current session PATH" -ForegroundColor Green
                }
            } else {
                Write-Host "  [WARNING] NASM installer returned code: $($process.ExitCode)" -ForegroundColor Yellow
            }
        } catch {
            Write-Host "  [ERROR] Failed to install NASM: $_" -ForegroundColor Red
            Write-Host "  Continuing anyway..." -ForegroundColor Yellow
        }
    }
} else {
    Write-Host "[$currentStep/$totalSteps] Skipping NASM installation" -ForegroundColor Gray
}

# ============================================================================
# Step 3: Download Cross-Compiler
# ============================================================================
$currentStep++
if (-not $SkipGCC) {
    Write-Host "[$currentStep/$totalSteps] Downloading i686-elf-gcc Cross-Compiler..." -ForegroundColor Yellow
    Write-Host "  This is a large file (~270 MB), please be patient..." -ForegroundColor Cyan
    
    $gccUrl = "https://github.com/lordmilko/i686-elf-tools/releases/download/13.2.0/i686-elf-tools-windows-13.2.0.zip"
    $gccZip = "$downloadDir\i686-elf-tools.zip"
    
    if (Test-Path $gccZip) {
        Write-Host "  [SKIP] Cross-compiler already downloaded" -ForegroundColor Gray
    } else {
        try {
            # Show progress for large file
            $ProgressPreference = 'SilentlyContinue'
            Write-Host "  Downloading... (this will take 2-5 minutes)" -ForegroundColor Cyan
            
            Invoke-WebRequest -Uri $gccUrl -OutFile $gccZip -UseBasicParsing
            
            Write-Host "  [OK] Cross-compiler downloaded successfully" -ForegroundColor Green
        } catch {
            Write-Host "  [ERROR] Failed to download: $_" -ForegroundColor Red
            Write-Host "  Download manually from: https://github.com/lordmilko/i686-elf-tools/releases" -ForegroundColor Yellow
            Write-Host "  Note: This is only needed for Phase 4+ (kernel development)" -ForegroundColor Yellow
            $SkipGCC = $true
        }
    }
} else {
    Write-Host "[$currentStep/$totalSteps] Skipping cross-compiler download" -ForegroundColor Gray
}

# ============================================================================
# Step 4: Extract Cross-Compiler
# ============================================================================
$currentStep++
if (-not $SkipGCC -and (Test-Path "$downloadDir\i686-elf-tools.zip")) {
    Write-Host "[$currentStep/$totalSteps] Extracting Cross-Compiler..." -ForegroundColor Yellow
    
    $gccExtractPath = "C:\i686-elf-tools"
    
    if (Test-Path "$gccExtractPath\bin\i686-elf-gcc.exe") {
        Write-Host "  [SKIP] Cross-compiler already extracted" -ForegroundColor Gray
    } else {
        try {
            Write-Host "  Extracting to $gccExtractPath..." -ForegroundColor Cyan
            
            # Create directory if it doesn't exist
            if (-not (Test-Path $gccExtractPath)) {
                New-Item -ItemType Directory -Path $gccExtractPath -Force | Out-Null
            }
            
            # Extract (this may take a minute)
            Expand-Archive -Path "$downloadDir\i686-elf-tools.zip" -DestinationPath $gccExtractPath -Force
            
            Write-Host "  [OK] Cross-compiler extracted successfully" -ForegroundColor Green
        } catch {
            Write-Host "  [ERROR] Failed to extract: $_" -ForegroundColor Red
            Write-Host "  Please extract manually to C:\i686-elf-tools" -ForegroundColor Yellow
        }
    }
} else {
    Write-Host "[$currentStep/$totalSteps] Skipping cross-compiler extraction" -ForegroundColor Gray
}

# ============================================================================
# Step 5: Add Tools to PATH
# ============================================================================
$currentStep++
Write-Host "[$currentStep/$totalSteps] Configuring System PATH..." -ForegroundColor Yellow

try {
    $pathsToAdd = @()
    
    # Add NASM to PATH
    if (Test-Path "C:\Program Files\NASM") {
        $pathsToAdd += "C:\Program Files\NASM"
    }
    
    # Add cross-compiler to PATH
    if (Test-Path "C:\i686-elf-tools\bin") {
        $pathsToAdd += "C:\i686-elf-tools\bin"
    }
    
    if ($pathsToAdd.Count -gt 0) {
        $currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
        
        foreach ($path in $pathsToAdd) {
            if ($currentPath -notlike "*$path*") {
                $currentPath += ";$path"
                Write-Host "  [OK] Added to PATH: $path" -ForegroundColor Green
            } else {
                Write-Host "  [SKIP] Already in PATH: $path" -ForegroundColor Gray
            }
            
            # Add to current session
            if ($env:Path -notlike "*$path*") {
                $env:Path += ";$path"
            }
        }
        
        # Save to user PATH
        [Environment]::SetEnvironmentVariable("Path", $currentPath, "User")
        Write-Host "  [OK] PATH updated successfully" -ForegroundColor Green
    } else {
        Write-Host "  [SKIP] No new paths to add" -ForegroundColor Gray
    }
} catch {
    Write-Host "  [WARNING] Could not update system PATH: $_" -ForegroundColor Yellow
    Write-Host "  Tools added to current session only" -ForegroundColor Yellow
}

# ============================================================================
# Step 6: Verify Installation
# ============================================================================
$currentStep++
Write-Host "[$currentStep/$totalSteps] Verifying Installations..." -ForegroundColor Yellow
Write-Host ""

$allGood = $true

# Check NASM
Write-Host "  Checking NASM..." -ForegroundColor Cyan
$nasmCmd = Get-Command nasm -ErrorAction SilentlyContinue
if ($nasmCmd) {
    $nasmVersion = & nasm -v 2>&1
    Write-Host "    [OK] NASM: $nasmVersion" -ForegroundColor Green
} else {
    Write-Host "    [FAIL] NASM not found in PATH" -ForegroundColor Red
    Write-Host "    Try: Restart PowerShell and check again" -ForegroundColor Yellow
    $allGood = $false
}

# Check QEMU
Write-Host "  Checking QEMU..." -ForegroundColor Cyan
$qemuCmd = Get-Command qemu-system-i386 -ErrorAction SilentlyContinue
if ($qemuCmd) {
    $qemuVersion = & qemu-system-i386 --version 2>&1 | Select-Object -First 1
    Write-Host "    [OK] QEMU: $qemuVersion" -ForegroundColor Green
} else {
    Write-Host "    [FAIL] QEMU not found" -ForegroundColor Red
    Write-Host "    QEMU is required for testing" -ForegroundColor Yellow
    $allGood = $false
}

# Check Cross-Compiler
Write-Host "  Checking i686-elf-gcc..." -ForegroundColor Cyan
$gccCmd = Get-Command i686-elf-gcc -ErrorAction SilentlyContinue
if ($gccCmd) {
    $gccVersion = & i686-elf-gcc --version 2>&1 | Select-Object -First 1
    Write-Host "    [OK] GCC: $gccVersion" -ForegroundColor Green
} else {
    Write-Host "    [INFO] Cross-compiler not found (only needed for Phase 4+)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan

if ($allGood) {
    Write-Host "  SETUP COMPLETE - ALL TOOLS READY!" -ForegroundColor Green
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "You can now build and run PD-OS:" -ForegroundColor White
    Write-Host ""
    Write-Host "  .\build.ps1 all      # Build the bootloader" -ForegroundColor Cyan
    Write-Host "  .\build.ps1 run      # Build and run in QEMU" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Or just run this to build and test right now:" -ForegroundColor Yellow
    Write-Host "  .\build.ps1 run" -ForegroundColor White
    Write-Host ""
    
    # Ask if they want to build now
    Write-Host "Would you like to build and run PD-OS now? (Y/N)" -ForegroundColor Yellow
    $response = Read-Host
    if ($response -eq 'Y' -or $response -eq 'y') {
        Write-Host ""
        Write-Host "Starting build process..." -ForegroundColor Cyan
        & ".\build.ps1" run
    }
} else {
    Write-Host "  SETUP INCOMPLETE - Some tools missing" -ForegroundColor Yellow
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Please restart PowerShell and run again:" -ForegroundColor Yellow
    Write-Host "  .\tools\autosetup.ps1" -ForegroundColor White
    Write-Host ""
}
