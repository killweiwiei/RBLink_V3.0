param(
    [ValidateSet("build", "clean", "flash", "monitor")]
    [string]$Action = "build",
    [string]$Port = ""
)

$IdfRoot = "D:\Program Files (x86)\Espressif\.espressif\v5.5.5\esp-idf"
$ToolsRoot = "D:\Program Files (x86)\Espressif\.espressif\v5.5.5\tools"
$IdfPython = "$ToolsRoot\python_env\idf5.5_py3.12_env\Scripts\python.exe"
$IdfCommand = "$IdfRoot\tools\idf.py"
$IdfRunner = Join-Path $PSScriptRoot "tools\idf_runner.py"
$Ninja = "$ToolsRoot\ninja\1.12.1\ninja.exe"
$ToolchainBin = "$ToolsRoot\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin"
$CCompiler = "$ToolchainBin\riscv32-esp-elf-gcc.exe"
$CxxCompiler = "$ToolchainBin\riscv32-esp-elf-g++.exe"

if (-not (Test-Path -LiteralPath $IdfPython)) {
    throw "ESP-IDF Python environment was not found at: $IdfPython"
}
if (-not (Test-Path -LiteralPath $Ninja)) {
    throw "Ninja was not found at: $Ninja"
}
if (-not (Test-Path -LiteralPath $CCompiler)) {
    throw "ESP32-C3 compiler was not found at: $CCompiler"
}

$env:IDF_PATH = $IdfRoot
$env:IDF_TOOLS_PATH = $ToolsRoot
$env:IDF_PYTHON_ENV_PATH = "$ToolsRoot\python_env\idf5.5_py3.12_env"
$env:PIP_CACHE_DIR = "D:\Program Files (x86)\Espressif\pip-cache"
$env:ESP_ROM_ELF_DIR = "$ToolsRoot\esp-rom-elfs\20241011"
$env:OPENOCD_SCRIPTS = "$ToolsRoot\openocd-esp32\v0.12.0-esp32-20260424\openocd-esp32\share\openocd\scripts"
$env:CC = $CCompiler
$env:CXX = $CxxCompiler
$env:ASM = $CCompiler
$env:RBLINK_IDF_PATH_PREPEND = @(
    "$ToolsRoot\ninja\1.12.1",
    "$ToolsRoot\cmake\3.30.2\bin",
    "$ToolsRoot\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    $ToolchainBin,
    "$ToolsRoot\openocd-esp32\v0.12.0-esp32-20260424\openocd-esp32\bin"
) -join ";"
$env:PATH = @(
    "$ToolsRoot\ninja\1.12.1",
    "$ToolsRoot\cmake\3.30.2\bin",
    "$ToolsRoot\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    $ToolchainBin,
    "$ToolsRoot\openocd-esp32\v0.12.0-esp32-20260424\openocd-esp32\bin",
    $env:PATH
) -join ";"

[string[]]$IdfArguments = @(switch ($Action) {
    "build"   { @("-D", "CMAKE_MAKE_PROGRAM=$Ninja",
                    "-D", "CMAKE_C_COMPILER=$CCompiler",
                    "-D", "CMAKE_CXX_COMPILER=$CxxCompiler",
                    "-D", "CMAKE_ASM_COMPILER=$CCompiler", "build") }
    "clean"   { @("fullclean") }
    "flash"   {
        if ([string]::IsNullOrWhiteSpace($Port)) {
            throw "Flash requires a serial port, for example: .\build_idf.ps1 flash COM8"
        }
        @("-p", $Port, "flash")
    }
    "monitor" {
        if ([string]::IsNullOrWhiteSpace($Port)) {
            throw "Monitor requires a serial port, for example: .\build_idf.ps1 monitor COM8"
        }
        @("-p", $Port, "monitor")
    }
})

Push-Location $PSScriptRoot
try {
    & $IdfPython $IdfRunner $IdfCommand $IdfArguments
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py exited with code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
