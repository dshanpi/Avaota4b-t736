# T736 LPDDR4 源码实现进度

这里是尚未完成的 SPL 组件，不是可启动镜像。没有 `init_DRAM()` 成功占位函数，没有链接厂家 DDR 库，没有包含 PMU/PIE 指令数组。

2026-11-14 时钟续测已上板：新增 3 次 SRAM 执行，共 387 次寄存器访问、25 次配置写入。`UcclkHclkEnables` 的 `3→7→3` 读回通过；PHY 配置后的 `DRAM0_CLK` 更新脉冲也通过，等待实测 24011 ticks（24 MHz，约 1.00046 ms）。随后重跑同一 ZCal 对照，原始候选 `0x20314/0x20315` 仍为 `ff/ff`，覆盖寄存器 `0x20326/0x20327` 仍为 `0/0`。两个时钟干预均未改变反馈，校准仍未验证。全部修改已按成功路径读回恢复，控制器仍在 Init，未发 DRAM 命令。证据见 `../../evidence/t736-zcal-clock-followup-summary-20261114.json`；本轮两个新探针的 ASan/UBSan 故障测试及 19 项主机测试均先于上板通过。

2026-11-14 最新实板续查：冷态前置配置和双通道静态配置已重建，新增 8 个 SRAM 诊断共完成 1151 次寄存器访问、57 次配置写入。`0x2030d` 脉冲、Run 高电平期间采样、100 微秒到 10 毫秒等待对比，以及 `0x2006e` 候选命令源选择均完成；`0x20326/0x20327` 仍为 `0/0`。另两个原始候选结果寄存器 `0x20314/0x20315` 在复位前、复位期间、运行期间及恢复后均为 `0xff/0xff`。它们的有效性和 PU/PD 含义尚未独立确认，不能把覆盖寄存器为零直接等同于状态机未运行，也不能把 `ff` 判作校准成功。

本轮没有验证 ZCal 成功，没有执行实际 DRAM 命令、训练、内存内容测试或 eMMC 烧录。板卡保留在 FEL，实验修改的 Rate、伴随脉冲及候选命令源选择已读回恢复。当前缺口是本代 PHY 内部 DFI/ZCal 时钟、复位/使能和完成反馈的准确契约；其他代际的 PLL 位定义不能直接套用。完整实板索引、读数、失败前置检查和测试说明见 `../../evidence/t736-zcal-followup-summary-20261114.json`，相应源码/载荷/主机脚本冻结在 `../../build/phy-zcal-*-probe-tested-20261114/`。下文保留各阶段的历史进展。

`controller.c` 实现了本板 8 GiB 拓扑的 MSTR/地址映射生成、复位初始化状态下的写入与回读检查，以及有超时的 SWCTL 更新握手。仅支持已核对的双通道、每通道双 rank、16-bit、17 行/10 列/3 bank bits 拓扑；其他组合明确拒绝。调用者必须在 SRAM 中执行、停止 DRAM 客户端并满足时钟/复位前置条件。函数成功只表示这些寄存器操作成功，不表示内存已经初始化。

`axp8191.c` 实现 DCDC8、ALDO4、ELDO1 的电压编码与 I²C 寄存器操作。参考 SyterKit 接口并按 AXP8191 手册核对：保留其他位、拒绝不可精确表示的电压、检查器件 ID 和回读、阻止已开启 DCDC8 跨 1.54 V 电压段切换。尚未连接物理 I²C 驱动或在板上改压；没有套用其他板子的电源预设。

已执行的实板工作是 `observe_linux.c` 只读探测。在旧 v11 启动链下，两通道 MSTR 均读到 `0x03081002`，22 个 MSTR/地址映射值一致。该 LPDDR4 配置同样置位 bit 19，不能沿用所查 A733 分支以该位判断 LPDDR5 的写法。地址映射位于单独的 `0x0a130000/0x0a530000` 区域。没有对板上 DDR 寄存器写入。探测模块已卸载、临时文件已删除；内核的外部模块 taint 为 4096，会保留至重启。

`tools/check_lpddr4_components.py` 从串口日志生成测试基准，检查源代码结果与实际两通道读数一致，并覆盖运行中控制器拒绝、写入失败、握手超时、非法电压、保留位及 I²C 失败。测试在 ASan/UBSan 下通过，两个组件以 ARM32 freestanding 模式编译成功；产物只放在 `build/lpddr4-components/`。

2026-11-14 新增 `timing.c`：仅支持已核对的 1800 MHz，计算 21 项 timing/DFI/refresh 字段，42 个双通道字段与新的只读实板采样一致；还实现带预检、回读和超时的 DFI 交接函数。没有在板上写入或执行该交接，完整控制器复位、模式寄存器初始化仍需实现。

`pmu/` 新增 LPDDR4 公共消息字段编辑、邮箱协议及眼窗选择算法，已通过原生测试和 ARM32/ARC EM4 编译。这些组件没有 PHY 训练命令执行后端，不会生成可执行 PMU 固件。具体缺失的命令编码、CBT、level/gate、采样/deskew 和 PIE 资料见 `pmu/backend-audit.json`。

相邻 `../early/` 提供仅在 SRAM 运行的诊断探针（启动、UART、计时器、R-TWI）；`../storage/` 提供有界的 eMMC 只读 PIO 协议核心。early 的源码 SRAM 诊断已经上机通过，storage 核心尚未上机；两者均未组成完整的独立启动 SPL。正常启动仍缺 PHY/PIE 配置、LPDDR4 ARC PMU 训练、完整板级时钟/供电与存储/ARM64 交接。SyterKit 的现有 LPDDR4 入口仍调用外部库，不能填补这部分源码。不得将上述组件测试当作独立冷启动或烧录验收。

来源、实测边界和状态见 `implementation.json`；构建结果见 `evidence/lpddr4-components-check.json`，实板原始记录见 `evidence/lpddr4-register-observation.log` 与 `evidence/lpddr4-addressmap-observation.log`。

本轮继续只做 LPDDR4。PIE 指 PHY Initialization Engine（DDR PHY 初始化引擎），与 PCIe 无关。`phy/` 已新增 PLL、PMU reset/stall、host mux、DQ selector 校验及 Pstate 配置组件；完整模拟和编译结果不能代替冷态初始化实测。运行中的板子只读到 MuxSel=1 后即停止，没有抢占 PHY 或读取内部 CSR。

`pmu/commands.c` 已实现并独立核对 LPDDR4 MRW/MRR/MPC 的 CA/CS 真值表。命令编码与真正驱动 PHY 是两件事：Type9 的 SDR slot/CS 展开、完成状态及接收通路仍未闭合，因此尚未生成训练固件或 PIE 程序。

`mode_register.c` 已通过 `observe_mrr_linux.c` 在实板执行：两通道、两 rank、MR5/MR6/MR8 正反顺序共 24 次读取均成功，结果一致为 `ff/07/18`。该诊断在已初始化 DRAM 上发出控制器 MRR 请求，写入仅限 MRCTRL0/1；没有 MRW、电压或 PHY 参数修改。临时模块已卸载，系统无失败服务。记录为 `evidence/lpddr4-mrr-hardware-20261114.log`；它验证了 MRR 组件，不能算新 PHY 训练成功。

通过现有 Windows usbUDisc 驱动的固定用途 FEL transport，168 字节无栈源码探针已完成上传、读回、执行及报告校验。本板 BootROM raw ID 为 `0x190300`，入口 `0x48c00`，MMU、I-cache、D-cache 均关闭。记录为 `evidence/t736-cp15-sram-hardware-20261114.json`。此结果只验证这个小窗口的执行；当时尚未验证相邻 `early/` 的完整探针；后续 v2 的实际结果见下文。未烧录新镜像，正常启动仍不是用户要求的独立源码链。

随后核对了本板此前 FES 的实际连续装载范围，并完成更大 SRAM 探针测试。v1 在 `CNTFRQ=0` 处明确返回失败；v2 按手册补齐时间戳频率记录，保留正在运行的计数器、不写 CNTFRQ，并实际通过 UART、SRAM 栈、计数推进、延时和 AXP8191 读取。六个电源寄存器解码出的当前配置为 DCDC8=1100 mV、ALDO4=1800 mV、ELDO1=650 mV，均开启；这是寄存器设定值，不是电压测量。源码未写 PMIC 电压。新结果见 `evidence/t736-early-v2-sram-hardware-20261114.json` 与串口同名 `.log`（含 `-serial-`）。

源码 DDR CCU 冷态启动已在 SRAM 实测通过：PLL 锁定、输出和 DRAM0 时钟/复位读回一致，NSI/MBUS 三个寄存器保持原值。随后公共域三写实验也通过，SYS_CFG/CHANNEL_TYPE/PRCM250 从 `3200/0/0` 变为 `2728/2/1`，写前、写后各 10 ms 延时均由计数器验证，程序正常返回 FEL。证据分别为 `evidence/t736-ccu-cold-sram-hardware-20261114.json` 与 `evidence/t736-domain-setup-sram-hardware-20261114.json`。这些结果尚未证明控制器或 PHY 可访问，更不代表 LPDDR4 训练完成；下一步为公共端口只读及控制器访问前置验证。
