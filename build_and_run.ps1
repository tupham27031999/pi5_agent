param (
    [int]$Port = 8080,
    [string]$Workspace = "workspace"
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -Path $ScriptDir

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "   BUILD & RUN PI 5 AGENT ON WINDOWS (SIMULATION)         " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Tim trinh bien dich g++
$gppCmd = "g++"
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $candidatePaths = @(
        "C:\msys64\ucrt64\bin\g++.exe",
        "C:\msys64\mingw64\bin\g++.exe",
        "C:\mingw64\bin\g++.exe",
        "C:\MinGW\bin\g++.exe",
        "C:\ProgramData\chocolatey\bin\g++.exe",
        "C:\w64devkit\bin\g++.exe"
    )
    $found = $null
    foreach ($p in $candidatePaths) {
        if (Test-Path $p) {
            $found = $p
            break
        }
    }
    if (-not $found) {
        $wingetPaths = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter "g++.exe" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
        if ($wingetPaths) {
            $found = $wingetPaths[0]
        }
    }

    if ($found) {
        $gppCmd = $found
        $binDir = Split-Path $gppCmd
        $env:PATH = "$binDir;$env:PATH"
        Write-Host "[+] Found g++ at: $gppCmd" -ForegroundColor Gray
    } else {
        Write-Host "[ERROR] g++ compiler not found on this system!" -ForegroundColor Red
        Write-Host "        Please install MinGW/MSYS2 or run directly on Raspberry Pi 5." -ForegroundColor Yellow
        exit 1
    }
}

if (-not (Test-Path "build")) { New-Item -ItemType Directory -Path "build" | Out-Null }
if (-not (Test-Path $Workspace)) { New-Item -ItemType Directory -Path $Workspace | Out-Null }

$srcs = @(
    "src/main.cpp",
    "src/http_server.cpp",
    "src/process_runner.cpp",
    "src/zip_unpacker.cpp"
)

Write-Host "[*] Compiling pi5_agent_server.exe..." -ForegroundColor Yellow
& $gppCmd -std=c++17 -finput-charset=UTF-8 -fexec-charset=UTF-8 -I include $srcs -lws2_32 -ladvapi32 -o build/pi5_agent_server.exe

if ($LASTEXITCODE -eq 0 -and (Test-Path "build/pi5_agent_server.exe")) {
    Write-Host "[OK] Compilation successful!" -ForegroundColor Green
    Write-Host ""
    Write-Host ">> STARTING DAEMON AGENT ON PORT $Port..." -ForegroundColor Green
    & "build/pi5_agent_server.exe" $Port $Workspace
} else {
    Write-Host "[FAIL] Compilation failed!" -ForegroundColor Red
    exit 1
}
