# T736 Firefox Cedar 候选适配

本目录保存 T736 私有浏览器解码适配及分阶段记录，尚未完成浏览器 1080p60 硬解验收。已验证的 Bilibili 约 30 fps 硬解基线是 media-v27 / FFmpeg v6 / backend v4 / OMX/WC v5 / protocol v1；最新实验组合是 media-v29 / FFmpeg v7 / backend v5 / OMX/WC v5 / SHM protocol v2。后者的原生硬解回放通过，但 Firefox user namespace 身份检查尚未适配，网页实际走软件回退。

2026-11-17 按用户要求停止新增性能实现和实验，转为代码整理与私有同步。本文不改变系统默认浏览器、默认内核或最终烧录镜像；厂家二进制和用户媒体不进入公开备份。

## 当前证据边界

以下保留历史基线，并单列最新候选结果。网页呈现、原生解码传输吞吐和像素正确性是不同验收项：

| 项目 | 已观察结果 | 结论 |
| --- | --- | --- |
| 本地 1080p30、180 帧样片 | VE IRQ 增加 180；浏览器 dropped 5/180；采样 RDD 约 17%、broker 约 45% CPU | 有限样片链路通过；不是零丢帧或长期稳定性证明 |
| 本地 1080p60、180 帧样片 | dropped 123/180 | 60 fps 性能未达标 |
| 私有 Bilibili 64 AU fresh 回放 | 64 帧逐帧 MD5/PTS 精确；首帧 1.914 s；close 返回 0；IRQ +64 为 root 另行报告 | 已核验原始 JSON、软件参考与输入 PTS；证明该捕获码流可用，不独自证明旧故障的唯一原因 |
| 实际 Firefox v6 Bilibili 网页，首轮，片源约 30 fps | 30.798 s 内 IRQ +923、totalVideoFrames +920、dropped +6；媒体时间 +30.688 s；rate=1、1080p；RDD 19.5%、broker 60.5% CPU | 原始计数已复算；旧 `effective_delivered_fps` 字段不能用作物理呈现帧率 |
| 同一网页，修正采样后的第二轮 | 30.958 s IRQ 窗口 +929；30.863 s 视频窗口 total +926、dropped +0，浏览器未丢帧计数速率 30.004/s；RDD 18.3%、broker 48.6% CPU | 有限时间内约 30 fps 计数通过；不是 60 fps、物理扫描输出或长期稳定验收 |
| v28 / FFmpeg v7 / backend v5 / protocol v1，1080p60 网页 | 20.489 s，total 1235、dropped 867、presented 368，末端延迟约 41.9 ms | v7 减少批量等待，但浏览器硬解 60 fps 仍未通过 |
| v29 / SHM v2 原生 180 帧回放 | 180/180 帧 MD5 与 PTS 精确；首帧 1.286 s；close=0 | 硬件解码与 SHM 像素传递正确性通过，不是网页验收 |
| 同一链路原生 540 帧、不做哈希 | 6.602932 s，81.7819 帧/s；IRQ +540、540 个 PTS 精确、close=0；有帧 PULL 平均 11.932 ms、p95 14.678 ms、最大 28.728 ms | 包含 backend/client 拷贝的原生吞吐，有 60 fps 吞吐余量；没有浏览器，不能称为实际显示帧率 |
| v29 / FFmpeg v7 / backend v5 / SHM v2，1080p60 网页 | 20.548 s，total 1233、dropped 0、mozPresented +1233；IRQ +0；RDD 149.5% CPU | **软件回退约 60 fps，通过的是软件播放；不是浏览器硬解成功** |
| v29 原生 SHM 保护测试 | seals 58 项通过（200 ms 测试超时）；实际 client 的 PID、继承锁、DONTFORK、exec/CLOEXEC 和 fd 平衡通过 | 目标内核支持已验证；不替代 Firefox namespace 认证 |
| 历史 v4 网站失败 | 64 包、0 帧后软件回退；捕获首 AU 已有两组完整 VPS/SPS/PPS | 不能把该失败归因为缺参数；记录保留，不被后续结果覆盖 |
| FFmpeg v6 软件回归 | ARM64 实际库经 QEMU，8311 项检查；真实 5.138 s 首帧等待超时后 70 帧原包回放与原生一致 | 验证 64 包预算、第 65 包保留、超时及旧回归；不替代实板验证 |

CPU 数字按 **100%=一个 CPU 核满载** 计算，不是整机百分比或纯解码基准。rVFC 回调次数受 JS 调度影响，metadata `presentedFrames` 可跳过帧编号，均不能代替实际 `mozPresentedFrames` 和 dropped 计数。`totalVideoFrames−droppedVideoFrames` 也只是浏览器计数差，不能证明物理扫描输出。IRQ、视频计数和 CPU 采样窗口并非完全同步。Python 像素回放的 `buffer.raw` 和 MD5 有额外拷贝/计算，包含哈希的耗时不得当成解码基准。完整实板状态与 `status.json` 由 root 统一整理，本 README 不覆盖历史记录。

## 当前 namespace 限制与封存状态

独立诊断 client 已在真实 RDD 记录：`uid_map` 只有 `1000 → 1000, length=1`，账户查询仍返回服务 UID 999，但 `/run/t736-cedar` 的 `st_uid` 在该 user namespace 内投影为 65534；v2 在 `directory.reject` 阶段拒绝连接。因此，即使 RDD 已加载 v2 client、末尾参数是 `rdd` 且启用变量为 1，仍会软件回退。exec 构造阶段不等于尚未进入 user namespace。

已完成的诊断源码快照归档于 `diagnostic_client_v1/`，附来源 manifest 与复现命令；原构建产物位于 `../../build/cedar-shm-client-diag-v1-20261117/`。它保留全部身份、协议、封印、只读映射和 fork 保护，只增加有界阶段日志；原生 sanitizer 回归和独立源码审查通过。它是排障产物，不是默认 launcher、性能候选或修复版。现场日志为原构建目录的 `board-firefox-constructor.log`。

拟议 **client v3（wire 仍为 protocol 2）** 只有设计和负例测试矩阵，**尚未生成实现目录、二进制或完成测试**，本轮收尾不继续实现。候选思路是严格限定 UID/GID 映射、钉住受信的不可写 `/run` 路径链并核验 peer；65534 本身不能证明原 UID 999，不能直接放行。PID namespace 下的 PID 0 与 `SO_PEERPIDFD` 试验也不构成 v3 已完成的证据。现有保护与浏览器 sandbox 保持不变。

## 架构与文件

```text
Firefox（独立候选 profile，保留 sandbox）
  RDD：私有 libavcodec 61 的 h264_cedar / hevc_cedar
    原包回放队列 → mp4toannexb BSF → cedar_client
      AF_UNIX SOCK_STREAM：压缩 AU、PTS、控制消息
      v1：像素经 socket；v2：只读 ordinary-memfd + 帧描述符
        cedar_broker（专用 UID；一个硬件 lease、一个保留 context）
          cedar_backend：appsrc → h264parse/h265parse → capsfilter
            私有 gst-omx → Allwinner 解码库 → Cedar/WC heap
          appsink → 校验 crop/stride → 普通 RAM I420
      RDD 普通 AVBufferPool → AVFrame → Firefox 图像/呈现路径
```

浏览器不会收到物理地址、DMA fd、寄存器接口或私有 heap 句柄。v2 使用每连接独立的 3,110,400 字节普通 RAM memfd，broker 保留已有 RW 映射，client 校验封印后使用 RO+DONTFORK 映射并关闭 fd。PULL 只经 socket 返回元数据；像素仍有 backend RAM→SHM→已有 AVFrame 目的区两次普通 RAM 拷贝，不是端到端零拷贝。AF_UNIX 没有 TCP Nagle/delayed-ACK 等待机制。

| 文件 | 职责 |
| --- | --- |
| `cedar_client.[ch]`、`cedar_protocol.h` | RDD exec 时预连接；同步、有界的请求/响应；长度、序号、会话和帧布局验证 |
| `cedar_broker.c` | `SO_PEERCRED` UID 检查、唯一 lease、保留 context、错误状态和普通像素传输 |
| `cedar_backend.[ch]` | GStreamer 解码、真实 crop/plane 范围检查、I420 拷贝和 idle/resume |
| `ffmpeg_cedar.c` | FFmpeg 7.1.5 内部 decoder、原生软件回退、PTS 转换、普通 RAM 帧引用 |
| `build_browser_ffmpeg.py` | 独立源码副本构建、候选版本打包和 SHA-256 记录；不安装到板卡 |
| `cedar-service`、`gstomx.conf`、`cedarc.conf` | broker 私有库、插件、组件路径和厂家日志级别 |
| `firefox-cedar` | 仅对候选进程设置 client/FFmpeg/PVR 环境 |
| `t736-cedar-experiment.service` | 专用 UID、设备白名单、CPU 亲和性；无自动启用段 |
| `shm_v2/`、`../build_cedar_shm_transport.py` | 独立 protocol v2 核心、构建与运行脚本；不覆盖本目录 v1 核心 |
| `shm_v2/70-t736-cedar.rules` | 仅给专用服务账号授予两个精确设备节点的 0600 权限 |
| `diagnostic_client_v1/` | 已构建的 constructor 诊断源码快照、来源清单与复现命令；不作默认 launcher |
| `../../tests/test_ffmpeg_cedar.c` | 实际候选库的无设备测试；导出 stub client，不连接板卡 |

## 候选版本含义

- **FFmpeg v1**：代理 decoder 与有界原包回退；生成的 codec 列表把代理放在原生 decoder 前。回退明确按 `h264` / `hevc` 名称查找，不递归选择代理。
- **v2**：无待提交 AU 且 FFmpeg 无新输入时，去掉多余的 20 ms 等待。待提交 AU 遇回压时仍内部有界重试，避免 send/receive 同时 `EAGAIN`。
- **v3**：普通 `AVBufferPool` 直接接收像素并移交引用。宽度为 64 的倍数时 Y/U/V stride 均满足 32 字节对齐；其他宽度保留原对齐拷贝。已交付帧不会被后续接收覆盖，flush/关闭 decoder 后下游引用仍有效。
- **v4**：仅向硬解 broker 发送未知 DTS（`INT64_MIN`），保持 AU 的解码顺序、原包 DTS、回放队列和 AVFrame 时间信息。实测 Firefox DTS 前移会触发 GstBaseParse 的 PTS 修正，造成 B 帧 PTS 重复；独立 180 帧 A/B 已证明未知 wire DTS 可消除该问题。
- **v5 候选**：每个 generation 首个缺配置的随机访问 AU，可从 BSF 的 Annex-B extradata 提取完整、依赖有序的 VPS/SPS/PPS 配置，再保留原 in-band 更新；完整配置 AU 不重复添加。保留首 AUD，排除 extradata 中的 VCL/SEI，flush 后重新补齐。QEMU 覆盖 Annex-B、partial 配置、AUD、flush、原 hvcC/avcC 路径，但未据此宣称真实 MSE 通过。
- **v6**：64 包/8 MiB 回放预算满时停止继续提交，并最多等待首帧约 5 s；保留尚未缓存的第 65 个原包，成功后继续提交，超时才完整转原生回放。5 s 是解码进展预算，底层 socket 的 15 s I/O 超时是另一边界。
- **v7**：将一次硬件 receive 的输出批次上限从 8 帧改为 1 帧，保留输入回压、首帧预算、flush/EOS 和软件回放语义。实际 ARM64 库基线回归 8311 项及独立延迟/回压回归 4009 项通过；不能据此认定浏览器 60 fps 已通过。
- **backend v4**：沿用 v3 的 NEON 色度拆分、crop/plane 校验及 idle/resume；新增首个错误保留、有界 GError 细节和私有 action 原始错误优先处理。不是新的内存回收机制。
- **backend v5**：普通 I420 RAM 输出改为私有引用计数缓冲池，最多缓存两个空闲块。已交付帧在下一次 pull、flush 和 decoder close 后仍有效；不改变 DMA 映射、crop/plane 校验、硬件 context 或 ABI。原生及独立 OOM/并发生命周期测试通过，实板精确回放通过。
- **OMX/WC v5**：沿用 v4 的空闲/暂停处理，新增控制路径状态快照和详细 idle 失败诊断；未据此宣称性能修复或 DMA drain。仍依赖专用 WC 内存适配和厂家 codec 库。
- **SHM transport v2**：单槽普通 memfd 传输，接收期间持续持有 client API 锁，下一请求确认上一帧已复制；v1/v2 明确互斥，不静默降级。v29 已补开其必需的 `CONFIG_ADVISE_SYSCALLS`；namespace 限制见上文。

版本号属于各组件，不可互相替代。`/opt/t736-browser-v1` 是保留的 protocol v1 安装前缀；最新 SHM 独立前缀为 `/opt/t736-cedar-transport-v2`，另引用 `/opt/t736-browser-ffmpeg-v7/lib` 和 `/opt/t736-cedar-browser-backend-v5`。实际版本以对应 manifest 和库 SHA-256 为准。

## 构建与私有安装流程

构建机使用经过记录的 FFmpeg **7.1.5** 源码及 ARM64 工具链。先在项目内执行构建，再以 SFTP 传送选定候选和 manifest；不通过临时 HTTP 服务传输，不在源码目录中嵌入登录信息。

```sh
python3 tools/browser/build_browser_ffmpeg.py \
  --source /home/ubuntu/t736-mainline/build/browser-ffmpeg-v1-20261117/FFmpeg-n7.1.5 \
  --work /home/ubuntu/t736-mainline/build/browser-ffmpeg-new-candidate \
  --bundle-name t736-browser-ffmpeg-v7
```

`--resume` 只用于本脚本已经配置的独立 work 目录；首次构建用新目录并省略此参数。版本名必须与该轮代码和记录对应，不得用新文件覆盖旧候选归档。backend 构建/无设备测试入口是 `../build_cedar_browser_backend.py`。broker/client、私有 OMX 包及 ARM64/QEMU 测试命令保存在相应构建 evidence 中；不存在自动刷板或自动发布步骤。

候选由 root 管理以下安装与检查步骤：

1. 对照 manifest 校验归档与实际库 hash，确认当前内核/DTB、私有 heap 和回退启动项。将私有文件安装在 `/opt` 的候选路径，保留旧版本；不替换 `/usr/lib` 的系统 FFmpeg。
2. 创建或检查无登录权限的 `t736-cedar` 服务用户。内核 bring-up 节点默认是 **root-only 0600**；部署 `shm_v2/70-t736-cedar.rules`，仅匹配 `misc/cedar_dev` 与 `dma_heap/t736-ve`，owner/group 均为专用服务账号、mode 0600。冷启动曾因临时 chown 未持久化导致 EACCES，厂家库随后空指针崩溃；新 `shm_v2/cedar-service` 在加载厂家库前检查两个节点的字符设备类型及读写权限并拒绝不满足条件的启动。不得开放给整个桌面组或设为 0666；节点重建后核验规则效果。
3. root 拥有候选可执行文件、库和配置。service 以 `User=t736-cedar`、`Group=t736` 运行；运行目录 `/run/t736-cedar` 为 0750，socket 由 umask 0007 限制。`cedar-service` 的参数 **1000 是被允许的 Firefox 客户端 UID**，不是 broker UID，必须与 `id -u t736` 的结果一致。
4. systemd 使用 `DevicePolicy=closed`，只放行两个 Cedar/heap 设备；无 capabilities，`NoNewPrivileges=yes`、`PrivateNetwork=yes`、`RestrictAddressFamilies=AF_UNIX`。`CPUAffinity=6 7` 把服务及其线程限定在本板两个大核；这是当前性能实验设置，不是通用 SoC 默认值。将本目录 `cedarc.conf` 安装为 root 拥有的 `/opt/t736-browser-v1/config/cedarc.conf`，unit 通过 `BindReadOnlyPaths` 仅在 broker mount namespace 挂到 `/etc/cedarc.conf`。厂家实际键名是 `[paramter]` 和 `cdc_log_level = 6`，保留 error、抑制逐帧 warning；WC5/backend 的独立错误输出仍保留。已有 retained 实例不会因修改文件自动重读；由 root 安排下一轮服务生命周期，在 v27 基线和 v28 候选中使用同一日志配置。上述两轮网页记录发生在该配置部署之前。
5. 在受控启动与内存预算确认后手动启动服务。该 unit 无 `[Install]`，`Restart=no`，不启用开机自动重启。**只换 FFmpeg 候选时不重启 broker**：退出候选 Firefox、核验 lease 已结束，再切换其私有库并重新启动候选。backend/OMX 服务替换需要单独安排生命周期与恢复步骤。
6. 使用独立 Firefox profile，并设置 `dom.ipc.forkserver.enable=false`，使 RDD 有独立 exec。此设置不能解决已经建立的 user namespace 下 UID 投影问题，当前 v2 仍会拒绝该 RDD。`firefox-cedar` 本身不创建 profile，也不设置该 pref；启动者必须明确指定。**不设置 `MOZ_DISABLE_*_SANDBOX`，不降低 sandbox level。**

保留的 protocol v1 基线运行示例（桌面用户执行，profile 必须事先准备；不是 SHM v2 验收入口）：

```sh
/opt/t736-browser-v1/bin/firefox-cedar \
  --no-remote --profile /home/t736/.mozilla/t736-cedar-candidate
```

launcher 保留本板 PVR/X11 EGL 环境：`LD_LIBRARY_PATH` 包括 `/opt/t736-img-ddk-24.2-x11/runtime-gles`，`LIBGL_DRIVERS_PATH` 指向对应 `lib/dri`，并设置 `MOZ_X11_EGL=1`。其 preload 只包含 client、私有 `libavutil.so.59` 和 `libavcodec.so.61`。service 和 `gstomx.conf` 使用 `/opt/t736-cedar-wc-v5`，当前候选配合 backend v5、选定的 GStreamer parser、独立 registry/config，清除 AFBC 默认开关。启动前清除继承的旧 client/捕获 shim preload，勿将 Firefox 和 broker 的库环境互相套用。

冻结 SHM 归档仅含 broker/client 二进制及源码、测试和 manifest；后加的 runtime 脚本、udev 规则、配置、backend/FFmpeg 库和原生测试可执行文件另行同步。归档保留构建 UID/GID 1000，root 解包必须使用 `--no-same-owner` 并规范 root 所有权及权限，避免桌面用户可修改服务加载的库。已完成归档与 build manifest 不重写；本 README 和 `shm_v2/README.md` 是后续整理文档，归档内 README 仍是原构建快照。

## 使用边界与判定方法

- 只允许一个活跃硬件会话、一个保留的 codec/尺寸 context。其他 codec/尺寸或并发打开可返回 busy/no-space，由代理在尚未输出硬件帧时回退原生。Close/断连进入 idle，后续同配置会话 resume；不会每个标签页重建 decoder。
- v27 heap 总预算 **128 MiB**，单 buffer 上限 64 MiB。导入映射退役后保留到重启，因为尚无 DMA drain 证明。正常关闭进程、FREE、flush 或停止服务均不能当作内存回收证据。历史多轮试验曾占用约 120 MiB，该数不代表 fresh 重启后的当前占用；每轮核验 live heap，不能靠反复重启服务/切换 codec 做试验。
- 相同 visible 宽高不保证相同 SPS、coded 尺寸或 DPB。重复相同 SPS 不必然重配 caps，但独立 OMX port-settings event 仍可能在同尺寸下重建输出池。真实网站/MSE 跨配置的长期内存占用稳定性尚未验证。
- 输出限定偶数尺寸、不超过 1920×1080、逐行、8-bit 4:2:0。已知不支持的 profile/pix_fmt 会早回退；真实 SPS 还由强制解析的 parser 和 capsfilter 验证。AFBC、编码、GPU/外国 dma-buf 导入、端到端零拷贝未纳入验收。
- 首帧之前保留最多 64 个原始 packet、总计最多 8 MiB；预算满后有界等待，硬解初始化或早期失败时按原包顺序交给原生 decoder。已经输出硬件帧后不会偷偷从头重放，以免重复/乱序；错误会明确返回。backend 致命错误使服务保持失败状态，不自动重新初始化。仅 FFmpeg 首帧进展超时不应与 kernel/backend poison 混为一谈。
- UID、cgroup 和 Firefox sandbox 隔离 CPU 侧权限；当前 VE DMA 仍在 bypass 路径，**没有 IOMMU DMA 隔离**。专用 UID 不能替代受限 DMA 域。
- 日志中的 `hardware frames`、RDD 标签或页面“播放正常”都不能单独证明网站使用硬解。结合实际库 maps、broker 会话提交/交付数、VE IRQ 增量、PTS 序列、输出像素验证及浏览器 dropped 统计判断。软件回退成功只证明回退可用。
- `T736_CEDAR_TRACE=1` 仅在短时排障启用；保留时间戳/计数等必要信息，避免把原始用户媒体、登录信息或私有库放进公开备份。

## 记录入口

- `../../evidence/browser-protocol-software-audit-20261117.json`
- `../../evidence/browser-ffmpeg-v1-independent-review-20261117.json`
- `../../evidence/browser-ffmpeg-v3-pool-review-20261117.json`
- `../../evidence/browser-ffmpeg-v4-build-20261117.json`
- `../../evidence/browser-ffmpeg-v6-first-frame-review-20261117.json`
- `../../evidence/browser-dts-clamp-root-cause-20261117.json`
- `../../evidence/cedar-browser-backend-v4-review-20261117.json`
- `../../evidence/cedar-wc-v5-build-20261117.json`
- `../../evidence/browser-hevc-v6-live-candidate-pending-20261117.json`（保留初始文件名，现已绑定原始文件与独立复算；有限结果不代表生产验收）
- `../../evidence/cedar-runtime-loglevel-review-20261117.json`
- `../../evidence/browser-ffmpeg-v7-bounded-batch-build-20261117.json`
- `../../evidence/browser-ffmpeg-v7-batch-independent-review-20261117.json`
- `../../evidence/cedar-browser-backend-v5-review-20261117.json`
- `../../evidence/cedar-shm-v2-build-review-20261117.json`
- `../../evidence/cedar-shm-v2-runtime-review-20261117.json`
- `../../evidence/cedar-shm-v2-client-diagnostic-build-20261117.json`
- `../../evidence/browser-v29-shm2-hardware-phase-20261117.json`
- `../../build/cedar-shm-transport-v2-20261117/native-transport540-v29-PASS.json`
- `../../build/cedar-shm-transport-v2-20261117/browser60-v29-software-fallback.json`
- `../../build/cedar-shm-client-diag-v1-20261117/board-firefox-constructor.log`

历史记录中的源码 hash 对应当时快照，后续工作文件可能已前进。记录通过与当前源码一致性需要分别核验。本目录当前不代表最终 Armbian 镜像、默认启动链或公开发布版本已完成。
