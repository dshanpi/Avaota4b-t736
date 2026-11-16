# 持久 GPU profile 与 source-v9/v10 镜像

`t736-gpu-profile` 管理持久桌面配置；`software` 恢复已保存的软件配置，`gpu` 安装已通过实板实验的 IMG 配置。软件 profile 为新镜像的默认值。当前 GPU provider 尚待验证时，不得将模板填写为 passed。

板上运行必须使用独立 root 控制台，先保存 `t736` 和 `Debian-gdm` 会话中的工作。切换会停止 GDM，并终止这两个用户的会话和 user manager，防止旧 D-Bus/systemd 环境泄漏；不终止 root。若此前运行过临时 `gpu_session_trial.sh enable`，先执行其 `rollback`。持久 helper 不加载/卸载 GPU 模块，不修改内核与固件。

```sh
t736-gpu-profile status
t736-gpu-profile gpu --evidence /root/accepted-gpu.json
t736-gpu-profile software
```

GPU 模式要求 v15 运行内核、已加载 `pvrsrvkm` 和唯一对应 render node，以及 `24.2.6603887-2` userspace 包；还要求实板验收 JSON 的五项检查均通过，并核对其指定的内核、DTB、模块、userspace manifest、`runtime-gles.json` 和固件哈希。包内文件/链接及固件与 manifest 再核对，并调用该 manifest 绑定的包内 helper 验证 runtime-gles 链接。验收记录不是自动生成的成功结论，必须来自真实的 GBM 绘制读回、X11 probe、GNOME 硬件 renderer、GTK 应用硬件 renderer 与 1080p HDMI 结果。GTK 项要求真实应用的 EGL/GLES PowerVR 渲染证据，不能只凭 GNOME Shell 已使用 GPU 推断应用也使用 GPU。记录测试 boot ID、原始日志 SHA256、实际部署文件哈希以及对应 kernel.deb 与 userspace.deb SHA256；不要仅凭 `uname` 相同复用旧记录。

模板位于 `config/gpu-v9/acceptance.template.json`，故意为 pending/false/空哈希，不能启用 GPU。`evidence_logs` 为“项目内原始日志相对路径 → SHA256”映射；helper 校验记录完整性与已安装 artifact，原始日志内容由验收者审阅，helper 不声称重新执行其中的硬件测试。新 raw 的冷启动和桌面验收仍须单独完成。

GPU 配置写入 `/etc/environment`、`/etc/X11/xorg.conf.d/20-t736-modesetting.conf`、`/etc/systemd/system/gdm.service.d/90-t736-gpu-profile.conf` 和 `/etc/modules-load.d/t736-gpu.conf`。最后一项使后续开机加载模块。Xorg 仅启用 glamor，仍保留 `PageFlip=false`、`SWcursor=true` 和原 DRM 节点。GDM 使用 `runtime-gles` 库视图和 GLES2；该视图排除厂家 `libvulkan.so*`，系统 Vulkan loader 继续由 Debian 提供。GTK 应用使用 `GDK_BACKEND=x11`、`GDK_DISABLE=gl-api,glx,vulkan`、`GSK_RENDERER=opengl` 选择已验证的 EGL/GLES 路径。环境中的无关项保留，旧 GTK 设置及冲突的图形栈覆盖项被替换或删除，包括 `VK_ICD_FILENAMES`、`VK_DRIVER_FILES`、`VK_ADD_DRIVER_FILES`；切回 software 完整恢复捕获的环境文件，因此本次加入的 GTK 设置不会残留。

GPU profile 使用 `config/gpu-v10/20-t736-modesetting-1080p.conf`（镜像内 `/usr/share/t736-gpu-profile/20-t736-modesetting-1080p.conf`），指定 HDMI-1 的 1920×1080@60、24-bit depth 和 1920×1080 virtual screen。software 恢复原 Xorg 文件。它不修改内核 framebuffer 的启动模式；GDM 和用户会话的完整切换仍需实板验收。

`t736-mutter-1080p` 默认只读当前 session bus 的 `GetCurrentState`，选择真实输出身份和 1080p60 mode ID。`--persist` 使用 method 0 校验、需要时 method 1 临时应用，保持全局 D-Bus 属性为空（Xorg 不支持设置 layout-mode）；随后连续验证 25 秒，备份并原子更新当前会话用户的 `monitors.xml`。若当前已是目标模式，不重复 modeset。脚本不用会触发确认倒计时的 method 2。运行用户和 `--config` 应对应同一个会话；它不修改其他用户或 GDM 的配置。

```sh
runuser -u t736 -- env XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    /usr/local/sbin/t736-mutter-1080p --persist --config /home/t736/.config/monitors.xml
```

Mutter 脚本已在当前实板完成 25 秒、26 次状态检查与 XML 保存；这不等于新镜像冷启动、GDM/用户切换或实体屏幕稳定性已通过。

首次切换只允许从已知软件环境捕获基线；原文件、权限和是否存在的状态存于 `/var/lib/t736-gpu-profile/software/`，不会被后续切换覆盖。该目录限 root 读取，因为 `/etc/environment` 可能含私有值。锁阻止并发切换；失败自动恢复软件配置并重启 GDM，诊断保留。SIGKILL、断电等中断会留下 `pending.json`；保留独立 root 通道并执行 `software` 恢复。成功启动 GDM 不等于硬件或实体屏幕验收通过。`software` 可在不同运行内核下执行，也不会卸载已加载的模块。

离线 `--root /absolute/mounted-root` 只编辑该根目录内文件，不调用 systemctl、loginctl，不读宿主运行内核。它仍检查 GPU 验收记录与目标镜像内的 artifact；software 不需要 GPU 证据。

```sh
# 默认软件桌面；输入是本轮明确构建的包和模块。
sudo sh tools/build_source_rootfs_v9.sh /absolute/kernel.deb /absolute/pvrsrvkm.ko

# 只有上述实板实验真实通过、记录与输入文件对应，才构建 GPU 默认桌面。
sudo sh tools/build_source_rootfs_v9.sh --userspace-deb /absolute/userspace-2.deb /absolute/kernel.deb /absolute/pvrsrvkm.ko gpu /absolute/accepted-gpu.json
sudo python3 tools/pack_t736_source_boot.py --variant v9
```

source-v9 仍使用 `build_source_rootfs_v9.sh`，从固定 v8 基线复制，内核为 `6.18.63-t736-gpu-v15`。当前 source-v10 改为独立入口 `build_source_rootfs_v10.sh`，从已完成的 source-v9 ext4 复制，目标为 `6.18.63-t736-storage-v16`。v10 尚在准备，主 packer 在创建 raw 前拒绝执行；不得用旧 v9 builder 的 `--variant v10` 路径冒充新 v16 镜像。

SPL 固定 v4；SPL v5 已弃用，CLI 与加载 API 均拒绝。最终 CPU 初始化位于独立 U-Boot CPU-D 包，必须绑定该包和独立实板证据。CPU-C 的 A76 路径通过但 A55 校验失败，不能作为成功默认；CPU0 runtime hotplug 不受支持，不能用于再次尝试 A55 调频。

`prepare_gpu_storage_v16.py --output /new/helper-directory` 生成 v16 profile、runtime-check、1080p 配置、Mutter helper 和 pending 验收模板。只改变 profile 的 RELEASE 常量、runtime-check 的 uname guard 和说明注释；原 v15 helper 不改。manifest 记录模板和生成文件 SHA256。生成成功和离线测试通过不代表 GPU 硬件通过；v16 必须重新生成真实验收记录，不能复用 v15 的 passed 字段。

v10 builder 将原 v15 profile/runtime/session-trial 和旧 CPU 试验模块归档至 `/boot/t736-source-v15-recovery/legacy-tools`，保留哈希；旧 session-trial 含 v7/v15 假设，不作为 v16 操作入口。新 runtime-check 支持 `status` 只读诊断和 `gbm` 显式加载模块及绘制读回；新 profile 的启用条件仍要求实板证据。它不移植或运行旧 A76 调频模块。

v10 的明确包输入为 kernel v16、相同 ABI 的 PVR 和 userspace `24.2.6603887-2`。媒体包版本从真实安装记录固定为 ffmpeg/libavcodec61 `7:7.1.5-0+deb13u1`，安装状态不代表硬件视频解码。详见 `docs/SOURCE_V10_DRAFT.md`。构建前必须提供 v16 GPU 验收，打包前还要完成 CPU-D、Firefox 与输入的绑定；所有新完整镜像的冷启动标志保持 false，直到该镜像实板完成。

`source-v9` 与历史以太网实验 `raw-v9` 是不同编号序列，文件和状态目录不相交。历史 `build_raw_rootfs_v9.sh`、`pack_armbian_raw_v6.py --revision 9`、`rootfs-armbian-raw-v9.ext4` 和 `t736-armbian-v9.raw.img` 对应旧厂家启动链，不用于本轮源码启动链封装。
