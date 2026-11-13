# 验证状态：2026-11-13

## 已实板验证

- U-Boot 2026.10 在 ARM64 模式运行，串口和重定位正常。
- 8 GiB 内存描述生效；4 GiB 以上地址完成抽样写读。
- Linux 6.18.63 的 8 个 CPU 全部上线，initramfs 自动执行 `/init`。
- eMMC A31Y5C 容量约 58.2 GiB，六个 GPT 分区可识别。
- 从 eMMC 读取 8.3 MB 验证 Image，SHA256 与构建产物一致。
- 在 boot 分区未用尾部完成 64 KiB 写入、清缓存回读比对，并恢复原数据。
- U-Boot 从 ext4 读取内核、initrd、DTB，文件读取约 5.3 MiB/s。
- Armbian rootfs 以读写方式挂载到 mmcblk0p4；systemd 到达 multi-user.target 和 ttyS0 登录。
- LYNX 对 Armbian 验证镜像报告写入成功及 verifyState=success。

## 实板版本与当前源码差异

实板 v2 U-Boot 缺少自动解压参数。启动 Armbian 时手工设置了：

```text
setenv kernel_comp_addr_r 0x68000000
setenv kernel_comp_size 0x385407
run bootcmd
```

`0x385407` 是当次压缩内核文件长度，不能用于任意新内核。
当前补丁在加载内核后保存 `${filesize}`，自动提供解压参数。
该修改已重新编译，尚未安装或完成实板自动启动验证。

`handoff/` 仅通过 ARM32 编译、反汇编检查及封装校验。没有实板运行结果。
所有烧录验证包仍保留厂家持久 Boot0/Boot1。

## 尚未完成

- 去掉旧运行时 U-Boot 2018.07，验证完整持久启动交接。
- 无人干预的断电冷启动。
- systemd-resolved 启动失败的日志诊断。
- initramfs 的 root fsck；构建时未识别实际根分区，启动日志提示缺少 fsck。
- eMMC DMA、高速时序、大容量压力测试和掉电测试。
- PCIe、网卡、Wi-Fi、蓝牙、HDMI、USB 及其他外设。
- 独立板级时钟、引脚、电源、休眠和动态调频支持。

登录时已观察到 root 初始密码认证通过。首次改密的 `Current password:` 需再次输入初始密码。
用户曾在该处输入新密码，出现认证令牌错误；该次操作不足以证明 PAM 或文件系统损坏。
备份时尚未取得用户完成改密后的 shell 验证结果。

## 证据

| 文件 | 内容 |
|---|---|
| `evidence/uboot-2026.10-hardware-validation.log` | 新 U-Boot、内存与 eMMC |
| `evidence/kernel-probe-v2-payload-crc.log` | 板端 Image、DTB、initramfs CRC |
| `evidence/linux-6.18.63-v2-firstboot.log` | 新内核、SMP、initramfs、块设备 |
| `evidence/linux-6.18.63-v2-emmc.log` | Image SHA256 读取验证 |
| `evidence/linux-6.18.63-v2-emmc-write.log` | 写入、回读、恢复标记 |
| `evidence/armbian-v2-uboot-ext4.log` | ext4 读取及 v2 解压参数问题 |
| `evidence/armbian-6.18.63-firstboot.log` | 根分区挂载、systemd、登录及现存服务错误 |
| `evidence/flash-verification.json` | 从原始 LYNX 结果提取的终态 |

日志中的 UTC/EST 构建时区来自构建机。本文日期按 Asia/Shanghai 记录。
日志保留早期失败和调试命令，不能仅靠检索一个错误行判断最终结果。
宿主工具测试只验证解析与封装逻辑，不替代硬件验证。
