# source-v6 验收工具

这些文件由已执行的实板验收辅助脚本整理而来。它们不接管串口、不控制电源、不进入 FEL，也不自动连接板卡。板上脚本仅在操作者确认目标板及运行状态后手动执行；本目录不是自动硬件测试套件。

脚本保留 source-v6 的固定内核、文件哈希、设备路径和介质容量检查，不是通用烧录或通用分区修复工具。原始分析目录副本保留不动。

| 文件 | 执行位置 | 行为 |
|---|---|---|
| `display_check.sh` | 已启动的板上 Linux，通常需 root 读取 debugfs | 只读显示日志、EDID、DRM 状态、时钟、电源域与中断；不切换模式 |
| `source_v6_runtime_check.sh` | 板上 Linux，root | 核验启动文件、SPL、系统与显示；**写入** `/root/t736-source-v6-io-check.bin` 16 MiB 及其 SHA256 文件，fsync 后 O_DIRECT 读回；已存在测试文件时停止 |
| `source_v6_reboot_check.sh` | 板上 Linux，root | 读取并核验跨重启文件、GPT、系统和显示状态，向固定网关 `10.0.0.1` 发三次 ping；不修改分区或启动配置 |
| `source_v6_finalize_gpt.sh` | 板上 Linux，root/Bash | **写盘操作，仅手动**：备份 GPT、迁移备份表到固定 eMMC 物理尾部、更新 LBA 40960 兼容副本，并输出读回数据；不扩容分区 |
| `t736_fb_capture.c` | 编译为 ARM64 程序后在板上执行 | 只读 `/dev/fb0`，查询当前布局并将可见区域以 PPM 输出到 stdout；不修改像素或显示模式 |
| `decode_t736_fb_capture.py` | 主机 Python，需 Pillow | 从串口日志的 `FB_CAPTURE_BEGIN/END` 解码 gzip/base64 PPM，**写入本地主机 PNG** |
| `verify_source_v6_gpt.py` | 主机 Python 标准库 | 只读本地日志与 raw 镜像，核验主备 CRC、镜像副本和六个分区条目；**写入本地证据目录**的解码快照与 JSON，不接触板卡 |

`source_v6_finalize_gpt.sh` 固定检查 `/dev/mmcblk0` 为 122142720 个 512 字节扇区的 MMC、根系统位于 p4，并要求备份目录不存在。它会改变分区表元数据，不能作为日常只读健康检查，也不要在另一块板或不同布局上直接运行。持久固件更新仍使用专用完整 raw/FES 流程。

## 本地 GPT 核验

将完整串口输出保存为证据目录中的 `source-v6-gpt-readback.log`，然后明确指定作为比较基准的 source-v6 raw 文件：

```sh
python3 tools/validation/verify_source_v6_gpt.py \
  --raw-image /path/to/t736-armbian-source-v6.raw.img
```

证据目录默认是本项目 `evidence/`；也可指定 `--evidence-dir /path/to/evidence`。程序成功后生成 `source-v6-gpt-after-{primary,mirror,tail}.bin` 和 `source-v6-gpt-verification.json`。这里比较的是原镜像的分区条目，并不重新计算整个 8 GiB raw 的 SHA256；部署前的镜像完整性检查是独立步骤。

本次整理仅用已有本地 source-v6 raw 和串口日志重跑此校验，未对板卡重新执行 GPT 写入。

## 帧缓冲抓图

交叉编译示例：

```sh
aarch64-linux-gnu-gcc -O2 -Wall -Wextra \
  tools/validation/t736_fb_capture.c -o /path/to/t736_fb_capture
```

在板上运行程序并将 stdout 保存为 PPM。通过串口传回时，在 gzip/base64 内容两侧分别输出独占一行的 `FB_CAPTURE_BEGIN`、`FB_CAPTURE_END` 标记，再在主机解码：

```sh
python3 tools/validation/decode_t736_fb_capture.py capture.log capture.png
```

抓图反映 framebuffer 内容，不能代替实体屏幕的目视验收。程序仅支持本次已验证的 32 bpp、RGB 各 8 位格式。
