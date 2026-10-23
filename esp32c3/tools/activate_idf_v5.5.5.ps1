# VS Code ESP-IDF extension invokes this space-free wrapper. EIM stores tools
# in a version-scoped directory, so export the validated paths explicitly.
$toolsRoot = 'E:\Esp\.espressif\v5.5.5\tools'
$env:IDF_PATH = 'E:\Esp\.espressif\v5.5.5\esp-idf'
$env:IDF_TOOLS_PATH = $toolsRoot
$env:IDF_PYTHON_ENV_PATH = "$toolsRoot\python_env\idf5.5_py3.12_env"
$env:ESP_ROM_ELF_DIR = "$toolsRoot\esp-rom-elfs\20241011"
$env:OPENOCD_SCRIPTS = "$toolsRoot\openocd-esp32\v0.12.0-esp32-20260424\openocd-esp32\share\openocd\scripts"
$env:PATH = @(
    "$env:IDF_PYTHON_ENV_PATH\Scripts",
    "$toolsRoot\ninja\1.12.1",
    "$toolsRoot\cmake\3.30.2\bin",
    "$toolsRoot\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    "$toolsRoot\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin",
    "$toolsRoot\openocd-esp32\v0.12.0-esp32-20260424\openocd-esp32\bin",
    $env:PATH
) -join ';'

# ESP-IDF VS Code extension invokes EIM activation scripts with `-e` and
# parses KEY=VALUE lines from stdout to construct its own environment.
if ($args -contains '-e') {
    "IDF_PATH=$env:IDF_PATH"
    "IDF_TOOLS_PATH=$env:IDF_TOOLS_PATH"
    "IDF_PYTHON_ENV_PATH=$env:IDF_PYTHON_ENV_PATH"
    "ESP_ROM_ELF_DIR=$env:ESP_ROM_ELF_DIR"
    "OPENOCD_SCRIPTS=$env:OPENOCD_SCRIPTS"
    "PATH=$env:PATH"
}
