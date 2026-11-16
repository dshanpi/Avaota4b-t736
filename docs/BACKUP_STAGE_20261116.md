# 2026-11-16 源码与阶段证据备份

本次保存已经完成的存储阶段与进行中的诊断源码，目标为 https://github.com/dshanpi/Avaota4b-t736 。公开检查点不等于最终镜像发布；发布状态以对应上传回执和提交为准。

## 已完成阶段

| 阶段 | 结果与证据 |
|---|---|
| v21 HS25 / HS50 | 两个有界启动、32 MiB 完整性与 GPU 健康阶段通过；`evidence/storage-v21-hs-stage-record-20261116.json` |
| v22 HS200 100 MHz | 64 个 tap 全部通过，选择中心31；独立32 MiB校验通过；`evidence/storage-v22-hs200-stage-record-20261116.json` |
| v22 HS200 200 MHz | 第一次调谐失败，保留失败记录；`evidence/storage-v22-hs200-200-failure-review-20261116.json` |
| v23 HS200 200 MHz | 修正仅调谐路径中 IDST[5] 卡错误汇总的分类；独立阶段通过；`evidence/storage-v23-hs200-stage-record-20261116.json` |

v23 实板为200 MHz、8-bit、1.8 V，HS_TIMING=0x02。64个tap扫描得到通过位图 `ffffffc0ffffffff`、abort失败位图 `0000003f00000000`，最长线性窗口0..31、选择tap15并完成16次中心校验。25项寄存器连续三遍一致。两套32 MiB直接文件读写模式、全部比较与哈希通过，临时文件已清理；64 KiB顺序读37.03/36.81 MiB/s、写89.10 MiB/s，整套10.086秒。systemd报告启动9.848秒。GPU60帧四象限回读通过，PVR错误计数为0。这些数字不是完整寿命、压力或实体屏幕验收。

默认启动选择仍为storage-v16和DT SHA256 `6124591aab1cfc02b287a88872835b20a6d2dac70b3e7cd2b93bb9af36d3e82b`。v23安装通过带fsync验证的备份事务后立即恢复默认，再用明确路径一次性启动；未保存新U-Boot环境。完整事务和产物哈希见 `evidence/storage-v23-updater-preparation-20261116.json` 和阶段记录。

## 代码入口与未完成项

| 用途 | 入口 |
|---|---|
| 已测试HS200 v23驱动 | `boards/t736_mmc_hs200_v23_draft.c`、`boards/t736_mmc_tuning_error_draft.h` |
| 调谐边界与错误分类测试 | `tests/test_mmc_hs200_error_recovery.py` |
| 精确产物安装/回退 | `tools/validation/update_t736_storage_v23.py`、`test_update_t736_storage_v23.py` |
| 只读时钟寄存器收集 | `tools/validation/t736-emmc-clocks-readonly-v4.c` |
| HS400隔离草稿 | `boards/t736_mmc_hs400_draft.c`、`t736_mmc_hs400_policy_draft.h`、`t736_mmc_hs400_tuning_draft.h`；默认关闭，未验收 |
| GTK回调与帧时间诊断 | `tools/validation/gtk_ui_frame_probe_steady.py`、`gtk_ui_frame_timings_probe.py` |
| EGL独立小窗口反馈 | `tools/validation/egl_x11_sync_feedback_probe.c`；窗口级交换计数，不替代GNOME或物理显示验证 |
| 桌面包清单与镜像构建 | `config/desktop-v7-packages.txt`、现有`tools/build_source_rootfs_v*.sh`；候选存在不代表最终镜像验收 |
| 公开备份准备/验证 | `tools/prepare_source_backup.py`；不会push或烧录 |

EGL v1与v2分别绑定源码/二进制哈希，构建记录为 `evidence/egl-x11-sync-feedback-probe-build-20261116.json`、`evidence/egl-x11-sync-feedback-probe-v2-build-20261116.json`。v2实板两次测试见 `evidence/egl-feedback-v23-probev2-i0-20261116.json`、`evidence/egl-feedback-v23-probev2-i1-20261116.json`、`evidence/egl-feedback-v23-probev2-summary-20261116.log`：每次提交60次、122次查询均成功、SBC到60、首尾两帧像素回读通过，反馈速率60/1。UST/MSC是垂直回扫反馈对，不能直接当成当前提交帧的显示时刻。GNOME帧反馈仍在诊断。

HS400、GNOME交互流畅度、最终整合镜像与其冷启动验收均未完成。厂家DDR库启动成功没有解决纯源码ZCal的PU/PD问题。历史source-v6完整镜像验收与本次增量阶段分别保留，见 `docs/BACKUP_SOURCE_V6.md`。

## 公开范围与核对

公开源码、配置、补丁、测试、构建脚本和允许的文本证据。完整raw/rootfs/内核包、厂家库及训练固件、认证与隧道日志、凭据、个人截图、私有home备份均不发布。唯一保留的历史PNG例外为此前已发布的source-v6控制台framebuffer图，不新增桌面截图。

`backup-manifest.json`逐文件记录公开副本SHA256、大小与原工作区SHA256。日志可做LF/ANSI清理，原始构建与实板证据哈希不因此改写；manifest分别保存规范化副本哈希。准备后校验全覆盖、JSON解析、Python编译、秘密扫描和私有路径排除，最终提交与push由主任务完成。
