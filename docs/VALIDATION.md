# 实板验证状态（2026-11-13）

## v3 恢复与自动启动

恢复任务 `flash-1794540554778633200` 终态 success、verifyState success，committedBytes 613262400。
恢复镜像 SHA256：`322859ec6b41aeada69b3fc25e605a6934eff89ebd21bc9cae78ce2a9a59c48d`。

FES 恢复使旧 U-Boot 环境 CRC 失效。重新保存以下命令后，另一次硬件断电上电，全程不输入字符，到达 Armbian 登录：

```text
setenv bootcmd 'sunxi_flash read 0x60000000 boot; bootm 0x60000000'
saveenv
```

新版 U-Boot 自动读取压缩内核，并根据实际 `${filesize}` 设置解压缓冲区；已不需要手工设置长度。
这证明现存两级 U-Boot 链可自动启动，不证明独立 ARM32 交接候选已成功。

恢复后 root shell 实测：

- Linux 6.18.63-t736-armbian #3，软件包 6.18.63-3。
- 根分区起点 368640，长度 15239680 个 512 字节扇区。
- ext4 根系统读写挂载，7.1 GiB 文件系统。
- initramfs 执行 fsck.ext4，结果 clean。
- `/run` 是 781 MiB tmpfs，`/dev/shm` 是 3.9 GiB tmpfs，statfs 有真实容量。
- `systemctl --failed` 返回 0 个单元；systemd-resolved active。
- 已完成登录和改密；普通用户创建向导暂未完成。密码不进入日志或仓库。

DNS 服务启动成功仅证明之前缺少 IP_MULTICAST 的问题已解决；网卡和实际 DNS 查询尚未验证。

## v4 USB1 实板验证

内核包 `6.18.63-4` 编译完成。SHA256：`edebf4e704ee8716a5b8b90458fda32adf234bb3a2f5c98d0c1882a69b5ab3fe`。
DTB SHA256：`40ece1d8d9682477bdc892817696f93b5a162015e79b47a9b24c7cb72c39450a`。
主机配置检查覆盖 21 个关键符号，完整 Linux 补丁可应用到锁定官方快照。
已通过整包 SHA256 后安装，备份 v3 启动文件于 `/boot/t736-v3-recovery`。正常关机后用硬件断电上电，无输入启动 Linux #4 和 Armbian 登录。

- USB 时钟、R-PIO、PHY、EHCI、OHCI 驱动全部绑定，没有 deferred probe。
- DMA zone 覆盖 0x40000000..0xffffffff；DMA32 zone 为空，因为 DMA 已覆盖 32 位范围。
- PL6 output high，usb1-vbus regulator enabled。
- 真实 USB2 Hub `1a40:0101`（Terminus，4 口）以 480 Mbps 枚举；描述符及 Hub 端口状态控制请求成功，EHCI 中断工作。
- Hub 下游 port 3 已枚举 Realtek 0bda:0316 读卡器（1-1.3，480 Mbps），usb-storage/SCSI 探测成功。用户确认没有 TF 卡，容量 0 和“无介质”正常；存储读写、HID、OHCI 设备仍未验证。
- 根分区布局未改变，systemd 失败单元 0，systemd-resolved active。

## 证据

| 文件 | 结论 |
|---|---|
| `evidence/runtime-v3-recovered-coldboot-auto.log` | 保存环境后的无输入冷启动、fsck、systemd、登录 |
| `evidence/runtime-v3-recovered-validation.log` | 分区几何、tmpfs、服务、软件包版本 |
| `evidence/usb1-v4-coldboot.log` | v4 无输入启动及 Hub 枚举 |
| `evidence/usb1-v4-validation.log` | 驱动、DMA、供电、中断、系统服务 |
| `evidence/usb1-v4-hub-status.log` | Hub 描述符、端口状态与分区核验 |
| `evidence/runtime-v3-layout-incident.json` | 错误布局事故及恢复结果 |
| `evidence/linux-6.18.63-v2-emmc.log` | 历史 eMMC Image SHA256 读取 |
| `evidence/linux-6.18.63-v2-emmc-write.log` | 历史 64 KiB 写入、清缓存回读和原数据恢复 |

## 未完成

旧运行时 U-Boot 替换、eMMC 高速/DMA、完整内存/存储压力测试、独立 PMIC/CCU、PCIe、Ethernet、Wi-Fi、蓝牙、HDMI、USB 存储读写/HID、USB0/USB3、休眠与动态调频仍需实板工作。
宿主测试验证解析、布局和打包逻辑，不替代硬件测试。

## 原理图审计与编译验证

`docs/HARDWARE_MAP.md` 记录来源哈希、PDF 页序、连线及实板验证边界。主 PIO 候选修正通过 85 项实际 C 寄存器 helper 检查以及 ARM64 core/main/R-PIO 单对象编译，尚未上板。证据：`evidence/pio-layout-candidate-build.log`。

读卡器及临时 FAT 字符集模块证据：`evidence/usb1-v4-reader-validation.log`、`evidence/schematic-review-runtime.log`。两个 NLS 模块加载成功仅证明与 v4 兼容，不证明介质读写；未持久安装，下一次内核构建才包含配置修正。
