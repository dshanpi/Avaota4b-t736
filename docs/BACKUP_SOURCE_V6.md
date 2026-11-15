# source-v6 源码与实板记录备份

整理日期：2026-11-15（Asia/Shanghai）。硬件实验日期：2026-11-14。本记录整理当前已验收版本及其构建、故障和验证依据；本文件本身不代表远端上传完成，发布提交与远端校验结果由上传记录另行确认。

目标仓库：<https://github.com/dshanpi/Avaota4b-t736>。源码导航见 [CODE_MAP.md](CODE_MAP.md)，当前状态见 [README](../README.md)，逐文件备份哈希以仓库 `backup-manifest.json` 为准。

## 当前版本

| 组件 | 已部署版本 |
|---|---|
| 完整 eMMC 镜像 | `t736-armbian-source-v6.raw.img`，8 GiB |
| SPL | source v4，含独立外围供电初始化 |
| DDR | 固定本板 8 GiB LPDDR4x；授权固定 DDR 库及内嵌训练程序 |
| 后续启动级 | 源码 ARM32/ARM64 交接、TF-A、U-Boot |
| 内核 | `6.18.63-t736-hdmi-v13`，包版本 `6.18.63-13` |
| 用户系统 | Armbian 26.11.0-trunk.128 / Debian Trixie minimal |

raw SHA256：

```text
5e36cd917e6c49c51e5946faecd838394dce0161dd0926c095b848236729d377
```

板上 SPL 读回 SHA256：

```text
a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea
```

## 本次保留的实现与修复

- 固定 LPDDR4x 参数 `para1=0x311a`、`para2=0xa0001001`、`tpr13=0x6063`，关闭类型、几何和容量探测，保留 PHY 训练和启动内存抽测。
- 源码 SPL 完成 eMMC 载荷读取与 CRC 校验，经源码交接、TF-A 和 U-Boot 启动 Armbian；正常启动不含厂家 Boot0、BL31、U-Boot、OP-TEE 或 SCP。
- 补齐当前 U-Boot 的完整板级补丁、实际构建配置与独立重建入口。与固定官方归档比较后，补丁重放逐字节一致；现有构建产物与部署载荷哈希一致。新构建脚本通过语法检查，但本次未重新编译、未更换板卡固件，详见 `evidence/source-uboot-backup-audit-20261115.json`。
- 外围电源初始化恢复 RTL8211F 识别、DHCP 以及 USB hub/读卡器枚举。
- HDMI 移植包含 DE 时钟、电源域、TCON 和 HDMI 设备树资源，以及 Linux 6.18 DRM API 适配。
- 修复 fbcon 接管时的空指针：补齐 DMA framebuffer 回调，并延后到首次 modeset 完成、释放锁后注册 framebuffer。264 个导入源码/头文件重放与编译工作树一致。
- 保留固定来源、补丁、构建脚本、配置、校验清单、原始失败与成功证据，避免把后续成功覆盖成早期实验已经通过。
- 将已验证的运行时、重启、显示与 framebuffer 辅助工具整理到 [tools/validation](../tools/validation/README.md)；GPT 离线核验器改用必填 `--raw-image` 和可选 `--evidence-dir`，不再依赖 Windows 应用目录。以已有 raw 和串口证据重跑通过，未重新操作板卡；GPT 迁移脚本仍有写盘操作、仅手动执行。

## 实板结果与边界

| 项目 | 实测结果 |
|---|---|
| 烧录 | 8 GiB 完整 raw/FES 写入与工具校验通过；SPL 板上读回哈希一致 |
| 启动 | 三次受控 10 秒继电器断电后，无输入进入 Armbian 并 root 登录；两次重启前均确认正常关机 |
| DDR | 每次识别 8192 MiB；低地址 524288 words 与含 >4 GiB、末尾 64 KiB 的 262144 次检查通过 |
| Linux 内存 | 256 MiB 已分配内存、四种模式校验通过；不等于完整 8 GiB 压力认证 |
| 存储 | 16 MiB fsync/O_DIRECT 哈希一致，两次重启后文件哈希保持 |
| GPT | 备份迁至物理 eMMC 尾部；主备头/分区条目 CRC 及兼容副本通过；六个分区与 PARTUUID 不变 |
| HDMI | connected/enabled、PHY PLL 锁定、2560×1440 约 59.93 Hz；CRTC active、帧中断递增、显示错误 0 |
| 登录画面 | fbcon 自动接管，framebuffer 抓图显示 Armbian tty1 的 `t736 login:`；实体屏幕尚未目视确认 |
| 系统 | 8 CPU、Linux 可见内存 7747 MiB、失败服务 0、内核 taint 0 |
| 网络 | RTL8211F DHCP、网关 ping 3/3；链路降到 100 Mbps/full，千兆未通过 |

电源测试没有物理电压测量，不能据此排除 USB 反向供电。eMMC 仍为 12 MHz、8-bit PIO。纯源码 ZCal、千兆链路、完整内存/存储压力认证及 PCIe、Wi-Fi、蓝牙尚未完成；图形桌面、HDMI 音频、CEC、HDCP、热插拔与引导级画面也未验收。

专用软件 FEL 在前序版本已验证，v6 沿用相同 v4 SPL；本次 v6 最终运行验收没有再次进入 FEL，不能写成 v6 专门重测通过。

## 公开备份范围

公开备份保存源码、配置、补丁、构建说明及经过检查的实验记录。完整 raw、rootfs、内核包、厂家 DDR/FES 二进制、SDK、登录凭据和 MCP 会话信息留在工作区或外部凭据存储，不随源码备份上传。允许本板使用固定 DDR 库不等于授予该库再分发许可。

完整镜像仍依赖外部 DDR 库、GPL 显示源文件、固定上游源码和已准备的 rootfs；因此本仓库不能宣称不依赖外部输入的一键完整固件构建。历史 `raw-v6` 至 `v11` 的厂家运行时启动链记录保留供追溯，不作为当前正常启动方案。

主要证据：[最终验收](../evidence/source-v6-emmc-acceptance-20261114.json)、[GPT 验证](../evidence/source-v6-gpt-verification.json)、[显示状态](../evidence/source-v6-display-state-20261114.log)、[帧缓冲抓图](../evidence/source-v6-hdmi-framebuffer-20261114.png)、[内核构建记录](../evidence/display-kernel-v13-build-record-20261114.json)。
