# STM32 release artifacts

此目录只存放由 `MDK-ARM/build.ps1` 发布的最终产物。直接在 Keil 中构建时，最新文件首先生成在
`MDK-ARM/RBLink_DAP_V3.0/`；未执行发布脚本前不要烧录本目录中的旧文件，应以源码声明的固件版本和生成时间共同核对。

- `RBLink_DAP_V3_0.hex`: program at `0x08000000` with no bootloader offset.
- `RBLink_DAP_V3_0.axf`: symbol-bearing image for Keil debugging.
- `build.log`: compiler, size and zero-error/zero-warning proof for this image.
