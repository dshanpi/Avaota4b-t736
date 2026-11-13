# T736 主线 Armbian 移植

本仓库保存 Avaota4b/T736 的主线移植代码与实板验证记录。当前已完成基础启动，外设仍在逐项适配；不是完整硬件支持的发行固件。

## 当前版本与实测结果（2026-11-13）

| 组件 | 版本与结果 |
|---|---|
| U-Boot | 官方 v2026.10 + 独立板级实现；ARM64、UART、重定位、8 GiB 内存描述、eMMC/ext4 读取已运行 |
| Linux | 官方 v6.18.63 + 独立板级代码；8 CPU、串口、eMMC PIO、ext4 根系统已运行 |
| Armbian | 26.11.0-trunk.128，Debian 13 Trixie minimal；已完成无输入冷启动及 root shell 登录 |
| v3 运行时修复 | 内核包 6.18.63-3；真实 tmpfs、IPv4 组播依赖和 initramfs fsck 已实板验证；systemd 失败单元为 0 |
| USB1 首版 | 时钟/复位、R-PIO PL6 VBUS、USB2 PHY、主线 EHCI/OHCI 和低地址 DMA 配置已编译成 6.18.63-4；已无输入启动；控制器、480 Mbps 四口 Hub 和 Realtek 读卡器枚举通过；读卡器为空，存储 I/O 待验证 |

eMMC 当前是 **12 MHz、8-bit PIO**；没有高速/DMA 或全面压力测试结论。8 GiB 内存完成识别和高地址抽测，没有完整稳定性认证。
PCIe、网卡、Wi-Fi、蓝牙、HDMI 尚未验证支持。USB 控制器探测、真实设备枚举与 I/O 必须分别记录，不能用编译成功代替外设可用。

## 启动链及依赖

厂家 Boot0 → 厂家 BL31/OP-TEE/SCP → 厂家 U-Boot 2018.07 → 新 U-Boot 2026.10 → Linux → Armbian。

目前冷启动已不需要手工输入引导命令，但旧运行时 U-Boot 仍在链中。独立 ARM32 交接候选位于 `handoff/`，尚未实板验证。
DDR、PMIC、UART、部分时钟和引脚初始化仍依赖厂家固件。USB1 使用独立的局部时钟/复位控制，不能称为完整 CCU/R-CCU 适配。

## 根系统恢复记录

v3 调试时曾误选分区布局不同的参考包，FES 更新 GPT 后原根系统不可挂载。
用户确认只有新设置的账号密码并授权恢复。正确布局的恢复镜像已通过 LYNX 写入和校验，根分区恢复为起点 **368640**、长度 **15239680** 个扇区。
恢复后重新保存了厂家 U-Boot 的启动命令；再次硬件断电上电，无输入到达 Armbian 登录。
root 密码已恢复为用户指定值，项目与备份不记录密码。

打包工具现在强制检查已实测 GPT、厂家 MBR 哈希及全部分区尺寸；不能把 `partition` 烧录模式理解为必然保留其他分区。
后续内核调试优先在运行系统中校验和安装软件包，保留上一版启动文件。
事故与恢复证据见 `evidence/runtime-v3-layout-incident.json`、`evidence/runtime-v3-recovered-validation.log`。

## 代码、构建和证据

- `boards/`：Linux 板级设备树、eMMC PIO、USB1 时钟/PHY 与 R-PIO。
- `userpatches/`：官方 Armbian board/family、内核配置、Linux 和 U-Boot 完整补丁。
- `tools/`：独立构建、布局约束、验证包生成及内核配置检查。
- `config/sources.lock.json`、`config/patch-ledger.json`：官方来源、补丁和厂家依赖。
- `docs/BUILD.md`、`docs/VALIDATION.md`、`docs/USB1.md`：构建、验证边界与 USB1 接入说明。
- `status.json`：当前实板状态及未完成事项。

构建机工程为 `/home/ubuntu/t736-mainline`。Tina SDK `/home/ubuntu/t736-aiot/tina` 只读参考，不在其目录构建或改代码。
Windows 文件名大小写冲突，因此 Linux 使用远端独立官方源码副本构建。
不导入瑞莎仓库、DTS、补丁、镜像、固件或衍生实现。厂家二进制和系统镜像不随源码备份发布。

原理图接线、装配选项及主 PIO 候选修正见 `docs/HARDWARE_MAP.md`；主 PIO 修正仅完成宿主验证，当前实板仍是内核包 6.18.63-4。
