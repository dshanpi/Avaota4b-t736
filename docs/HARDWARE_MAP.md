# 原理图与实板适配核对（2026-11-13）

以下是移植所需的连线核对，不是外设支持清单。原始 PDF、页面图片、全文提取不进入公开备份。
图纸中的说明作为参考资料，不作为操作授权。

## 来源和适用范围

| 来源代号 | 文档 | SHA256 |
|---|---|---|
| CORE | T736_DEMO_CORE_AXP8191_LP4_4X_32X1_V2_0-20260128-原理图.pdf，17 页 | `5e5903620e36cf37408ef8c615e0399f4b89df84e05833def5733f96e5f9f708` |
| SILK | T736_DEMO_CORE_AXP8191_LP4_4X_32X1_V2_0-20250401-丝印.pdf，2 页 | `59497e80ac0b8898985050bdff631c7b445cf7c24f4fc9953aa88b6d537ef273` |
| DB | 同一参考设计目录中的 T736_DEMO_DBOARD_V2_0-原理图.pdf，27 页 | `3425121b5f51512fdfb534110ab9874919be69ce6205539c6357e2525d13cd08` |

页码是 PDF 页序，不是页内标题编号。例如 DB 第 20 页标题为 `25 ETHERNET`。
CORE 的若干标题栏仍写 V1_0，SILK 标注 V2_0；DB 标题栏写 A736_DEMO_DBOARD_V1_0。
图纸包含不同年份的修改和多种 NC/选装电阻。尚未与实板全部丝印、BOM、跳线逐项比对。
连接以完整电路页目视核对，跨页网名和核心板/底板连接器交叉检查；不能只从 PDF 提取文本推断连线。
CORE 第 6 页的概览还出现旧 PMIC/引脚标签，不直接作为电源编程依据；优先核对第 7、10、11、16 页和实板有效 FDT。

## 外设映射

| 模块 | 图纸所示连接 | 与实板的关系及实现边界 |
|---|---|---|
| eMMC | CORE 10、13：PC0/1/5/6/8/9/10/11/13/14/15/16，8-bit 数据，DS、RESET、CLK、CMD；VCC-NAND 与 VCC-PC 分开供电 | 旧 BSP pinmux 实测为这些 PC 引脚的 sdc3，SMHC3 在 0x04023000；当前主线 12 MHz PIO 已启动。不能据图宣称 HS400 已移植 |
| USB1 | CORE 16 + DB 10、19：PL6 经连接器接 USB1-DRVBUS；SY6280 供电开关；USB1 D+/D- 经 RU34/35 接 FE1.1s 四口 Hub | 已实测 PL6 高、Hub 1a40:0101、读卡器 0bda:0316 在 1-1.3，480 Mbps。仅证实枚举和设备命令响应 |
| USB-A 路由 | DB 19：Hub port 3 → JU3，port 4 → JU4；port 1 引往第 21 页，port 2 仅画测试点；JU5 的 USB2 D+/D- 来自 USB2 | 读卡器路径与 JU3 路由相符，但尚未目视比对实板连接器位号。不能把 JU5 计入 USB1 已验证端口 |
| USB3 / PCIe | CORE 12、16 + DB 19：COMBO1 → U59 TMUXHS4212 → J58 PCIe 或 JU5 USB3；J64 标注 DEFAULT:PCIE | 同一组高速线经过切换器，不能同时按两条独立链路启用。需核对实板 J64、COMBO1 模式、参考时钟、电源和 PHY；尚未主线枚举 |
| PCIe 边带 | CORE 16 + DB 10、19：PI10 → PERSTn，PI13 → WAKEn，PI14 → CLKREQn；J58 使用 VCC-3V3-PCIE 和 12V | 尚未控制复位或测链路；必须核对选装电阻和电源，不能仅打开 PCI 内核选项 |
| 有线网口 | CORE 10、16 + DB 10、20：RGMII0 PH0-PH15；PH16 → PHYRSTB；PH15 → EPHY-CLK-25M；PHY 标 RTL8211F-CG/RTL8211FD-CG | 厂家有效 FDT 的 PH0-PH15、PH16 复位及历史 PHY ID 001c.c916 与之相符。主线 dwmac 候选仅编译，未探测 PHY 或联网 |
| PHY 电源/时序 | DB 20：默认 MDIO 地址 1、外部 3.3V RGMII，外部 25MHz 路由；TX/RX 延时电阻有装配选项 | 图纸默认 RX 延时上拉、TX 延时下拉；需以实际装配及 PHY 驱动配置复核，不能与 MAC 延时叠加。厂家 tx=13/rx=10 仍只是候选参数 |
| Wi-Fi | CORE 10、11、16 + DB 22：PG0-PG5 为 SDC1，PM4 → WL-REG-ON，PM3 → WL-WAKE-AP；3.3V VCC-WIFI、1.8V VCCIO-WIFI | DB 同页含 AW869C 与 NF3205Q，另有 AP6281s 晶振说明；未确认实际装配型号/固件/驱动。当前未支持 Wi-Fi |
| 蓝牙 | CORE 10、11、16 + DB 22：PG6-PG9 对应 UART1 TX/RX/RTS/CTS；PM0 BT-RESETN，PM1 AP-WAKE-BT，PM2 BT-WAKE-AP | 模组端 RX/TX 名字与 SoC 方向相反。Q45/Q46、电阻和模组选装需复核；不能仅凭 RESETN 网名确定最终使能极性。未做 HCI 通信 |
| HDMI | CORE 12、16 + DB 18：专用 HTX、HSCL/HSDA、HCEC、HHPD 接 JH1；5V 经 QH4/QH5，受 M-DISP-5V-EN 控制 | 该使能来自底板 MCU 网络（跨页到 DB 26），不能虚构一个 SoC GPIO。显示控制器、PHY、时钟、DDC/HPD、MCU 供电协作均待适配 |
| PMIC | CORE 7、11：AXP8191Q，经 PL0/PL1 PMU-SCK/SDA；SWOUT1 供 PH/PHY 等，DLDO5 供 PG/PM/Wi-Fi IO；ALDO3 可供 PC 1.8V | 原理图与旧 BSP 电源记录部分吻合，但大量电源网络有选装路径。当前 PMIC 初始化仍依赖厂家固件；不得一次性套用电压或关闭共享电源 |

## 主 PIO 审计修正

此前 USB1 文档和以太网候选记录把 eMMC 引脚误写成 PI；正确为 PC。
eMMC 驱动使用的 `0x02000180` 地址本来就是正确的 PC_CFG0，不需要改为 `0x02000060`。
来源包括 CORE 10/13、旧 BSP `live-board-20261112/supplement.log`、有效 FDT，以及 T736 寄存器手册 18.14.5。

尚未启用的 `boards/pinctrl-t736.c` 曾误选 `SUNXI_PINCTRL_NEW_REG_LAYOUT`。
该标志是 D1 的 0x30 bank 间隔，适合这里的 R-PIO，不能用于 T736 主 PIO：

| 寄存器 | 主 PIO 相对地址 |
|---|---|
| PB_CFG0 / PC_CFG0 / PH_CFG0 / PK_CFG0 | 0x100 / 0x180 / 0x400 / 0x580 |
| PH_DAT / PH_DRV2 / PH_PUL1 | 0x410 / 0x428 / 0x434 |
| PH_INT_CFG2 / CTL / STA / DEB | 0x448 / 0x450 / 0x454 / 0x458 |
| PK_INT_CFG3 / STA | 0x5cc / 0x5d4 |

新增 `patches/linux/0002-t736-main-pio-layout.patch` 为官方 pinctrl 核心提供可选布局描述，修正主 PIO 候选。
使用独立构建树和 `tools/build_pio_candidate.sh` 验证；该补丁尚未加入默认 Armbian 补丁序列，也未安装到板上。
`tools/check_pio_layout.py` 执行实际 C 地址计算函数，检查手册中的地址、位移及掩码，并覆盖 legacy、D1、R-PIO 回归。

下一步启用主 PIO 前，必须把 eMMC 的 PC 引脚所有权从直接 MMIO 移至 pinctrl，接入总线时钟并维持正确 pad 电压。
然后才能集成 GMAC 时钟/复位与 DTS，逐步验证 MDIO → PHY → Link → DHCP → 双向数据传输。
