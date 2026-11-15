# T736 Armbian FES/raw 流程

> 当前 v6–v11 镜像保留 Tina 运行时启动链，不符合用户要求，不能作为最终交付。目标是 Tina 仅用于 RAM 中的 FES 烧录，正常 eMMC 启动使用独立启动链。

目标流程中 Tina 只提供 RAM 中的 DRAM/FES 服务。现有 v6–v11 的 loader 文件确实只在 RAM 中使用，但 raw 本身仍包含 Tina 运行时启动组件，因此尚未满足该目标。
参考流程来自 https://github.com/dshanpi/AvaotaA1-T527-MainLineLinux ，但不复用 T527 的板级二进制或 SPL。

## v6 历史调试输入（不用于最终交付）

- `output/t736-armbian-v6.raw.img`：8 GiB，GPT、启动内容、Armbian 根系统及 v6 内核。
- `output/t736-tina-fes-loader.img`：从已验证的 Tina 包提取四个原始组件，0 分区。`fes1.fex`、`u-boot.fex`、`config.fex`、`board.fex` 均未修改。
- `output/t736-armbian-v6.raw.manifest.json`：镜像和组件 SHA-256、物理扇区位置、启动链依赖。

raw 继续使用已验证的厂家 Boot0/BL31/OP-TEE/SCP/U-Boot 2018 到主线 U-Boot 2026.10 的交接链。完整 Armbian 系统镜像不等于已移除全部厂家启动组件。T736 目前没有经过验证的主线 SPL，不能启用 T527 示例中的 `--emmc-boot0-from-image`。

## 地址合同

当前 Windows OpenixCLI 0.2.11 的 raw/command 路径已经进行地址换算：

```
FES 扇区 = (raw 物理扇区 - 40960) modulo 2^32
Tina MMC 扇区 = (FES 扇区 + 40960) modulo 2^32
```

因此 raw 从物理扇区 0 开始。第一笔 FES 地址 `4294926336`（`0xffff6000`）是这个换算的结果，不能再次修改 loader 的逻辑偏移。

2026-11-13 曾错误地把 RAM loader 的偏移改为 0，导致首块返回 FES `0xff`，已确认成功数据块为 0，但临时 MBR/GPT 已提交。该 loader 已移至 `evidence/REJECTED-*`，不再作为烧录输入；构建脚本已撤回该修改。准确记录见 `evidence/raw-v6-offset-rejected-status.json`。最终写入和启动结果须以完成的任务状态及冷启动日志为准。

## 构建与验证

构建机 `/home/ubuntu/t736-mainline` 中的 `tools/build_raw_rootfs_v6.sh` 生成历史根系统。`tools/pack_armbian_raw_v6.py` 仅保留历史调试复现用途，默认拒绝运行；显式传入 `--legacy-debug-image` 才会生成仍含 Tina Boot0/U-Boot 的旧链镜像。新清单固定 `release_eligible=false` 和 `independent_boot_compliant=false`。这不是独立启动镜像的打包入口。

8 GiB 是部署镜像长度，板上 eMMC 实测容量为 62,537,072,640 字节。启动后需要把备份 GPT 移到实际介质末端；剩余空间扩展及运行时检查单独记录。这不是安全擦除整个 eMMC 的流程。

LYNX 使用 `mode=command`、`verify=true`、`raw=true`，把 loaderPath 与 firmwarePath 分开传入。只跟踪当前任务直到确定结束。Windows FEL→FES 自动重连曾失败；只有在旧任务已经退出，并且同一绑定设备被重新确认处于 FES 后，才能继续当前 RAM 服务中的 raw 操作。不能在写入或校验期间断电。

烧录后必须断电再上电，检查实际新 U-Boot、Linux #6 和 Armbian 登录；FES 校验成功本身不能证明冷启动成功。

## v6 历史实测结果

任务 `flash-1794563107553214100` 返回 `state=success`、`verifyState=success`，传输 8,589,934,592 字节。随后硬件断电 3 秒再上电，无人工引导进入 U-Boot 2026.10、Linux #6、Armbian 26.11.0-trunk.128 trixie 登录界面。根分区 fsck clean 并挂载为读写。

随后已用用户提供的凭据完成 root 登录。板上 GPT 元数据备份在 `/root/t736-v6-gpt-before-20261113`，已将备份 GPT 从 LBA 16777215 迁到实际 eMMC 的 LBA 122142719，并同步物理 LBA 40960 的厂家 GPT 副本。主备头与分区表 CRC、互指位置、厂家副本一致性均通过独立读回检查；六个分区的全部条目与 raw 原镜像一致，没有扩容分区，新增可用空间保留为未分配空间。

内核、initrd、DTB、主线 U-Boot 启动分区、Boot0 用户区副本和两个厂家 boot package 副本的 SHA-256 与构建输入一致；v4 回退文件校验通过。16 MiB 文件写入加 fsync 与 O_DIRECT 回读 SHA-256 一致，eMMC 仍为 12 MHz、8-bit PIO。正常关机后再次硬件断电 3 秒，自动启动到 Armbian 并登录 root；新启动日志无 GPT 警告，根分区读写正常，systemd 失败单元为 0。

上述 GPT 迁移是本板安装后的操作，发布的 8 GiB raw 字节及哈希没有改变；再次写入更大介质后仍需进行这一收尾步骤。证据为 `evidence/raw-v6-gpt-verification.json`、`evidence/raw-v6-runtime-verification.json`、`evidence/raw-v6-gpt-fixed-coldboot.log`、`evidence/raw-v6-post-coldboot-validation.log`。

网卡控制器已经探测，但 MDIO 地址 1 没有 PHY，重启后仍为 `cannot attach to PHY (error: -19)`，不能报告网络可用。
