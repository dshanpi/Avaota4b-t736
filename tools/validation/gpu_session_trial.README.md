# v8 GPU 会话实验

`gpu_session_trial.sh` 仅用于已启动 `6.18.63-t736-gpu-v15`、安装且校验通过 IMG 用户态包 `24.2.6603887-2` 和对应固件的新 v8。**先完成 GPU 内核、渲染节点及独立绘制/读回 probe 验证，再执行 enable。** 本脚本不操作板卡连接、镜像、分区或内核加载。

包 `-2` 的 `runtime-gles` 视图保留厂家 EGL/GLES/GBM，排除厂家 Vulkan loader，使 GTK 使用系统 `libvulkan.so.1`。脚本校验该视图，清除 Vulkan ICD 覆盖变量；厂家原始库及其校验记录保持不变。模块版本检查使用 Python，不要求板上安装 `strings`。

必须从独立的 root 串口控制台执行；脚本会停止 GDM，并结束 `t736`、`Debian-gdm` 的会话和 user manager，清除遗留的 D-Bus/systemd 软件渲染环境。不要从这两个账户的 SSH、图形终端或其 `sudo`/`su` 会话运行。先保存这两个账户的工作。

```sh
bash tools/validation/gpu_session_trial.sh status
bash tools/validation/gpu_session_trial.sh enable
# 观察日志、实际 Xorg/GNOME 的库映射、renderer 及画面后，按需恢复：
bash tools/validation/gpu_session_trial.sh rollback
```

备份和诊断保存在 `/root/t736-gpu-session-trial/runs/`；锁阻止并发执行，`active` 记录尚待回退的实验。脚本保留 `/etc/environment` 中无关设置，清理软件渲染与外部图形栈覆盖项，加入厂家库路径及 `COGL_DRIVER=gles2`；在 `/run/systemd/system/gdm.service.d/` 放置临时环境文件引用。只有既有 `20-t736-modesetting.conf` 与预期逐字匹配时才将 `AccelMethod` 改为 `glamor`，`PageFlip=false`、`SWcursor=true` 和 DRM 节点保持原值。

启用步骤失败会自动尝试完整回退并重启 GDM。显式 `rollback` 可在另一内核下执行，恢复原文件和原同名 drop-in，重建上述 GUI 用户环境；诊断和备份保留。若系统停止服务或文件写入本身失败，脚本保留 `active` 和 `rollback-needs-attention`，便于从独立 root 控制台再次回退。

成功启用只标记 `enabled-unverified`。GDM 单元 active、GNOME 加速检查返回 0、屏幕截图均不能单独证明硬件渲染通过。检查实际 Xorg 与 `gnome-shell` 的环境、加载库、IMG renderer、日志和实体 HDMI。临时 drop-in 在重启后消失，而 `/etc/environment` 与 Xorg 文件仍保留修改，因此实验期间不要把重启当作回退；应显式运行 `rollback`。断电、SIGKILL 或内核故障无法由 EXIT trap 恢复，但完整备份仍可用于后续回退。
