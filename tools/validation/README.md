# source-v6 与 source-v7 验收工具

source-v6 文件由已执行的实板验收辅助脚本整理而来；source-v7 工具为桌面候选新准备，**截至 2026-11-16 仍在构建，未上板验证**。它们不接管串口、不控制电源、不进入 FEL，也不自动连接板卡。板上脚本仅在操作者确认目标板及运行状态后手动执行；本目录不是自动硬件测试套件。

v6 脚本保留其固定内核、文件哈希、设备路径和介质容量检查，不应原样当作 v7 检查；v7 也有自己的固定内核及布局要求。它们不是通用烧录或通用分区修复工具。原始分析目录副本保留不动。

| 文件 | 执行位置 | 行为 |
|---|---|---|
| `display_check.sh` | 已启动的板上 Linux，通常需 root 读取 debugfs | 只读显示日志、EDID、DRM 状态、时钟、电源域与中断；不切换模式 |
| `source_v6_runtime_check.sh` | 板上 Linux，root | 核验启动文件、SPL、系统与显示；**写入** `/root/t736-source-v6-io-check.bin` 16 MiB 及其 SHA256 文件，fsync 后 O_DIRECT 读回；已存在测试文件时停止 |
| `source_v6_reboot_check.sh` | 板上 Linux，root | 读取并核验跨重启文件、GPT、系统和显示状态，向固定网关 `10.0.0.1` 发三次 ping；不修改分区或启动配置 |
| `source_v6_finalize_gpt.sh` | 板上 Linux，root/Bash | **写盘操作，仅手动**：备份 GPT、迁移备份表到固定 eMMC 物理尾部、更新 LBA 40960 兼容副本，并输出读回数据；不扩容分区 |
| `t736_fb_capture.c` | 编译为 ARM64 程序后在板上执行 | 只读 `/dev/fb0`，查询当前布局并将可见区域以 PPM 输出到 stdout；不修改像素或显示模式 |
| `decode_t736_fb_capture.py` | 主机 Python，需 Pillow | 从串口日志的 `FB_CAPTURE_BEGIN/END` 解码 gzip/base64 PPM，**写入本地主机 PNG** |
| `verify_source_v6_gpt.py` | 主机 Python 标准库 | 只读本地日志与 raw 镜像，核验主备 CRC、镜像副本和六个分区条目；**写入本地证据目录**的解码快照与 JSON，不接触板卡 |
| `source_v7_desktop_check.sh` | 已启动的 v7 板上 Linux，推荐 root | 只读查询 v14、GDM、DRM、输入节点、会话和 GLX；不登录、不截屏、不采集按键、不改变服务。退出码 0/1/2 分别表示所列检查通过/失败/信息不完整 |
| `x11_input.py` | 已观察到画面的 X11 环境，Python/libX11/libXtst | **注入输入**：向显式指定的 `DISPLAY`/`XAUTHORITY` 发送单键、限定 ASCII 文本或左键点击；无窗口发现、焦点猜测或剪贴板读取，不能当作物理键鼠验收 |
| `source_v7_finalize_gpt.sh` | v7 板上 Linux，root/Bash | **写盘操作，仅手动**：检查固定介质、p4 PARTUUID、六项分区条目哈希，再备份并迁移 GPT；不扩容 |
| `verify_source_v7_gpt.py` | 主机 Python 标准库 | 只读 v7 raw 和指定日志，**写入本地** GPT 快照及验证 JSON；不访问板卡 |

`source_v6_finalize_gpt.sh` 固定检查 `/dev/mmcblk0` 为 122142720 个 512 字节扇区的 MMC、根系统位于 p4，并要求备份目录不存在。它会改变分区表元数据，不能作为日常只读健康检查，也不要在另一块板或不同布局上直接运行。持久固件更新仍使用专用完整 raw/FES 流程。

## 本地 GPT 核验

将完整串口输出保存为证据目录中的 `source-v6-gpt-readback.log`，然后明确指定作为比较基准的 source-v6 raw 文件：

```sh
python3 tools/validation/verify_source_v6_gpt.py \
  --raw-image /path/to/t736-armbian-source-v6.raw.img
```

证据目录默认是本项目 `evidence/`；也可指定 `--evidence-dir /path/to/evidence`。程序成功后生成 `source-v6-gpt-after-{primary,mirror,tail}.bin` 和 `source-v6-gpt-verification.json`。这里比较的是原镜像的分区条目，并不重新计算整个 8 GiB raw 的 SHA256；部署前的镜像完整性检查是独立步骤。

本次整理仅用已有本地 source-v6 raw 和串口日志重跑此校验，未对板卡重新执行 GPT 写入。

## source-v7 构建与桌面验收

构建命令、两阶段与 `resume-prepare` 条件、临时镜像源恢复、私有状态归档与凭据文件均见 [BUILD.md](../../docs/BUILD.md)。v7 目标为 `6.18.63-t736-hdmi-v14` + GNOME Xorg + 稳定 Mesa llvmpipe，GDM 使用普通账户 `t736`，禁用自动登录与定时登录，保留默认 root 图形登录限制。密码由用户选择并私有注入，不写入源码、参数、截图或公开证据。

在实际 GDM 完成普通账户登录后运行：

```sh
bash tools/validation/source_v7_desktop_check.sh
```

保存完整输出及退出码。脚本的 GLX 调用使用已有图形会话环境并禁用 Mesa shader 磁盘缓存；成功只代表 GLX 可响应，脚本也能报告其他用户或 Wayland 会话。因此即使退出码为 0，仍须从实际日志核对账户为 `t736`、会话类型为 `x11`、renderer 为 `llvmpipe`，不能仅凭 PASS 宣告目标配置通过。

桌面验收还需要实际 GDM 口令登录、锁屏解锁、注销重登、Firefox/文件管理器等应用、中文输入、重启后的会话和 HDMI 稳定性。记录结果、boot ID 和截图哈希，不记录口令。`runuser`、已经存在的会话或自动登录均不能证明口令正确。脚本不自动实施这些交互步骤，也不证明物理键鼠可用。

截图可通过 GNOME 自带界面完成，或在已有 Xorg 会话的终端中显式执行 `xwd -root -silent -out "$HOME/t736-desktop.xwd"`，传回后转为 PNG 并检查图像确为真实桌面。截图只能证明捕获的图形内容；证明实体 HDMI 显示仍需目视或显示器照片。不要把 tty1 framebuffer 图片作为 GNOME 登录成功的证据。

`x11_input.py` 可辅助已授权的实际 GDM 交互；每一步前应观察画面并明确目标，调用者必须给出可读的 `XAUTHORITY` 和 `DISPLAY`。文本只能通过标准输入传入，限 1–256 个无换行的可打印 ASCII 字节，要求键盘 group 0 且无活动或锁定修饰键；不支持中文文本注入。示例只展示非私密账户名称：

```sh
printf '%s' t736 | python3 tools/validation/x11_input.py text
python3 tools/validation/x11_input.py key Return
```

该工具只注入输入，不修改键盘映射，也不替代观察或身份验证；凭据输入必须使用私有、无回显的通道，不能把值写进命令行或日志。

时间验收独立于桌面脚本：核对当前 UTC、`systemd-timesyncd` 状态和 `timedatectl show -p NTPSynchronized`，并在重启后确认时间不会回到旧值。fake-hwclock 0.14 使用 `fake-hwclock-load.service`、`fake-hwclock-save.service` 与 `fake-hwclock-save.timer`；旧单体 `fake-hwclock.service` 的状态不能代表这些单元。finalize 写入的构建 UTC 只是首次启动基准，不能当作 NTP 已同步的证据。

## source-v7 GPT 核验

`source_v7_finalize_gpt.sh` 只针对当前 122142720 个 512 字节扇区的 MMC（62537072640 字节），要求根为 `/dev/mmcblk0p4`、PARTUUID 为 `a0085546-4166-744a-a353-fca9272b8e48`，且六项分区条目哈希匹配。它要求 `/root/t736-source-v7-gpt-before-20261116` 不存在；写入前保存旧表，保持分区不变，把备份 header 移到 LBA 122142719 并更新 LBA 40960 兼容副本。它不是通用修复或扩容入口。

将实际执行的完整串口输出保存为 `source-v7-gpt-readback-20261116.log`，明确指定部署的原始 v7 raw 后在主机运行：

```sh
python3 tools/validation/verify_source_v7_gpt.py \
  --raw-image /path/to/t736-armbian-source-v7.raw.img \
  --evidence-dir /path/to/evidence
```

输出为 `source-v7-gpt-after-{primary,mirror,tail}-20261116.bin` 和 `source-v7-gpt-verification-20261116.json`。输入日志与输出文件日期由当前脚本固定。该检查验证 GPT 字节与原镜像条目的一致性，不计算整盘镜像 SHA256；部署前镜像完整性和部署后存储读写仍需各自验收。此前用 v6 本地样本做过的布局回归只说明工具可处理该布局，不能作为 v7 上板结果。

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
