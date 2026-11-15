# 启动链与外部固件

## 2026-11-14 当前路线

用户现已授权固定厂家 DDR 库及内嵌 PMU/PIE；其他厂家启动级仍排除，见 `config/boot-policy.json`。

```text
源码 eGON SPL（0x47000）+ 授权 LPDDR4 库
  → 源码 eMMC 读取及逐载荷 CRC 校验
  → 源码 AArch32/AArch64 交接与高地址内存测试（0x47000000）
  → 源码 TF-A（0x48000000）
  → 源码 U-Boot（0x52000000）
  → Linux 6.18.63 / Armbian 26.11
```

当前 `source-v6` 已完成 8 GiB raw 烧录及工具校验，板上 SPL 读回哈希一致，三次受控 10 秒继电器断电启动均无输入进入 Armbian 并 root 登录。固定 LPDDR4x 参数为 `para1=0x311a`、`para2=0xa0001001`、`tpr13=0x6063`；跳过类型、几何和容量自动探测，保留 PHY 训练及启动抽测。受控电源结果没有物理电压测量。

v3 远程恢复也已验证：专用 `lynx_allwinner_enter_fel` 识别标准 `=>` 提示符，发送固定 efex 后进入本芯片 ROM `0x20`，绑定 USB FEL 枚举成功。没有复制厂家 Boot0 代码。v2 自定义提示符曾导致工具未发送 efex，属于已修复的历史问题。

v6 沿用已通过软件 FEL 恢复的 v4 SPL，更新内核为 `6.18.63-t736-hdmi-v13`。256 MiB 四模式内存校验、16 MiB fsync/O_DIRECT 及跨两次重启保持、GPT 尾部迁移及主备 CRC 检查通过，分区定义不变。HDMI 自动接管 fbcon，PHY 锁定、2560×1440 约 59.93 Hz、帧中断递增、错误计数 0；framebuffer 抓图显示 tty1 登录。实体屏幕尚未目视确认。网卡 DHCP/网关 ping 通过，但仍降速到 100 Mbps。完整证据见 `evidence/source-v6-emmc-acceptance-20261114.json`。

## 2026-11-13 历史调查与旧镜像

以下关于纯源码限制、未链接 DDR 库或 TF-A 尚未实板运行的描述是旧阶段记录，已经被上述新授权和实验结果更新。

历史 v6–v11 验证链路：

```text
厂家 Boot0：DDR 与早期存储初始化
  → 厂家 BL31 / OP-TEE / SCP
  → 厂家 U-Boot 2018.07（32 位）
  → boot 分区 Android 容器中的 U-Boot 2026.10（64 位）
  → eMMC ext4 中的 Linux 6.18.63 + DTB + initrd
  → Armbian Trixie
```

新 U-Boot 的装载地址为 `0x52000000`。
已观察到的 BL31 地址为 `0x48000000`，OP-TEE 地址为 `0x48600000`。
主线 DTS 保留了当前固件占用区域；保留范围不能直接推广到其他 PCB 或固件。

2026-11-13 独立链调查：实板为 8 GiB LPDDR4、AXP8191。所查 InfernoEmbedded 的 A733 SPL 实现只支持 LPDDR5，不能套用其训练固件或电源预设。维护者 2026-11-11 的说明也确认 LPDDR4 尚缺。用户已明确禁止正常启动使用任何 Tina 厂家二进制，包括 DDR 库和 DDR PMU 固件；不得改为数组、封装或改名后装入。执行边界记录在 `config/boot-policy.json`。尚未链接厂家 DDR 库，也未生成可用于本板的独立 SPL。

TF-A 候选固定到社区分支提交 `7d322909068fc4bb4ee35e9e43c78ed68d98619b`，不是已经合入官方主线的 A733 支持。`tools/build_tfa_independent_candidate.sh` 使用独立构建目录；本地补丁将 BL31 放在 `0x48000000`、预留到 `0x48200000`，BL33 为 `0x52000000`。已编译、核对 ELF 入口/段范围和原生 PSCI 符号；没有链接厂家 OP-TEE/SCP。尚未实板启动，不能替代 SPL/DDR 验证或算作完整镜像。

只读介质核对：eMMC `PARTITION_CONFIG=0x00`，两个 4 MiB 硬件 boot 分区的完整 SHA256 都等于全零文件。当前厂家 Boot0 来自 user area；没有暗藏的已启用硬件 boot 分区需要沿用。证据为 `evidence/independent-boot-selection.log`。

LYNX 当前 RAM 启动工具的 SPL 限定为 R528/T113-S3，不能当作已支持 T736 的入口；没有伪装芯片或绕过工具限制。FES/raw 路径保持原地址合同。

后续源码核查固定到 InfernoEmbedded/U-Boot 的 `upstream-v2-core-bringup` 提交 `2f5e45fa7993ce8a4a59b638e539a06d8454f823`。隔离取出 12 个 PMU 源文件和一份文档，Git blob 哈希全部核对，通过 ARC GCC 13.3.0 从源码编译出 53,232 字节的 PMU 试验产物；没有使用 `PMU_FW` 或缺失固件占位选项。`tools/build_open_pmu_probe.py` 明确拒绝这两类输入，输出只放在 `build/pmu-source-probe/`，不作为可烧录产物。

这仍不是 LPDDR4 支持：A733 Kconfig、DMC 模式设置、MR8 容量解码和 PHY/PIE 表都按 LPDDR5 配置。名为 `pmu_is_lpddr4()` 的函数仅检查消息块的 MR19 配置位，不构成 LPDDR4 冷启动训练实现。T736 手册第 11.1 节只列 DRAMC 特性，没有详细 PHY 训练编程说明。具体证据见 `evidence/lpddr4-source-feasibility.json` 和 `evidence/open-pmu-compile-probe.json`。本轮没有生成或烧录新 raw，现有 v11 仍不符合最终启动要求。

继续核对分支头和其他项目后，`upstream-v2-core-bringup` 仍为上述固定提交；`feature/a733-pmu-refactor` 也使用 LPDDR5 电源、控制器和容量解码。SyterKit 的 `soc/sun60iw2/Makefile` 明确为 LPDDR4 链接 `libdram-lpddr4.a`，不是纯源码训练。hftrx 的 LPDDR4 成功记录直接引用了瑞莎板级参数，因此排除且没有导入。详见 `evidence/lpddr4-source-followup.json`。目前没有可合规烧录的候选，DDR 初始化的缺失不能由 FES 烧录工具解决。

后续已实际编写 `spl/lpddr4/` 控制器布局与 AXP8191 调压组件，22 个布局值以串口实板读数作为测试基准，ARM32 编译和 ASan/UBSan 测试通过。实板 LPDDR4 的 MSTR 为 `0x03081002`，因此所查 A733 分支的 bit 19 类型判断不能沿用。此轮上机仅执行只读寄存器探测，模块已卸载；没有 DDR/PMIC 写入，也没有独立 SPL、训练或新镜像烧录验收。详见 `spl/lpddr4/implementation.json`。

历史 `handoff/` 候选保留本板 1600 字节 spare header 数据，并使用本板厂家 DTB，不属于独立启动实现。
候选调用厂家 SMC `0x8000ff10` 启动 SCP，再通过 `0x8000ff04` 进入 64 位 U-Boot。
候选尚未验证，不能据此直接替换持久 Boot1。

## FES 验证包

`tools/pack_boot_validation.py` 从本板恢复包读取原始 loader、Boot0、Boot1 和分区元数据。
脚本替换 boot，可选加入 rootfs，并更新对应组件校验和。
验证使用 LYNX `partition` 模式、`verify=true`、`postFlashAction=none`。
这些包不是完整主线启动链替换镜像。

`partition` 模式仍可能更新 GPT/厂家 MBR，不能保证其他分区不受影响。
v3 曾因错误参考布局损坏原根系统，现已按用户授权恢复。
打包工具强制校验已实测 GPT/MBR 哈希和全部分区尺寸；参考 Boot0/FES 哈希相同不足以证明布局相同。
早期调试曾在运行系统安装软件包。当前用户要求的持久更新统一通过完整 Armbian raw/FES 完成，并保留上一版启动文件。

FEL/FES 的逻辑地址与 eMMC 物理 LBA 不能混用。
以下为本次实板 GPT，单位为 512 字节扇区：

| 分区 | 起始 LBA | 扇区数 |
|---|---:|---:|
| boot-resource | 73728 | 65536 |
| env | 139264 | 32768 |
| boot | 172032 | 196608 |
| rootfs | 368640 | 15239680 |
| private | 15608320 | 32768 |
| UDISK | 15641088 | 106501599 |

当前 root PARTUUID 为 `a0085546-4166-744a-a353-fca9272b8e48`。
布局和 PARTUUID 均是本板验证输入，不能用于未经核对的其他设备。

USB FES 下载功能由厂家 RAM loader 提供；这不代表主线 Linux 的 USB 驱动已经可用。
历史验证包使用的厂家 DDR、BL31、SCP、OP-TEE、spare header 和 DTB 是旧启动链的依赖，禁止带入新的正常启动镜像。厂家 loader 仅允许用于 RAM 中的 FES 烧录。源码备份不发布厂家二进制；本地历史调试和恢复产物仍保留。
