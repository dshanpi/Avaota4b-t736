# 构建与恢复源码

这是开发过程的源码备份。内核与 U-Boot 可以从锁定官方源码和补丁重建。
完整 FES 镜像仍依赖本板厂家固件、SDK 打包工具及官方 Armbian rootfs 输入。
不能仅从本仓库生成无厂家依赖的完整固件。

## 2026-11-14 固定 LPDDR4x 与已验证 HDMI 控制台

当前正常启动链只保留用户授权的固定 DDR 库及内嵌训练程序；SPL、交接、TF-A、U-Boot 均为源码构建。厂家 FES loader 仅用于 RAM 烧录。`source-v6` 已通过三次无人干预 eMMC 启动、HDMI tty1 帧缓冲、内存及文件系统校验，沿用 v4 的独立外围供电；RTL8211F DHCP/100 Mbps 链路和 USB 枚举可用，千兆尚未通过。最终验收见 `evidence/source-v6-emmc-acceptance-20261114.json`。

在已有本板 Linux 基础工作树上，仅首次导入 GPL 显示源码时运行 `python3 tools/prepare_t736_display_port.py`。该工具不会覆盖已有显示工作树，导入清单记录每个原始文件的 SHA256。创建 `build/linux-hdmi-v13`，复制 `userpatches/linux-t736-hdmi-v13.config` 为其 `.config` 后，执行：

```sh
sh tools/build_display_kernel.sh 13
sudo -n unshare --mount sh tools/build_source_rootfs_v6.sh
python3 tools/pack_t736_source_boot.py --variant v6
```

rootfs 脚本以 `output/rootfs-armbian-source-v1.ext4` 为只读输入，创建独立 v6 文件；保留凭据和 `/boot/t736-source-v4-recovery` 内核恢复副本。包版本为 `6.18.63-13`，release 为 `6.18.63-t736-hdmi-v13`；源码 U-Boot 的固定加载路径通过符号链接指向新内核/initrd。打包器继续使用已实板核对的 v4 SPL 和相同源码交接、TF-A、U-Boot，并拒绝覆盖已有 raw。当前 raw SHA256 为 `5e36cd917e6c49c51e5946faecd838394dce0161dd0926c095b848236729d377`。每次整盘写入后须重新迁移 GPT 备份到物理介质尾部，并验证分区及 PARTUUID 不变。

兼容补丁可用 `python3 tools/export_display_compat.py` 从当前显示工作树重新导出，并在临时目录重放检查，当前 264 个源码/头文件完全一致。补丁包含 framebuffer DMA 绘图和读写回调，以及首次 modeset 完成后延后注册 framebuffer 的修复。最终构建日志为 `evidence/display-kernel-v13-fbops-build-20261114.log`；无 `fbops` 的 v13 日志是尚未补齐绘图回调的中间版本。显示支持只导入 GPL 源文件，不含 HDCP 可执行固件；HDCP 和 CEC 在板级设备树中关闭。新增时钟驱动只配置独立 DE PLL，不修改共享 DDR、PERI 或参考 PLL。

## 当前源码 U-Boot 的归档与重建入口

`patches/u-boot/t736-source-boot.patch` 是相对锁定官方提交的完整板级差异，包含当前源码启动路线的修改；不要与历史 `userpatches/u-boot/` 补丁叠加。`config/u-boot-source-v6.config` 是实际部署配置，`CONFIG_BOOTDELAY=3`。补丁已在官方归档上重放并与构建工作树逐字节核对，现有 `u-boot.bin` 与已部署载荷 SHA256 均为 `c9b05a7d207781a730c5e12aaae2bdfaf113194f8a3ea4b1e625d330e3f4018b`。

准备审计记录所指的 `sources/archives/u-boot-upstream.tar.gz` 后，执行 `sh tools/build_source_uboot.sh`。脚本验证归档、补丁和配置哈希，输出到新建的 `build/source-uboot-v6-rebuild`，不会覆盖打包器锁定的原产物。本次仅核对源和已有产物、检查脚本语法，没有重新编译；时间戳与工具链可能改变候选哈希。记录见 `evidence/source-uboot-backup-audit-20261115.json`。

## Linux 构建机

实测环境为 Ubuntu 24.04、GCC 13.3 ARM64 交叉工具链、GNU binutils 2.42。
使用区分大小写的 Linux 文件系统。Windows Linux checkout 曾出现文件名大小写碰撞。

原有构建脚本保留了实际使用的路径：

- 工程：`/home/ubuntu/t736-mainline`
- SDK：`/home/ubuntu/t736-aiot/tina`，只读使用。

恢复到新机器时，将本仓库检出到上述独立工程目录，或先审阅并调整脚本路径。
不要覆盖已经存在且有修改的源码工作副本。

所需开发包包括：

```sh
sudo apt-get install git build-essential gcc-aarch64-linux-gnu libc6-dev-arm64-cross \
  bc bison flex libssl-dev libelf-dev libdw-dev libgnutls28-dev libncurses-dev \
  python3 python3-dev python3-setuptools swig device-tree-compiler \
  debhelper fakeroot rsync cpio zstd xz-utils patch e2fsprogs \
  qemu-user-static binfmt-support
```

## 取得官方源码并应用本板补丁

在工程根目录执行。此步骤要求新建或未修改的上游源码 checkout。
准备工具遇到不同远端、不同 HEAD 或已有修改时会停止，不会 reset/clean。

```sh
python3 tools/prepare_sources.py --check
python3 tools/prepare_sources.py --fetch --destination sources/work
git -C sources/work/u-boot apply --check "$PWD/userpatches/u-boot/u-boot-t736-mainline/0001-t736-uart-memory-mmc.patch"
git -C sources/work/linux apply --check "$PWD/userpatches/kernel/t736-mainline-6.18/0001-t736-minimal-board.patch"
git -C sources/work/u-boot apply "$PWD/userpatches/u-boot/u-boot-t736-mainline/0001-t736-uart-memory-mmc.patch"
git -C sources/work/linux apply "$PWD/userpatches/kernel/t736-mainline-6.18/0001-t736-minimal-board.patch"
```

官方提交记录在 `config/sources.lock.json`。应用补丁后，不要再次对修改后的 checkout 运行源码准备工具。
`tools/prepare_linux.sh` 和 `tools/build_minimal_kernel.sh` 保留早期调试流程；它们不是当前 Armbian 构建入口。

## U-Boot 与 Linux

```sh
mkdir -p build/u-boot-armbian build/linux-armbian output evidence/build
make -C sources/work/u-boot O="$PWD/build/u-boot-armbian" \
  ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- t736_bringup_defconfig
make -C sources/work/u-boot O="$PWD/build/u-boot-armbian" \
  ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- -j8
cp userpatches/linux-t736-mainline-current.config build/linux-armbian/.config
sh tools/build_armbian_kernel.sh
DEB_BUILD_PROFILES=pkg.linux-upstream.nokernelheaders \
  make -C sources/work/linux O="$PWD/build/linux-armbian" \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KBUILD_DEBARCH=arm64 \
  KDEB_PKGVERSION=6.18.63-4 -j8 bindeb-pkg
```

构建脚本在新输出目录中会使用仓库内的种子配置，并检查 olddefconfig 后的必需符号。
构建输出为 `u-boot.bin`、Image、DTB、modules 和内核 Debian 包。
Debian 包中的 `vmlinuz` 是 gzip 压缩 Image。当前 U-Boot 补丁为它设置了解压参数。
构建时间、工具链及 Debian 包依赖可能变化，因此不承诺重建产物与历史哈希逐字节一致。

## Armbian BSP 与 rootfs

先通过 QEMU/binfmt 确认构建机可以执行 ARM64 用户程序。
在工程根目录记录 userpatches 的绝对路径，然后运行官方框架：

```sh
export USERPATCHES_PATH="$PWD/userpatches"
cd sources/work/armbian-build
./compile.sh rootfs ARCH=arm64 RELEASE=trixie BUILD_MINIMAL=yes BUILD_DESKTOP=no SHARE_LOG=no
./compile.sh armbian-bsp-cli BOARD=t736-mainline BRANCH=current RELEASE=trixie \
  BUILD_MINIMAL=yes BUILD_DESKTOP=no EXPERT=yes SHARE_LOG=no USERPATCHES_PATH="$USERPATCHES_PATH"
```

当次采用的官方 rootfs 文件名及 SHA256 见 `config/external-inputs.json`。
新运行可能取得不同月份的缓存；使用历史缓存前须核对哈希。
返回工程根目录，在新的空目录中展开已经核对的 tar.zst：

```sh
mkdir -p build/armbian-rootfs
sudo tar --numeric-owner --xattrs --acls -I zstd -xf /path/to/verified-rootfs.tar.zst \
  -C build/armbian-rootfs
sudo env KERNEL_PACKAGE_REVISION=4 unshare --mount --fork sh tools/build_armbian_rootfs.sh
```

`/path/to/verified-rootfs.tar.zst` 必须替换为实际文件，staging 目录不能混入其他系统内容。
脚本需要前述内核包、Armbian BSP 包以及 SDK 的 `img2simg`。
脚本创建与已验证 rootfs 分区等长的 ext4 和 Android sparse 镜像。
初始 root 密码为 Armbian 调试默认值 `1234`，首次登录强制更改。
脚本会安装软件包，包仓库尚未做完整快照锁定。

Armbian family 的 `write_uboot_platform()` 仍会停止普通整盘镜像写入流程。
这是当前持久启动布局尚未验证的明确状态，不要把它解释为已完成自动整盘构建。

## 临时 initramfs 与 FES 容器

临时 initramfs 使用官方 BusyBox 1.37.0；源码 URL、SHA256 和 `.config` 已保存。
使用 `CROSS_COMPILE=aarch64-linux-gnu-` 在 `build/busybox` 构建静态 BusyBox。
`tools/build_bringup_payloads.sh` 还需要主机工具 `build/linux-bringup/usr/gen_init_cpio`。
该工具可从锁定 Linux 源码的 `usr/gen_init_cpio.c` 用宿主 C 编译器生成。
临时 BusyBox shell 仅用于硬件验证，不是最终 Armbian rootfs。

当次 Android 启动容器的构建参数为：

```sh
/home/ubuntu/t736-aiot/tina/tools/pack/pctools/linux/android/mkbootimg \
  --kernel build/u-boot-armbian/u-boot.bin --dtb build/vendor-kernel.dtb \
  --base 0x40000000 --kernel_offset 0x12000000 \
  --ramdisk_offset 0x1000000 --dtb_offset 0x1000000 \
  --pagesize 2048 --header_version 2 --board t736-uboot-probe \
  -o output/boot-uboot-probe.img
```

`build/vendor-kernel.dtb` 来自本板恢复包，不在仓库内。
历史 boot 验证载荷在该容器的 8 MiB、40 MiB、42 MiB 偏移分别附加 Image、主线 DTB、临时 initramfs。
具体长度、哈希、CRC 和实板读取命令见 `evidence/boot-mainline-probe-v2.payloads.json`。
新产物需要重新检查边界、生成清单和校验值，不能直接复用旧长度。

准备好完整 boot 验证载荷后，可用 `tools/pack_boot_validation.py --help` 查看 IMAGEWTY 封装参数。
该工具需要本板恢复包；可选 `--rootfs` 加入已构建 sparse rootfs。
该工具仅生成文件，不连接或烧写设备。

## 交接候选

`tools/build_handoff.sh` 需要 SDK ARM32 工具链、厂家 spare header、厂家 DTB 和新 U-Boot。
这些外部文件的哈希见 `evidence/t736-handoff-ram-v1.json`。
交接候选尚未实板验证，构建成功不能作为直接替换 Boot1 的依据。
