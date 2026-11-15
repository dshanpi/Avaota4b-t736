# T736 源码导航

整理日期：2026-11-15（Asia/Shanghai）。当前实板版本为 **source-v6 raw + v4 SPL + Linux hdmi-v13**。这三个版本号对应不同组件；历史 `raw-v6` 也不等于当前 `source-v6`。

先读 [当前状态](../README.md)、[构建说明](BUILD.md) 和 [source-v6 验收](../evidence/source-v6-emmc-acceptance-20261114.json)。本次备份范围与验证边界见 [备份记录](BACKUP_SOURCE_V6.md)。

## 当前启动与显示实现

| 功能 | 源码与配置 | 构建或核对入口 |
|---|---|---|
| 固定 8 GiB LPDDR4x 参数 | `config/t736-lp4x-fixed.fex`、`spl/vendor_ddr/probe.c` | `tools/build_t736_emmc_spl.py --variant v4` |
| SRAM 入口、UART、定时器、PMIC 总线 | `spl/vendor_ddr/entry_boot.S`、`spl/early/`、`spl/lpddr4/axp8191.*` | 同上；DDR 类型/几何/容量固定，保留 PHY 训练 |
| eMMC SPL、载荷校验、恢复入口 | `spl/vendor_ddr/emmc_boot.c`、`spl/vendor_ddr/boot.ld`、`spl/storage/` | 同上；打包器核对已验收 SPL SHA256 |
| 外围电源 | `spl/peripheral/board_power.*` | v4 SPL 编译选项启用；保持 DDR 电源位不变 |
| ARM32/ARM64 交接与高地址内存抽测 | `spl/vendor_ddr/handoff32.S`、`stage64.S`、`stage64.c` | `tools/build_t736_source_handoff.py` |
| TF-A | `patches/tf-a/0001-t736-bl31-dram-layout.patch` | `tools/build_tfa_independent_candidate.sh`；固定社区提交与归档哈希 |
| 当前 U-Boot | `patches/u-boot/t736-source-boot.patch`、`config/u-boot-source-v6.config` | `tools/build_source_uboot.sh`；完整差异重放已核对，现有产物与已部署 autoboot 哈希一致；新脚本只构建独立候选 |
| Linux 基础板级与 Armbian 集成 | `boards/`、`userpatches/kernel/`、`userpatches/config/` | [BUILD.md](BUILD.md)、`config/sources.lock.json` |
| HDMI 显示时钟、电源域、设备树 | `boards/clk-t736-display.c`、`t736-display-power.c`、`t736-hdmi.dtsi` | `tools/build_display_kernel.sh 13` |
| DRM 移植及 fbcon 修复 | `patches/t736-display-linux618-compat.patch` | `tools/prepare_t736_display_port.py` 首次导入；`tools/export_display_compat.py` 导出并重放检查 |
| 已验收内核配置 | `userpatches/linux-t736-hdmi-v13.config` | Linux `6.18.63-t736-hdmi-v13`，Debian 包版本 `6.18.63-13` |
| rootfs 更新 | `tools/build_source_rootfs_v6.sh` | 从独立 v1 rootfs 副本安装 v13 内核，保留旧内核恢复文件 |
| 完整 raw 组装 | `tools/pack_t736_source_boot.py --variant v6` | 使用 v4 SPL 和源码交接、TF-A、U-Boot；校验载荷哈希、分区布局与文件系统 |

显示修复的关键点：以主线 DMA framebuffer helpers 提供绘图和读写回调，替代不存在的 `CONFIG_AW_FB_CONSOLE` 开关；初次 modeset 完成并释放锁后才注册 framebuffer，避免控制台接管时的空指针调用和锁重入。

## 重建入口与外部输入

在已有本板基础工作树、已审核补丁及外部输入的 Linux 工程中，当前显示镜像入口为：

```sh
cd /home/ubuntu/t736-mainline
mkdir -p build/linux-hdmi-v13
cp userpatches/linux-t736-hdmi-v13.config build/linux-hdmi-v13/.config
sh tools/build_display_kernel.sh 13
sudo -n unshare --mount sh tools/build_source_rootfs_v6.sh
python3 tools/pack_t736_source_boot.py --variant v6
```

`prepare_t736_display_port.py` 仅在首次导入显示源码时运行，已有移植目录会拒绝覆盖。Tina SDK 作为只读输入，编译、rootfs 和镜像均留在独立工程。构建时间、工具链与包依赖变化可能改变输出哈希；打包器遇到与锁定产物不符时会停止，不应直接删除检查。

这些入口复现已有工程的构建步骤，尚不是从公开仓库一条命令取得全部输入的流程：需要官方上游 checkout、Tina GPL 显示源文件、获授权的固定 DDR 库、TF-A 固定归档和 v1 Armbian rootfs。当前 U-Boot 的完整源码差异与配置已归档；新构建入口尚未重新编译，构建时间会影响二进制哈希，打包器仍锁定已验收 autoboot 产物。精确依赖以构建脚本和 [启动策略](../config/boot-policy.json) 为准。厂家 DDR 库及内嵌训练程序获准用于本板启动，但不随源码备份发布；厂家 FES loader 只在 RAM 中承担烧录传输。

## 实验与历史文件

| 路径 | 用途与边界 |
|---|---|
| `spl/lpddr4/phy/`、`spl/lpddr4/pmu/`、控制器时序组件 | 纯源码 LPDDR4 研究；部分组件和 SRAM 实验通过，不构成完整独立训练实现 |
| `spl/*_probe/`、对应 `tools/build_t736_*_probe.py` | SRAM 单变量实验；保留复现与失败证据，不能替代当前 v4 SPL |
| `spl/vendor_ddr/probe.c` | 同时由 SRAM DDR 实验与当前 SPL 使用，不能仅因文件名含 probe 就归入废弃代码 |
| `handoff/`、早期 `tools/pack_handoff.py` | 厂家旧启动链阶段的历史交接候选；当前交接位于 `spl/vendor_ddr/` |
| `tools/build_raw_rootfs_v6.sh` 至 `v11.sh`、`pack_armbian_raw_v6.py` | 历史运行时镜像；当前打包器仍复用其中 GPT/布局函数，不能整批删除 |
| `tools/install_*` | 早期运行时安装实验；当前持久固件更新使用完整 raw/FES |
| `tests/`、模块内 `test_*.c` | 主机侧工具、边界和组件检查；编译/测试通过不等于上板通过 |
| `tools/validation/` | 从已验证分析脚本整理的验收工具；显示采集和 GPT 离线核验可复用，板上 GPT 迁移有写操作、仅手动执行 |
| `evidence/` | 实板、构建和故障记录；以 source-v6 最终验收为当前状态，保留失败历史 |

当前仍未解决纯源码 ZCal 的 PU/PD 问题。授权 DDR 库实现的成功启动不能记作纯源码训练成功。

## 验收索引

- [验收工具说明](../tools/validation/README.md)：运行时与重启检查、只读 framebuffer 抓取、可移植 GPT 日志核验及写盘边界。
- [最终验收](../evidence/source-v6-emmc-acceptance-20261114.json)：三次无人干预启动、root 登录、内存与存储检查。
- [HDMI 状态](../evidence/source-v6-display-state-20261114.log)：PHY 锁定、active CRTC、帧中断与错误计数。
- [帧缓冲抓图](../evidence/source-v6-hdmi-framebuffer-20261114.png)：Armbian tty1 登录文字；不是实体显示器照片。
- [GPT 核验](../evidence/source-v6-gpt-verification.json)：主备 CRC、镜像副本、分区定义一致。
- [v13 构建记录](../evidence/display-kernel-v13-build-record-20261114.json)：内核、包、设备树与兼容补丁哈希。
- [v5 故障日志](../evidence/source-v5-late-fbcon-oops-20261114.log)：修复前 `soft_cursor` 空指针的证据。
