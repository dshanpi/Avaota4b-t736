# T736 Cedar 2.0 用户态桥接候选

2026-11-16 已实际交叉编译 `gst-omx 1.20.1`（Tina 的 19 个补丁）和 `vdecoderdemo`，并制作隔离运行包。v2 在实板 Linux storage-v16、GStreamer 1.26.2 上成功加载并注册 `omxh264dec`：rank=0、AFBC 默认 false，枚举出 YV12/NV21/DMABuf 输出。没有在板卡上执行解码，也没有实现 Firefox 硬解接口。实测见 [v2 插件检查](../evidence/storage-v16-cedar-v2-inspect-20261116.log)。

## 重建与制品

构建入口为 [build_cedar_bridge.py](../tools/build_cedar_bridge.py)。在构建机执行：

```sh
python3 tools/build_cedar_bridge.py --work /home/ubuntu/t736-mainline/build/cedar-bridge-new
```

输出目录必须不存在，且不能位于 Tina 内。脚本只读 Tina 源包、补丁和 SDK，编译到独立目录；不修改系统库、板卡或现有内核。

当前构建包位于构建机 `build/cedar-bridge-package-v2/t736-cedar-bridge-v2.tar.gz`，本地副本为 `output/t736-cedar-bridge-v2.tar.gz`。大小 1,103,293 字节，SHA256 为 `39199fe332a5e2320e308ea8a5c1f0039bce5e277b892ab71af5f6bccc8fc226`。包包含厂家解码库，仅作本地/构建机实验备份，不随公开源码仓库上传。v1 原件和失败证据保留：原来的裸 `core-name=libOmxCore.so` 被插件的 `g_file_test` 拒绝，得到 0 features；v2 的 wrapper 在私有临时配置中生成正确绝对路径。

源码来源、19 个补丁、编译器及全部 32 个文件的哈希见 [v2 制品清单](../evidence/cedar-bridge-v2-manifest-20261116.json)，[v2 构建记录](../evidence/cedar-bridge-v2-build-20261116.json)和[v1 静态 ABI 核查](../evidence/cedar-bridge-v1-abi-20261116.json)分别保存构建结果和静态检查。

选择 `libcedarc-dev_2.0.0_arm64.deb`，SHA256 `229b69699184bbf7b0bade1540cc06cda50e20ccb5cd6da9f8c365d1bc1023d0`。Tina 打包脚本明确将它用于 A733/T736；3.0 包指向 A537/A333，未采用。新编译的插件和 demo 最高需求为 `GLIBC_2.17`；整套 Cedar 依赖最高为 `GLIBC_2.30`。Tina 随附 Debian 13 的 GStreamer 1.26.2 包可解析插件的全部 202 个 `gst_*` 符号，`GstVideoInfo` 声明与 SDK 一致。板上插件加载已通过；真实解码的帧协商仍待验证。

## 隔离运行方式

解压后先仅检查插件注册：

```sh
./t736-cedar-bridge-v2/bin/cedar-env gst-inspect-1.0 omxh264dec
```

wrapper 仅为当前进程设置私有库路径、插件路径、临时配置和 registry。配置中的 OpenMAX core 路径由解压位置生成，支持目录含空格，不写入全局配置。H.264 元素的 rank 为 0，需显式选择。`libvideoengine.so` 从进程映射中的 `libvdecoder.so` 路径查找同目录 codec 插件；对应解码库已放在一起。OpenMAX core 通过动态加载查找 `libOmxVdec.so`。

Cedar 本身仍读取 `/etc/cedarc.conf`，包内 `config/cedarc.conf.reference` 不会自动加载。wrapper 不修改全局配置；若板上已有此文件，验收时应记录其内容和哈希。缺少文件时数字配置回落到库内默认值。

VE 内核前置验收后，第一项显式解码采用短 H.264 elementary stream、`afbc=false`、`fakesink` 和超时：

```sh
./t736-cedar-bridge-v2/bin/cedar-env timeout 30 gst-launch-1.0 -e \
    filesrc location=sample.h264 ! h264parse ! omxh264dec afbc=false ! fakesink sync=false
```

这项命令只是后续测试入口，当前没有执行记录。成功注册元素不等于硬件解码；真实验收需统计输出帧，使用 demo 导出线性 YUV，并与软件解码结果比较。AFBC、H.265、VP9、零复制显示和浏览器接口分别验收。Firefox 不直接消费这个 Cedar/OpenMAX 插件，不能因安装它而宣称浏览器硬解可用。

## Linux 6.18 前置条件

`linux-storage-v16/.config` 当前关闭 `CONFIG_DMABUF_HEAPS` 和 `CONFIG_IOMMU_SUPPORT`；启用 CMA 及 256 MiB CMA 区不能替代这两项。实际 Cedar 2.0 二进制 `CdcIonGetMemType()` 固定返回 `MEMORY_IOMMU=1`，`CdcIonOpen()` 进入 `/dev/dma_heap/system` 分支。仅凭库里存在 `/dev/dma_heap/reserved` 字符串，不能推断它会使用物理连续堆。

下表来自 Tina `bsp/configs/linux-6.6/sun60iw2p1.dtsi`、`ccu-sun60iw2.c` 和 `pck600_domains.c`，是移植依据，不是可直接执行的寄存器写入脚本：

| 项目 | T736 解码端要求 |
|---|---|
| VE | `0x01c0e000` 长度 `0x3000`，sysconfig `0x03000000` 长度 `0x10`；SPI 49 |
| PLL | CCU `0x02002000 + 0x0220` 为 PLL_VE0；LDO/EN/LOCK_EN/LOCK/OUTPUT 位 30/31/29/28/27 |
| 模块时钟 | CCU `+0x0a88`：M 位 `[4:0]`、mux `[26:24]`、gate 位 31；PLL_VE0 对应 mux 1，mux 0 是 PLL_VE1 |
| 时钟门控 | AHB `+0x05c0` 位 0；decoder MBUS master `+0x05e0` 位 14；MBUS `+0x05e4` 位 18；bus `+0x0a8c` 位 2 |
| 复位 | CCU `+0x0a8c` 位 18，`RST_BUS_VE_DEC` |
| 电源域 | PCK `0x07060000` 域 2，即 `0x07062000`；control `+0`、status `+8`，低四位 on=8 |
| PCK 延时 | 域内 `+0x170=0x1f1f1f`、`+0x174=0x1f1f`、`+0xc00=0x08080808`、`+0xc04=0x808`、`+0xc10=8`；还需 R_PPU 时钟 |
| IOMMU | `allwinner,iommu-v20`，`0x03900000` 长度 `0x20000`，SPI 170/171；VE master 6 和 companion master 7 |
| IOMMU 时钟/复位 | CCU `+0x058c`、`+0x05b4` 各 gate 位 2/1/0，reset 位 16；需双实例的翻译和失效操作 |
| NSI | 解码 master 7/8，NSI 资源 `0x02020000/0x10000` 与 `0x0a030000/0x20000` |

三个共享门控寄存器使用 donor 的 `SUNXI_CCU_GATE_WITH_KEY` 路径，key 分别为 `0x010000ff`、`0x41055800`、`0x00040302`。必须先核对共享位和现有 GPU/display/storage provider 的锁与所有权，不能将整字写入当作只修改 VE。供电序列必须传播超时和读回错误：donor 的 `sunxi_do_pmu_set_power_domain()` 只打印超时，而 `sunxi_pd_power()` 仍返回 0，这个行为不能原样移植。

IOMMU 不能通过套用 H6 compatible 解决。需按 T736 v20 双实例及 master 6/7 结构移植，限制私有 ioctl 返回的地址在 32 位范围内，验证整个 dma-buf 的 DMA 映射连续、长度完整，并成对执行 attach/map/unmap/detach。`system` heap 可能分散分配物理页面，不能将首个 scatterlist 地址当成整块连续内存。

首次媒体内核应使用独立源码/输出目录，保留当前 v16。除 Linux 6.18 的 `.remove` 返回类型和 `MODULE_IMPORT_NS("DMA_BUF")` 调整，还需修复 donor 的 probe/runtime-PM 错误回退、mmap 资源边界、debugfs `proc_info_len` 和 channel 边界，以及任意用户地址 cache-flush 的访问验证，再开放设备节点。完整 donor 入口与已发现 API 差异见 [原始移植审计](../evidence/source-v9-media-port-audit-20261116.json)。

## media-v17 编译阶段

构建机现已建立独立 `sources/work/linux-media-v17` 和 `build/linux-media-v17`，由 [准备脚本](../tools/prepare_media_kernel.py) 从 v16 复制并导入准确的 IOMMU-v20 donor。[API 适配脚本](../tools/port_media_iommu.py)及[对应补丁](../patches/linux/media-v17-iommu-api.patch)迁移 Linux 6.18 的分页、分配、attach 和 remove 回调，并约束 32 位 IOVA、修正页目录跨边界分配和 unmap 错误长度处理。

stage1 的 `Image`、内核模块和原生 system dma-heap 已实际编译、链接通过，版本 `6.18.63-t736-media-v17`。Image 为 11,866,120 字节，SHA256 `ebe25f4e205ef0cb6fff0eb415e271bf642d7a1cf9577d03c72e66a72962e195`；原 v16 Image 哈希核对未变。见 [准备清单](../evidence/media-v17-prepare-20261116.json)和[stage1 构建记录](../evidence/media-v17-build-stage1-20261116.json)。构建仍有 donor/既有源码警告，不是零警告验收。

**stage1 不可部署。** 没有加入或启用新的媒体设备树节点，VE clock/reset/power provider 尚未实现。独立复核已指出 donor 先使 TLB/PTW 失效再清页表、L2 释放早于确认完成的撤销顺序问题，以及页表 DMA ownership、全局 domain/group 生命周期、跨 4 GiB PTE 编码和错误回退缺口。现有 v2 donor 的失效流程是置位、等待 10 微秒、清位；手册只有 IOMMU 概述，没有给出完成位定义，不能把另一个 v3 donor 的自清语义直接套用。编译成功不代表这些 DMA 语义已验证。

stage2 在保存 stage1 源码快照后，仅修复物理地址跨 4 GiB 时的页表编码：对每一页分别编码高位，避免在已编码的 32 位 descriptor 上直接累加 4 KiB。对实际 C 函数的 host 测试中，stage1 的 4/8 GiB 两项失败；stage2 的四项 L1/PTE 边界检查全通过，[独立审查](../evidence/media-v17-stage2-pte-independent-review-20261116.json)确认该项修复。见[地址补丁](../patches/linux/media-v17-pte-address.patch)、[stage1 测试日志](../evidence/build/media-v17-stage1-pgtable-test-20261116.log)、[stage2 测试日志](../evidence/build/media-v17-stage2-pgtable-test-20261116.log)和[stage1 独立审查](../evidence/media-v17-stage1-independent-review-20261116.json)。

当前 `build/linux-media-v17/arch/arm64/boot/Image` 已由 stage2 重链，大小仍为 11,866,120 字节，SHA256 为 `0f7bf076642bd32dd18e275c1ecc03d3cc9b7de8f8ece6f54631b98bfe2ca730`，见[stage2 构建记录](../evidence/media-v17-build-stage2-20261116.json)和[完整构建日志](../evidence/build/media-v17-stage2-image-build-20261116.log)。其余 DMA、撤销、生命周期、master 6/7 和提供者问题仍未解决，stage2 同样不可部署；host 算术测试不验证 DMA 可见性或硬件完成。

复核另发现 donor 的 master 名称查询误将 `strcasecmp` 非零当作匹配，IRQ 故障路径还可能索引超出每实例六个 master 的边界；需要与 master 6/7 所有权一起修正，不能在故障处理时引入越界访问。
