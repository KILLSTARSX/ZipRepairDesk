# 伪装压缩包修复器 · Zip Repair Desk

把“MP4 视频”拖进 7-Zip 却提示“不是压缩包”？这类文件是视频外壳拼接了压缩包数据：
播放器只看前面的 MP4 盒子，解压工具却从文件头找签名，两边都对不上。

这个工具按 MP4 容器结构找出视频真正结束的位置，从断点处切出后面的压缩包，
写成能正常打开的 ZIP / RAR / 7Z，源文件始终不动。

![界面预览](preview.png)

## 能做什么

| 格式 | 处理 |
| --- | --- |
| ZIP / ZIP64（含 WinZip AES 加密） | 识别 + 切片修复 |
| 7Z | 识别 + 校验通过后切片修复 |
| RAR5 | 识别 + 校验通过后切片修复 |
| RAR4 | 只识别（无结构校验，不自动改写） |
| CAB / ISO / GZIP / BZIP2 / XZ / ZSTD / TAR / ACE / ARJ / LZH | 只识别 |
| MP4 / MKV / AVI / FLV / ASF / MPEG | 识别为视频，提示“并非压缩包” |

- 批量拖放、文件夹递归导入，后台队列处理，可取消。
- 输出到源文件同目录或统一目录，同名自动加后缀，绝不覆盖。
- 可选清理压缩包末尾的多余数据（拼接残留）。
- 浅色 / 深色主题，中文 / 英文界面；首次启动跟随系统语言与系统主题。
- 按钮、主题与语言切换、开关都带轻量过渡动画；关于页链接悬停时才显示下划线。
- 原生 Win32 + C++17，单文件约 230 KB，无运行时依赖。

## 使用

1. 双击 `ZipRepairDesk.exe`（不需要管理员权限）。
2. 把文件或文件夹拖进窗口，或点“添加文件”。
3. 确认状态列显示“待处理”，点“开始批量转换”。
4. 转换完成后点“打开输出位置”。

视频文件、无损 ZIP、只识别格式会标成“视频文件 / 无需转换 / 暂不支持”，不会生成多余副本。

## 构建

需要 MinGW-w64 工具链（`g++`、`windres`、`objcopy`），例如 [w64devkit](https://github.com/skeeto/w64devkit)。
PowerShell 5 及以上：

```powershell
# 工具链在 PATH，或设置 W64DEVKIT，或显式传入
.\build.ps1 -Toolchain C:\path\to\w64devkit
```

产物在 `dist\ZipRepairDesk.exe`。加 `-WithCli` 会额外构建命令行版。
构建使用 `-Oz`、段回收与静态链接，资源节通过 `objcopy` 锚定，避免被 `--gc-sections` 回收。

## 代码签名与证书

发布包用 KILL-STAR SX 代码签名证书签名（SHA256），证书体系与 Jpg-Raw-Pair-Cleaner 相同：

- 根证书：`certs\KILL-STAR SX Root CA.cer`，装到当前用户的“受信任的根证书颁发机构”；
- 叶证书：CN=KILL-STAR SX，由根证书签发，装到“受信任的发布者”（开发机才需要）；
- 同一套证书可以签所有 KILL-STAR SX 软件，换软件不用换证书；装一次根证书即可。

安装步骤见 [certs/README.txt](certs/README.txt)。构建后用本机证书重新签名：

```powershell
.\sign.ps1 -Thumbprint <证书指纹>
```

脚本使用 SHA256 并校验 `Status -eq "Valid"`。每次重新构建都要重新签名。签名只减少“未知发布者”和 SmartScreen 提示，不能保证杀毒软件不误报。

## 测试

```
python tests\run_tests.py     # ZIP / ZIP64 布局、批量、尾部清理、唯一命名、CRC、源文件不变
python tests\test_formats.py  # MP4+7Z、MP4+RAR5 切片、RAR4 只识别、视频判定
python tests\gui_smoke.py     # 真实窗口点按钮跑一次转换，校验输出 CRC
```

## 许可证

[Apache License 2.0](LICENSE)。Copyright © 2026 KILL-STAR SX.
