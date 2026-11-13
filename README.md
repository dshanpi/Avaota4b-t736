# Avaota4b / T736 主线 Armbian 移植备份

2026-11-13 源码备份。主线 Linux 已从 eMMC 启动 Armbian，并到达串口登录界面。
本仓库保存本板适配代码、补丁、配置、构建脚本和验证证据。当前是开发验证版本，外设适配仍未完成。

| 组件 | 官方基线 | 状态 |
|---|---|---|
| U-Boot | v2026.10 | ARM64、UART、内存、eMMC 读取、ext4 加载已上板 |
| Linux | v6.18.63 | 8 CPU、8 GiB 内存描述、UART、eMMC PIO、ext4 已上板 |
| Armbian | 锁定官方框架提交；26.11 / Debian 13 Trixie minimal | systemd 到达 multi-user.target 和 ttyS0 登录 |

当前仍保留厂家 Boot0、BL31、SCP、OP-TEE 和前级 U-Boot 2018.07。
独立交接程序已编译，尚未上板或安装到持久 Boot1。
当前 U-Boot 补丁包含一个后续修复：自动设置压缩内核解压地址。该修复已编译，尚未安装到板端。
已验证 v2 的差异和手工启动步骤见 [验证记录](docs/VALIDATION.md)。

| 功能 | 当前支持程度 |
|---|---|
| UART0 | 串口日志和登录界面可用 |
| CPU / RAM | 8 CPU 上线；8 GiB 内存描述；高地址抽样读写通过，未做完整压力测试 |
| eMMC | 12 MHz、8-bit PIO；GPT、文件读取、64 KiB 写入回读、根分区挂载通过 |
| PCIe / NVMe | 未适配 |
| 有线网卡 / PCIe 网卡 | 未适配 |
| Wi-Fi / 蓝牙 | 未适配 |
| HDMI / 显示 | 未适配 |
| USB Host / OTG | 未适配 |
| 音频、GPU、NPU、摄像头 | 未适配 |
| 板级 CCU / pinctrl / PMIC | 尚依赖厂家部分初始化，独立适配未完成 |

## 目录

- `boards/`：独立 Linux 设备树和 SMHC3 PIO 驱动。
- `userpatches/u-boot/`：完整 U-Boot 板级补丁，含源文件、defconfig 和接入修改。
- `userpatches/kernel/`：完整 Linux 板级补丁，含 DTS、驱动、Kconfig 和 Makefile。
- `userpatches/config/`：Armbian board/family 配置。
- `userpatches/linux-t736-mainline-current.config`：实测内核配置。
- `handoff/`：未实板验证的 AArch32 → AArch64 交接候选。
- `initramfs/`、`tools/`、`tests/`：验证环境、构建/打包工具和宿主测试。
- `config/`：官方源码锁定、来源登记、硬件状态与外部输入哈希。
- `evidence/`：选定的实板日志、烧录结果和镜像清单。

## 构建与验证

参见 [构建说明](docs/BUILD.md)、[启动链与固件依赖](docs/BOOT_CHAIN.md)、[验证记录](docs/VALIDATION.md)。

宿主工具检查：

```sh
python3 tools/prepare_sources.py --check
python3 -m unittest discover -s tests -v
```

上游源码通过锁定提交重新取得，不在本仓库重复保存。
厂家二进制、完整镜像、rootfs、工具链、SSH 凭据和 LYNX 会话信息不随源码上传。
`config/external-inputs.json` 与 `config/reference-components.json` 记录外部输入。

原有源码许可证和 SPDX 声明保持不变，见 [许可证说明](LICENSES/README.md)。
