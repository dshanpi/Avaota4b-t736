# GMAC0 进展

## 2026-11-16：独立启动链 source-v9 的当前结果

当前内核为 `6.18.63-t736-gpu-v15`，板卡 `end0` 获得 `10.0.0.249/24`，网关 `10.0.0.1`。本端和对端都广告 `1000baseT/Full`，但实际链路为 **100 Mbps/full**；独立启动链日志多次记录 RTL8211F 从千兆降到百兆。因此，旧 v11 的千兆通过记录不能替代当前镜像的验收。

source-v9 已完成板卡主动访问构建机的三轮测试，每轮下载 64 MiB 并回传 64 MiB，总计 384 MiB。六次传输均完成，客户端和服务器 SHA256 一致；客户端实际速率约下载 93.4 Mbps、上传 93.9 Mbps，已接近当前百兆链路上限。测试服务在第三轮后自动退出。基线列出的 CRC、对齐、FIFO、checksum 和总线错误均为 0；完整传输前后计数仍需从客户端 JSON 汇总，不能提前声明所有错误计数没有增加。

当前网页访问是另一项待解决的问题：板卡时钟正确、NTP 已同步，DNS over TLS 能成功解析域名；公网 HTTP 出现空响应，HTTPS 在握手时 EOF。构建机对相同公网目标的 HTTP、默认 TLS、TLS 1.2 均正常。局域网大数据通过不能视为网页已可用；正在对固定目标 IP 做仅记录 TCP 包头的对照。

网络位于 NAT 后。当前构建机看到板卡来源为 `192.168.1.53`，旧记录为 `.50`；两个地址的 ARP MAC 都是 `60:be:b4:01:48:ad`，板侧网关 MAC 为相邻的 `...:ac`。构建机向 `10.0.0.249` 的默认路由经 `192.168.1.1`，所以反向 SSH 超时不能单独证明板卡 TCP 故障。没有改动构建机路由。

保留旧实测配置 `rgmii-rxid`、MAC TX=13/RX=10、PHY 仅 RX 2 ns 延时。source-v9 的 `PLL_PERI0=0xff126310` 与旧启动链一致，GMAC PHY 分频仍为 150 MHz / 6；目前没有依据盲改延时、分频或 checksum offload。下一步需结合 PHY 主从协商、接收器训练状态与实际时钟读回定位降速，并在稳定 1000/full 下重新完成吞吐、完整性和冷启动验证。

当前证据：`evidence/source-v9-ethernet-initial-20261116.log`、`source-v9-ethernet-roundtrip-20261116.log`、`source-v9-ethernet-server-20261116.json`、`source-v9-web-diagnostic-20261116.log`、`source-v9-web-buildhost-comparison-20261116.json`、`source-v9-ethernet-review-20261116.json`。诊断工具在 `tools/validation/ethernet_roundtrip_*.py` 和 `ethernet_web_probe.py`；工具自测与实板验收分开记录。

## v11：正式内核修正已通过网络数据测试，启动链仍不合格

已通过 v11 串口 root 登录，内核为 `6.18.63-11`，未经临时 PHY 模块修改即获得 DHCP 地址 `10.0.0.230`，1000 Mbps/full。全部受支持的 stmmac 自检返回 0，未支持项目返回 -95，flags=1。自检后的链路重协商期间首轮 ping 丢包，恢复后第二轮 ping 为 3/3。

随后与编译机完成 64 MiB 下载和 64 MiB 回传，客户端及服务器 SHA256 一致，MAC CRC/对齐/FIFO/总线错误计数没有增加。证据为 `evidence/ethernet-v11-selftest.log`、`evidence/ethernet-v11-network.log`、`evidence/ethernet-v11-network-server.json`。这是旧启动链下的网卡修正验证；v11 首次冷启动曾无提示复位一次，独立启动链和最终冷启动验收仍未完成。

v10 的 MAC 回环通过，PHY 回环失败；RGMII 引脚电气配置已生效但不足以修复时序。对 RTL8211F 的四种 TX/RX 延时组合做了可恢复测试：无延时、仅 TX、TX+RX 均失败；仅 RX 通过全部受支持的 stmmac 自检，未支持的项目返回 -95。开启 RX 之后重新请求 DHCP 成功，网关 ping 3/3 成功，4 MiB HTTP 下载/回传的 SHA256 与编译机一致，CRC 错误未增加。

v11 通过标准 `phy-mode="rgmii-rxid"` 固化 PHY RX 的 2 ns 延时，保留实测通过的 MAC TX=13/RX=10 taps，PHY TX 延时关闭。增量补丁 `0009-t736-phy-rx-delay.patch`；临时诊断模块已卸载并恢复寄存器，不安装到系统。

网络实际位于 10.0.0.0/24 后，经 192.168.1.50 NAT 访问编译机。因此之前在编译机按板卡 MAC 过滤而未抓到包，不能证明物理发送失败。

## v10：显式设置 RGMII 引脚电气参数（单独修改仍未通过网络）

v9 完整 FES 写入、冷启动、root 登录通过，RTL8211F ID 为 `001cc916`，千兆全双工链路建立，但有效 RX 为 0，DHCP 未成功。临时百兆协商仍无有效 RX，只记录两个对齐错误帧；已恢复原千兆广告。只读诊断确认 PHY CLKOUT 已启用、MAC 时钟未关闭，PH0–PH15 仍为默认 drive=20、bias disabled。

v10 使用板级参考的 `drive-strength=<40>`、`bias-pull-up`，导出增量补丁 `0008-t736-rgmii-pad-configuration.patch`，启用官方 `CONFIG_STMMAC_SELFTESTS`。仍通过完整 raw/FES 部署，保留 v4/v6 回退文件和现有凭据。构建成功不代表网络已通过。

## v9：选择外部 PHY

进一步核对 T736 User Manual 18.1.7.1 后确认：GMAC_CFG_REG bit 15 的复位值为 1，表示内部 PHY；外接 RTL8211F 必须清为 0。原来的保留位读改写错误地保留了这个选择位。`0007-t736-external-phy-select.patch` 显式清除 bit 15，保留 v7 的 MDIO 复位及 v8 的 AXI/AHB 修复；预期扩展配置为 `0x11545`。

当前构建版本为 6.18.63-9，完整根系统脚本为 `tools/build_raw_rootfs_v9.sh`，打包参数为 `--revision 9`。七个补丁在官方源码副本上应用成功，驱动和 DTB 一致性通过。v8 已写入并校验成功，但发现 PHY_SELECT 问题后未冷启动，保持同一 FES RAM 服务继续写入 v9；不能把 v8 记为实机网络通过。

## v8：修正 MAC 复位域

v7 已经通过 FES 完整写入并冷启动，PH16 为已申请的高电平输出；但 PHY 仍未响应。在 `/run` 临时加载的只读诊断模块（未安装到系统镜像）确认 PH0–PH15 mux 全为 5、PHY 时钟寄存器为 `0x80000005`、总线/复位寄存器为 `0x00030001`，MDIO 全部 32 个地址读回 `ffff:ffff`。关键是 RGMII 扩展寄存器实际为 `0x00008000`，平台初始化的配置已经丢失。

与芯片时钟/复位定义及官方 stmmac 调用顺序对照后，发现 `stmmaceth` 错接了 AHB bit 16；核心在平台初始化后脉冲复位，清除了扩展配置。`0006-t736-gmac-reset-domains.patch` 把 `stmmaceth` 接到 AXI bit 17、`ahb` 接到 bit 16，并在平台配置前取得平衡的 AHB shared-reset 引用。探测完成后读回配置，若不一致就明确失败，避免将配置丢失误报为成功。

v8 软件包版本为 6.18.63-8，完整根系统脚本为 `tools/build_raw_rootfs_v8.sh`。六个补丁在官方源码副本上应用成功，导出驱动与工作树一致、重建 DTB 与软件包 DTB 字节一致。任务 `flash-1794566410769428000` 写入和校验成功；未冷启动，由 v9 取代。

## v7：在 PHY 探测前完成复位

v6 实机 debugfs 显示 PH0–PH15 已由 GMAC 接管、25 MHz PHY 时钟已启用，但 PH16 为 UNCLAIMED、MDIO 设备列表为空。官方 Linux 的 `fwnode_mdiobus_register_phy()` 先调用 `get_phy_device()` 读取 ID，成功后才进入注册过程并申请子节点的复位 GPIO；因此 v6 子节点的复位配置不能启动一个尚未响应的 PHY。

v7 将 PH16 放到标准 MDIO 总线 `reset-gpios`，使用 `reset-delay-us=10000` 与 `reset-post-delay-us=150000`，在读取 ID 前完成复位。PHY 地址仍为 1，没有硬编码 PHY ID 来绕过识别。增量补丁是 `patches/linux/0005-t736-mdio-reset-before-discovery.patch`。

v7 软件包版本为 6.18.63-7。`tools/build_raw_rootfs_v7.sh` 从已验证的 v6 staging 构建完整根系统，保留凭据、v4 和 v6 回退文件；`tools/pack_armbian_raw_v6.py --revision 7` 生成新 raw，并检查已有 loader 与原始 Tina 组件完全一致。五个导出补丁在官方源码副本上应用成功，重建 DTB 与软件包 DTB 字节一致。任务 `flash-1794565260734178000` 写入 8,589,934,592 字节且校验成功；冷启动和 root 登录通过，启动文件哈希一致、systemd 失败单元为 0。PHY 仍缺失，继续由 v8 处理。

## v6 历史状态

当前板上已通过 FES/raw 安装 v6，并冷启动至 Armbian 登录界面。GMAC 控制器已探测，但 PHY 未识别，网络尚不可用。

## 已实现

- `clk-t736-gmac.c` 仅接管 CCU 的 GMAC0 PHY 分频/门控、总线门控、MBUS 门控和两路复位，不覆盖 eMMC/USB 的资源。
- 25 MHz PHY 参考由厂家已配置的 150 MHz 父时钟除以 6；AHB/MBUS/PLL 和共享 PHY 电源仍依赖厂家初始化，不能称为完整 CCU/PMIC 驱动。
- `dwmac-t736.c` 使用官方 Linux stmmac 的 GMAC4/5 核心、队列 0 独立收发 IRQ、32-bit DMA、禁用 split header。
- PH0–PH15 为 RGMII/MDIO/25 MHz，PH16 为 PHY 低有效复位；MDIO 地址 1，等待 10 ms / 150 ms。
- `phy-mode = "rgmii"`：主线 RTL8211F 驱动明确关闭 PHY 的 RX/TX 延时，MAC 使用候选 TX=13、RX=10 taps。实际通信仍需验证。

## 复现

v6 构建产物版本为 6.18.63-6，现有产物与验证记录保留。构建脚本只构建，不安装或重启。

默认 Armbian 补丁序列保持 v5：0001 板级驱动、0002 主 PIO 布局、0003 缺失 PA 的 IRQ 编号修复。`patches/linux/0004-t736-gmac0-candidate.patch` 是可选网卡增量，不自动加入默认序列。

交叉打包使用 `DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders`，不生成需要目标原生开发库的 headers 包，保留 image/libc 包的正常依赖检查。初次打包失败也保留在构建日志中；最后一次打包退出码为 0。

## 当前证据

- 85 项地址/位移/掩码检查和 216 个引脚 IRQ 表回归通过。
- 在官方源码文件副本上依次应用四个导出补丁成功，重新编译的 DTB 与 v6 包中 DTB 完全一致。
- 软件包 SHA256：`717f9e6778884294a5563f21b003446af9d4f3d888a0aa3991db3ffa38a20b7a`。
- DTB SHA256：`020ee57ead39f4535a3945165ddb7eef513d1facdaa8496f62a9f6c387d5f2b4`。
- 冷启动识别 Synopsys ID `0x53`、DWMAC4/5，接口命名为 `end0`。
- `MDIO device at address 1 is missing`，随后 `cannot attach to PHY (error: -19)`；没有链路、DHCP、双向流量或性能通过结论。

完整 Armbian raw 的 FES 写入和校验已完成，证据为 `evidence/raw-v6-flash-fes-status.json`、`evidence/raw-v6-coldboot.log`。root 登录、GPT 迁移、启动文件校验、16 MiB fsync/O_DIRECT 测试及再次冷启动已经通过，systemd 失败单元为 0。`end0` 仍为 DOWN，重启后同样报告 PHY 缺失。后续保持用户要求的 FES/raw 更新路线，首先核查 PHY 电源、复位、时钟和 MDIO 地址。
