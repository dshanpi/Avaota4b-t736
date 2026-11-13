# USB1 首轮接入

范围是板上的 USB1 USB2 Host。USB0 OTG/FES、USB3、Type-C 控制、休眠唤醒均不在本轮支持结论中。
主机协议和设备驱动直接使用官方 Linux 6.18.63 EHCI/OHCI、USB storage、HID；板级胶合代码独立编写。

## 接口及寄存器范围

| 资源 | 地址/接口 |
|---|---|
| EHCI1 | 0x04200000，0x400 字节，GIC SPI159 |
| OHCI1 | 0x04200400，0x400 字节，GIC SPI160 |
| USB1 PMU/PHY | 0x04200800，0x20 字节 |
| USB CCU 子范围 | 0x02003300，0x50 字节；完整 CCU 基址为 0x02002000 |
| MSI gate | 0x020025a4 bit0 |
| master gate | 0x020025c0 bit9，RMW 使用厂家寄存器要求的 key 0x010000ff |
| R-PIO | 0x07025000，0x410 字节；PL14、PM6 个引脚 |
| USB1 VBUS | PL6，高有效固定 5V 开关，启动延迟 1ms |

时钟编号 0..5 为 ref、EHCI1、OHCI1、OHCI1 模块时钟、MSI、master；复位编号 0..2 为 PHY1、EHCI1、OHCI1。
这是局部实验性 DT 接口，未向上游提交，后续完整 CCU 驱动可能替换这些编号和 compatible。

USB ref 的 mux 选择 sys24M。EHCI/OHCI gate 和 reset 按厂家芯片寄存器定义接入 Linux clock/reset API。
PHY 的 SIDDQ 为 PMU+0x10 bit3，调谐写入 PMU+0x18。
本板有效 DT 的调谐值 0x188 按硬件字段映射为 0x806；对寄存器只更新对应字段。
EHCI/OHCI 共享同一个 generic PHY，由 PHY 核心维护初始化/供电引用计数。

局部 MMIO 资源不覆盖现有 eMMC 驱动的 CCU 寄存器，也不触碰 USB0 的 VBUS GPIO。
打开 ZONE_DMA/ZONE_DMA32，保证 8 GiB 板上 32-bit DMA 设备具有低于 4 GiB 的可用内存。

## 明确保留的厂家依赖

- R-APB 时钟、PL/PM pad 电压和相应 PMIC 电源仍由前级固件初始化。
- R-PIO 的 apb 使用临时时钟句柄；其频率不表示已测量 R-APB 速率。26MHz hosc/32768Hz losc 用于框架所需的去抖时钟。
- gate-only 总线时钟的名义父频率不是完整时钟树建模；没有实现 DVFS 或 suspend/resume 保证。
- `clk_ignore_unused` 仍保留，不能据此宣称摆脱厂家初始化。

## 参考与验证要求

寄存器事实来自本板有效厂家 DT、只读 Tina SDK 的 `ccu-sun60iw2.c`、`ccu_gate.c`、`pinctrl-sun60iw2-r.c` 和 `sunxi-hci.c/.h`。
GPIO 控制复用官方 `sunxi_pinctrl_dt_table_init`；没有复制厂家驱动实现。

验证必须记录：内核版本/包哈希、DMA zone、PHY/pinctrl/regulator probe、EHCI/OHCI root hub、端口供电、外接设备 ID/速度和 I/O。
仅 root hub 出现不等于外接设备通信已通过。存储写读测试只创建临时文件，不能写整盘或覆盖用户文件。
实板证据与仍缺少的测试列于 `VALIDATION.md` 和 `status.json`。

## 后续主 PIO 候选

`boards/pinctrl-t736.c` 已按本 SoC 的 B–K 引脚计数、IRQ 映射和新版寄存器布局编写并通过单对象编译。
它尚未接入 v4 构建，也没有出现在 v4 DT 中。启用前必须把 eMMC 的 PC 引脚配置从直接 MMIO 移至 pinctrl，避免两个驱动占用同一资源。
有线网卡审计记录见 `evidence/ethernet-reference-audit.json`，其中没有主线联网成功的结论。

`boards/dwmac-t736.c` 同为未启用候选：只通过 6.18.63 的单对象编译，尚缺 GMAC 时钟/复位、DT 集成和全部网卡实测。

## 原理图核对与后续修正

见 `HARDWARE_MAP.md`。主 PIO 原候选错误套用 D1 寄存器布局，已加入独立布局修正补丁及宿主检查；尚未安装到板端。R-PIO 保持原有布局。

USB1 下游已识别空读卡器，不再按硬件故障处理。v4 缺少 FAT 默认字符集模块；官方源码构建的两个模块已临时加载，构建脚本已补配置，完整内核尚未重编。临时模块随重启消失，FAT 挂载/文件读写尚未实测。
