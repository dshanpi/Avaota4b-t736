# 2026-11-16 源码与阶段证据备份

本次为第二检查点的 prepare/verify，尚未提交或 push，目标为 https://github.com/dshanpi/Avaota4b-t736 。旧检查点 `72333d335c0256542ee9b60912d666c4dbd82649` 已发布并保留，回执为 `evidence/source-stage-upload-receipt-20261116.json`。本次冻结 v24 HS400 阶段、性能观测与 v25 计时诊断草稿；准备后的新构建结果须另行复核，不能自动扩大本次范围。公开检查点不等于最终镜像发布。

## 已完成阶段

| 阶段 | 结果与证据 |
|---|---|
| v21 HS25 / HS50 | 两个有界启动、32 MiB 完整性与 GPU 健康阶段通过；`evidence/storage-v21-hs-stage-record-20261116.json` |
| v22 HS200 100 MHz | 64 个 tap 全部通过，选择中心31；独立32 MiB校验通过；`evidence/storage-v22-hs200-stage-record-20261116.json` |
| v22 HS200 200 MHz | 第一次调谐失败，保留失败记录；`evidence/storage-v22-hs200-200-failure-review-20261116.json` |
| v23 HS200 200 MHz | 修正仅调谐路径中 IDST[5] 卡错误汇总的分类；独立阶段通过；`evidence/storage-v23-hs200-stage-record-20261116.json` |
| v24 HS400 100 MHz | 实际时钟100 MHz，命令中心31、DS中心28；两个32 MiB模式校验及GPU60帧/PVR健康通过 |
| v24 HS400 200 MHz | 实际时钟200 MHz，命令中心16、DS中心14；两个32 MiB模式校验及GPU60帧/PVR健康通过；两频点独立证据见 `evidence/storage-v24-hs400-stage-record-20261116.json` |

v23 实板为200 MHz、8-bit、1.8 V，HS_TIMING=0x02。64个tap扫描得到通过位图 `ffffffc0ffffffff`、abort失败位图 `0000003f00000000`，最长线性窗口0..31、选择tap15并完成16次中心校验。25项寄存器连续三遍一致。两套32 MiB直接文件读写模式、全部比较与哈希通过，临时文件已清理；64 KiB顺序读37.03/36.81 MiB/s、写89.10 MiB/s，整套10.086秒。systemd报告启动9.848秒。GPU60帧四象限回读通过，PVR错误计数为0。这些数字不是完整寿命、压力或实体屏幕验收。

v24 的 200 MHz 阶段为8-bit、1.8 V、HS_TIMING=0x03/BUS_WIDTH=0x06。命令通过位图 `fffffff1ffffffff`、线性窗口0..32、中心16；DS通过位图 `0000000003fffff0`、窗口4..25、中心14。25项寄存器三遍一致。32 MiB 两模式完整性校验通过；带比较/哈希的64 KiB读为28.03/45.68 MiB/s，写为102.50 MiB/s。完整运行日志是 `storage-v24-hs400-200-runtime-full-20261116.log`；较早的26行截断副本保留，不能替代完整证据。

默认启动选择仍为storage-v16和DT SHA256 `6124591aab1cfc02b287a88872835b20a6d2dac70b3e7cd2b93bb9af36d3e82b`。v23/v24 安装通过带fsync验证的备份事务后立即恢复默认，再用明确路径一次性启动；未保存新U-Boot环境。完整事务和产物哈希见各版本 updater preparation 和阶段记录。

## 性能样本的边界

| 读取观测 | 大小与协议 | MiB/s |
|---|---|---:|
| 短时直接读取 | 8 MiB，1 MiB 用户读取块，无比较/哈希 | 83.92 |
| 临时持续窗口一 | 同一8 MiB前缀重复64次，共512 MiB | 45.01 |
| 临时持续窗口二 | 同量读取，额外记录工作线程CPU端点计数 | 48.95 |
| 冻结探针基线 | 同量读取，目标缓冲区提前触碰；固定探针哈希 | 74.90 |

这些不是统一协议下的重复统计，不能认定差异来自缓冲区提前触碰或某类CPU。固定64 KiB请求上限使512 MiB样本拆成8192次块读取。临时窗口二的eMMC工作线程消耗6.17秒CPU，冻结窗口为2.45秒；CPU起终编号不代表驻留时间。源码表明读回包含从Normal Non-Cacheable DMA缓冲区复制到SG页，但各段成本尚未实测拆分。400 MB/s只是HS400接口上限。详见 `evidence/storage-v24-hs400-performance-review-20261116.json` 和其原始日志绑定。

## 代码入口与未完成项

| 用途 | 入口 |
|---|---|
| 已测试HS200 v23驱动 | `boards/t736_mmc_hs200_v23_draft.c`、`boards/t736_mmc_tuning_error_draft.h` |
| 调谐边界与错误分类测试 | `tests/test_mmc_hs200_error_recovery.py` |
| 精确产物安装/回退 | `tools/validation/update_t736_storage_v23.py`、`test_update_t736_storage_v23.py` |
| 只读时钟寄存器收集 | `tools/validation/t736-emmc-clocks-readonly-v4.c` |
| HS400 v24驱动 | `boards/t736_mmc_hs400_draft.c`、`t736_mmc_hs400_policy_draft.h`、`t736_mmc_hs400_tuning_draft.h`；默认关闭，100/200 MHz有界阶段通过 |
| v25计时诊断草稿 | `boards/t736_mmc_hs400_timing_draft.c`、`t736_mmc_timing_stats_draft.h`；保留v24的DMA/轮询/复位/64 KiB限制，默认关闭，无性能优化结论 |
| 固定读取负载与计时采集 | `tools/validation/mmc_read_cost_probe.py`、`mmc_timing_collect.py`；原始快照、计数下界、丢样本与CPU端点局限可见，生命周期最大值不相减 |
| v25升级草稿 | `tools/validation/update_t736_storage_v25.py`；8项最终构建绑定仍为空，拒绝安装；14项离线故障注入通过 |
| GTK回调与帧时间诊断 | `tools/validation/gtk_ui_frame_probe_steady.py`、`gtk_ui_frame_timings_probe.py` |
| EGL独立小窗口反馈 | `tools/validation/egl_x11_sync_feedback_probe.c`；窗口级交换计数，不替代GNOME或物理显示验证 |
| Mutter反馈候选 | `patches/mutter/0001-egl-xlib-observe-sbc-completion.patch`；17项源码提取mock检查及独立复核通过，完整构建/加载与实板验证另行记录，本检查点未上板 |
| source-v11整合草稿 | `tools/build_source_rootfs_v11.sh`、`build_source_uboot_source_v11.sh`、`pack_t736_source_v11.py`、`validation/source_v11_plan.py`；静态复核通过，仍pending，未构建/烧录 |
| 公开备份准备/验证 | `tools/prepare_source_backup.py`；不会push或烧录 |

EGL v1与v2分别保存源码/二进制哈希，构建记录为 `evidence/egl-x11-sync-feedback-probe-build-20261116.json`、`evidence/egl-x11-sync-feedback-probe-v2-build-20261116.json`。v2两次原始测试各提交60次、122次查询均成功、SBC到60、首尾两帧像素回读通过，反馈速率60/1。运行版本来自接续记录；原始JSON没有嵌入可执行文件哈希，也没有额外上传/run-hash日志，因此没有宣称独立加密绑定。UST/MSC不能直接当成当前提交帧的显示时刻，小窗口成功不替代Mutter候选部署或GNOME流畅度验收。

本检查点的v25状态仅为源码复核、实际C聚合ASan/UBSan、ARM64编译检查、实际C输出与Python解析契约通过；完整内核/PVR包构建仍在进行，未上板。Mutter反馈候选也未上板。存储/GPU阶段结束后，重启GDM窗口新增206.306/206.514秒两条fbdev timeout，见 `evidence/storage-v24-x11-gnome-health-20261116.log`；之后有限Settings/GTK/X11测试未新增，不代表根因已解决。GNOME交互流畅度、最终整合镜像及其冷启动验收仍未完成。厂家DDR库启动成功没有解决纯源码ZCal的PU/PD问题。历史source-v6完整镜像验收与本次增量阶段分别保留。

## 公开范围与核对

公开源码、配置、补丁、测试、构建脚本和允许的文本证据。完整raw/rootfs/内核包、厂家库及训练固件、认证与隧道日志、凭据、个人截图、私有home备份均不发布。唯一保留的历史PNG例外为此前已发布的source-v6控制台framebuffer图，不新增桌面截图。

`backup-manifest.json`逐文件记录公开副本SHA256、大小与原工作区SHA256。日志可做LF/ANSI清理，原始构建与实板证据哈希不因此改写；manifest分别保存规范化副本哈希。准备后校验全覆盖、JSON解析、Python编译、秘密扫描和私有路径排除，最终提交与push由主任务完成。
