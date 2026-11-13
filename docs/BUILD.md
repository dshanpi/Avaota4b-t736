# 构建与恢复源码

这是开发过程的源码备份。内核与 U-Boot 可以从锁定官方源码和补丁重建。
完整 FES 镜像仍依赖本板厂家固件、SDK 打包工具及官方 Armbian rootfs 输入。
不能仅从本仓库生成无厂家依赖的完整固件。

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
  KDEB_PKGVERSION=6.18.63-2 -j8 bindeb-pkg
```

种子内核配置必须先复制，避免依赖未随备份上传的旧 `build/linux-bringup/.config`。
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
sudo env KERNEL_PACKAGE_REVISION=2 unshare --mount --fork sh tools/build_armbian_rootfs.sh
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
