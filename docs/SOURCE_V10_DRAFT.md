# source-v10 里程碑候选：CPU-D、storage-v16、GLES 与 FFmpeg

2026-11-16，source-v10 rootfs 和 8 GiB raw 已构建完成，尚未刷入实板。这份候选保存已通过组件级实测的 CPU-D、storage-v16 与 GPU/Firefox 配置；普通应用冷启动仍慢，后续优先推进 eMMC DMA 和高速模式。它不是最终验收镜像。

构建回执见 `evidence/source-v10-rootfs-build-result-20261116.json`（退出码 0，11:16:26 UTC），打包回执见 `evidence/source-v10-pack-result-20261116.json`（退出码 0，11:19:02 UTC）。构建机产物：

| 产物 | 字节数 | SHA256 |
| --- | ---: | --- |
| `output/rootfs-armbian-source-v10.ext4` | 7802716160 | `935a58e933ec831c470a5ef20c89dd5f0f675f417d4fa43bbe0a831e91414469` |
| `output/t736-armbian-source-v10.raw.img` | 8589934592 | `5af964ac3f24b747151bc4f3f292dcae943e6be28bc2d4cabe17dc876547cecf` |

完整清单 `output/t736-armbian-source-v10.raw.manifest.json` 已保存本地。大镜像仅在构建机，不能把本地清单误认为镜像已下载或已刷入。

## 固定输入

- SPL v4：221184 字节，SHA256 `a1d821eaee602fdf8b481c38cf4790d0f8a6f31539dd9fc6c1a714c1e92bd0ea`。弃用的 SPL v5 仍被 CLI/API 拒绝。
- CPU-D 启动包：325632 字节，SHA256 `f2ea2157acc3b65b2cc7416dfdba39650e7daa6679693c558fbbdcc30ef9780a`。实际冷启动 A76 从 416 MHz 切至 1014 MHz，A55 为 1014 MHz；8 核在线、最终寄存器与 30 秒验证通过。DSU 和 PMIC 电压不变。CPU0 runtime hotplug 不支持，不应重试。
- 内核 `6.18.63-t736-storage-v16`，包版本 `6.18.63-16`。PVR 使用同一 ABI 构建，用户态固定 `24.2.6603887-2`。
- FFmpeg 与 `libavcodec61` 均固定 `7:7.1.5-0+deb13u1`。
- 1080p `monitors.xml` 已复制给 t736 与 Debian-gdm；原 GDM/PAM/账户文件保留，未导入临时自动登录。
- v15 内核、initrd、DTB、完整模块树及旧工具保留在恢复路径。v16 工具使用只读 CPU 快照模块，旧可写试验模块归档。

输入清单为 `config/source-v10-inputs.json`。原始硬件证据、包与实际 rootfs 字节分别验证；离线构建成功不替代新镜像冷启动。

## 已完成的构建检查

`tools/build_source_rootfs_v10.sh` 在独立 mount namespace 中冻结输入并复制 v9 基线。包安装期间屏蔽启动选择 hooks，显式安装内核、用户态、PVR 和指定媒体包，执行 depmod/initrd，再核对内核、配置、System.map、DTB、PVR 和恢复文件后切换启动别名。卸载后 `e2fsck -fn` 通过并生成 `finalize.complete` 与 ext4 哈希。

`tools/pack_t736_source_boot.py --variant v10` 的真实输入门禁已通过：固定 SPL、CPU-D 包的几何/CRC/来源、最终 ext4 中 8 项 GPU/固件文件的实际字节、GPU 原始日志、CPU-D 冷启动和 v16 运行寄存器、Firefox baseline WebRender/PowerVR WebGL。打包后各区域重新读回哈希，`sfdisk --verify` 通过。

GPU GBM、GNOME/GTK GLES 和 Firefox baseline 已有组件级实测；Firefox 不依赖强制 WebRender、关闭沙箱或跳过 blocklist。v16 存储仍是 12 MHz、8-bit、legacy PIO；10.28 MiB/s 原始读取不等于 eMMC 高速验收。

## 尚未完成

- 普通应用冷启动延迟及后续 eMMC DMA/HS52/HS200/HS400 验收。
- source-v10 完整镜像刷入、冷启动与板端恢复路径验证；目前 `cold_boot_validated=false`。
- 板端 GPT 物理尾部修复。raw 的备份 GPT 在 LBA 16777215，物理卡尾为 122142719；未来刷入后使用 v10 专用 finalize/verify 工具，不能复用旧版本串口标记。
- 千兆以太网与硬件视频解码；目前均未验收。

当前 v10 仅保留为可追溯里程碑，不作为 DMA 后最终发布版本。
