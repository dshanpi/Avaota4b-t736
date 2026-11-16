# T736 主线 Armbian 移植

## 当前阶段快照（2026-11-16）

**storage-v23 的 HS200 200 MHz 阶段已通过，持久默认仍保留 storage-v16。** 这次是源码与证据备份检查点，尚不是最终镜像发布。完整索引见 [阶段备份记录](docs/BACKUP_STAGE_20261116.md)，实板依据见 [v23 阶段记录](evidence/storage-v23-hs200-stage-record-20261116.json)。

本板固定 8 GiB LPDDR4x，沿用用户授权的 DDR 库及源码后续启动级。v23 一次性启动到 GNOME，eMMC 为 200 MHz、8-bit、1.8 V HS200；独立命令采样选中 tap15，32 MiB 两套模式的直接读写校验全部通过，GPU 60 帧四象限回读与 PVR 健康检查通过。64 KiB 带校验顺序读为 37.03/36.81 MiB/s，写为 89.10 MiB/s。历史 v22 的 200 MHz 首次失败保留，不改成成功。

HS400 正在独立实现和复核，未验收。GNOME 帧反馈仍在诊断；EGL 小窗口的交换计数与像素回读成功不等于桌面流畅度或实体 HDMI 目视验收。新最终镜像、完整压力测试均未完成。下面的 source-v6 段落是当时的完整镜像验收历史，不表示当前运行内核或桌面状态。

## 历史 source-v6 验收快照（2026-11-14）

源码入口见 [代码导航](docs/CODE_MAP.md)，本次整理与备份范围见 [2026-11-15 备份记录](docs/BACKUP_SOURCE_V6.md)。

**当前部署为 source-v6，启动与 HDMI 控制台修复已通过实板验收。** 8 GiB raw/FES 写入、工具校验和板上 SPL SHA256 核对通过。按本板固定 **8 GiB LPDDR4x** 参数启动，关闭类型、几何和容量自动探测，保留 PHY 训练。三次受控 10 秒继电器断电后均无输入进入 Armbian，并完成 root 登录；没有物理电压测量，不能据此排除 USB 反向供电。

当前启动链：源码 eGON SPL + 用户授权的固定 DDR 库 → 源码 ARM32/ARM64 交接 → 源码 TF-A → 源码 U-Boot → Linux 6.18.63 → Armbian 26.11。正常启动不含厂家 Boot0、BL31、U-Boot、OP-TEE 或 SCP；厂家 FES loader 仅在 RAM 中烧录。纯源码 ZCal 的 PU/PD 问题仍未解决。

实板通过：8192 MiB 初始化、低地址 524288 words 与含 >4 GiB 的 262144 次内存抽测、Linux 256 MiB 四模式校验、16 MiB fsync/O_DIRECT 同哈希及两次重启后保持。8 CPU 上线，Linux 可见内存 7747 MiB，失败服务 0，taint 0。v6 GPT 备份已移至 eMMC 物理尾部，主/备 CRC 与兼容镜像读回验证通过，六个分区定义和 PARTUUID 保持不变；后两次启动无 GPT 告警。

远程恢复已验证：v3 独立 `=>` 提示符被专用工具识别，固定 `efex` 在 DDR 前进入 ROM 0x20，USB FEL 枚举成功；恢复后再次无输入启动成功。v2 的自定义提示符造成工具未发送 efex，已由 v3 修复。

v6 沿用 v4 已验证的独立供电 SPL，启用 DC1SW1 与 DLDO2，全部写入读回正确。RTL8211F 正确识别，DHCP 得到地址，网关 ping 3/3 成功；链路从 1 Gbps 降到 100 Mbps，千兆尚未通过。USB hub 和读卡器恢复枚举。早期供电修复证据见 `evidence/source-v4-peripheral-acceptance-20261114.json`。

HDMI 故障根因是绘图回调被不存在的 `CONFIG_AW_FB_CONSOLE` 开关屏蔽，`soft_cursor` 调用空指针。v6 使用主线 DMA framebuffer helpers，并在首次 modeset 完成、释放锁之后注册 framebuffer。Linux `6.18.63-t736-hdmi-v13` 三次普通启动均自动接管 fbcon；HDMI connected/enabled、PHY PLL 锁定、2560×1440 约 59.93 Hz、TCON 中断持续递增、显示错误为 0。只读抓取的 framebuffer 清楚显示 Armbian tty1 的 `t736 login:`，见 `evidence/source-v6-hdmi-framebuffer-20261114.png`。用户已离场，实体屏幕目视结果待确认；没有把 framebuffer 抓图当作屏幕照片。264 个导入源码/头文件重放一致，没有导入显示可执行固件。

最终验收：`evidence/source-v6-emmc-acceptance-20261114.json`；GPT：`evidence/source-v6-gpt-verification.json`；构建：`evidence/display-kernel-v13-build-record-20261114.json`；镜像清单：`output/t736-armbian-source-v6.raw.manifest.json`。root 凭据保存在工作区外。板卡留在正常 Armbian，HDMI 提供文字登录控制台；图形桌面、HDMI 音频、热插拔等尚未验收。以下为历史记录，不代表当前限制或运行结果。

## 历史 v6–v11 版本与实测结果（2026-11-13）

| 组件 | 版本与结果 |
|---|---|
| U-Boot | 官方 v2026.10 + 独立板级实现；ARM64、UART、重定位、8 GiB 内存描述、eMMC/ext4 读取已运行 |
| Linux | 官方 v6.18.63 + 独立板级代码；8 CPU、串口、eMMC PIO、ext4 根系统已运行 |
| Armbian | 26.11.0-trunk.128，Debian 13 Trixie minimal；当前 v11 已 root 登录，但第一次冷启动中发生无提示复位，不能记作稳定冷启动通过 |
| v3 运行时修复 | 内核包 6.18.63-3；真实 tmpfs、IPv4 组播依赖和 initramfs fsck 已实板验证；systemd 失败单元为 0 |
| USB1 首版 | 时钟/复位、R-PIO PL6 VBUS、USB2 PHY、主线 EHCI/OHCI 和低地址 DMA 配置已编译成 6.18.63-4；已无输入启动；控制器、480 Mbps 四口 Hub 和 Realtek 读卡器枚举通过；读卡器为空，存储 I/O 待验证 |
| 主 PIO / eMMC | 当前内核 6.18.63-11；历史 v5/v6 已验证引脚归属和 16 MiB fsync/O_DIRECT 回读；运行时仍为 12 MHz、8-bit PIO |
| 网卡 v11 | DHCP、1000 Mbps/full、受支持的 stmmac 自检、64 MiB 双向 HTTP/SHA256 已通过，无新增 CRC/对齐/FIFO 错误；仍依赖旧启动链建立父时钟和电源 |
| 独立 TF-A 候选 | 固定社区 A733 分支，BL31 入口 0x48000000、BL33 0x52000000；编译和 ELF 核对通过，没有实板启动结论 |

eMMC 当前是 **12 MHz、8-bit PIO**；没有高速/DMA 或全面压力测试结论。8 GiB 内存完成识别和高地址抽测，没有完整稳定性认证。
PCIe、Wi-Fi、蓝牙、HDMI 尚未验证支持。USB 控制器探测、真实设备枚举与 I/O 必须分别记录，不能用编译成功代替外设可用。

## 历史 v6–v11 启动链及依赖

厂家 Boot0 → 厂家 BL31/OP-TEE/SCP → 厂家 U-Boot 2018.07 → 新 U-Boot 2026.10 → Linux → Armbian。

目前冷启动已不需要手工输入引导命令，但旧运行时 U-Boot 仍在链中。独立 ARM32 交接候选位于 `handoff/`，尚未实板验证。
DDR、PMIC、UART、部分时钟和引脚初始化仍依赖厂家固件。USB1 使用独立的局部时钟/复位控制，不能称为完整 CCU/R-CCU 适配。

移除这条链还缺本板 LPDDR4 的独立 SPL；找到的开源 A733 DDR 实现只支持 LPDDR5。此前的纯源码限制已增加 DDR 库及内嵌训练固件例外；其他厂家启动级仍未放开，见 `config/boot-policy.json`。详见 `docs/BOOT_CHAIN.md` 和 `evidence/independent-boot-audit.json`。旧打包脚本默认拒绝生成含 Tina 运行时组件的 raw，历史调试版本不能重新部署为正常启动方案。

LPDDR5 ARC PMU 已从固定源码编译成功，但只是隔离的编译试验，不支持本板 LPDDR4，也没有烧录。后续核查确认 SyterKit 的 LPDDR4 选项仍链接预编译 DDR 库，另一个 LPDDR4 候选使用了已排除的瑞莎板级参数。所查来源均未解决本板纯源码冷启动；仍需实现 LPDDR4 控制器、PHY 训练和独立 SPL。证据见 `evidence/lpddr4-source-feasibility.json`、`evidence/lpddr4-source-followup.json`。

继续实现后，`spl/lpddr4/` 已加入本板控制器布局与 AXP8191 调压组件。22 个布局值与只读实板探测一致；ASan/UBSan 测试及 ARM32 编译通过。PHY/ARC 训练与完整 SPL 尚未实现，没有生成或烧录新镜像；这些组件不能算独立启动完成。详见 `spl/lpddr4/README.md` 和 `evidence/lpddr4-components-check.json`。

## 根系统恢复记录

v3 调试时曾误选分区布局不同的参考包，FES 更新 GPT 后原根系统不可挂载。
用户确认只有新设置的账号密码并授权恢复。正确布局的恢复镜像已通过 LYNX 写入和校验，根分区恢复为起点 **368640**、长度 **15239680** 个扇区。
恢复后重新保存了厂家 U-Boot 的启动命令；再次硬件断电上电，无输入到达 Armbian 登录。
root 密码已恢复为用户指定值，项目与备份不记录密码。

打包工具现在强制检查已实测 GPT、厂家 MBR 哈希及全部分区尺寸；不能把 `partition` 烧录模式理解为必然保留其他分区。
当前持久系统更新统一使用完整 raw/FES；临时诊断不能替代镜像烧录和冷启动验证。
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
用户已允许复用 Radxa A733 源码和固定 DDR 库，需记录来源与哈希。厂家二进制和系统镜像不随源码备份发布。

原理图接线、装配选项及主 PIO 修正见 `docs/HARDWARE_MAP.md`。v4 启动文件在板上 `/boot/t736-v4-recovery`，已校验；v5 的源码快照与软件包保存在本地 output。

历史 v6–v11 的 8 GiB raw 已通过 FES 写入和校验，但它们都不符合 Tina 仅用于烧录的最终启动要求。OpenixCLI 已抵消 Tina 的 40960 扇区逻辑偏移，不要修改 loader 的偏移。v6 曾完成 GPT 迁移；当前 v11 的 GPT 尚未迁移，不能沿用 v6 的验收结论。详见 `docs/FES_RAW.md`、`docs/ETHERNET.md`。
