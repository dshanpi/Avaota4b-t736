# 启动链与外部固件

当前验证链路：

```text
厂家 Boot0：DDR 与早期存储初始化
  → 厂家 BL31 / OP-TEE / SCP
  → 厂家 U-Boot 2018.07（32 位）
  → boot 分区 Android 容器中的 U-Boot 2026.10（64 位）
  → eMMC ext4 中的 Linux 6.18.63 + DTB + initrd
  → Armbian Trixie
```

新 U-Boot 的装载地址为 `0x52000000`。
已观察到的 BL31 地址为 `0x48000000`，OP-TEE 地址为 `0x48600000`。
主线 DTS 保留了当前固件占用区域；保留范围不能直接推广到其他 PCB 或固件。

`handoff/` 候选保留本板 1600 字节 spare header 数据，并使用本板厂家 DTB。
候选调用厂家 SMC `0x8000ff10` 启动 SCP，再通过 `0x8000ff04` 进入 64 位 U-Boot。
候选尚未验证，不能据此直接替换持久 Boot1。

## FES 验证包

`tools/pack_boot_validation.py` 从本板恢复包读取原始 loader、Boot0、Boot1 和分区元数据。
脚本替换 boot，可选加入 rootfs，并更新对应组件校验和。
验证使用 LYNX `partition` 模式、`verify=true`、`postFlashAction=none`。
这些包不是完整主线启动链替换镜像。

`partition` 模式仍可能更新 GPT/厂家 MBR，不能保证其他分区不受影响。
v3 曾因错误参考布局损坏原根系统，现已按用户授权恢复。
打包工具强制校验已实测 GPT/MBR 哈希和全部分区尺寸；参考 Boot0/FES 哈希相同不足以证明布局相同。
常规内核更新使用已启动系统中的文件/软件包安装，并保留上一版启动文件。

FEL/FES 的逻辑地址与 eMMC 物理 LBA 不能混用。
以下为本次实板 GPT，单位为 512 字节扇区：

| 分区 | 起始 LBA | 扇区数 |
|---|---:|---:|
| boot-resource | 73728 | 65536 |
| env | 139264 | 32768 |
| boot | 172032 | 196608 |
| rootfs | 368640 | 15239680 |
| private | 15608320 | 32768 |
| UDISK | 15641088 | 106501599 |

当前 root PARTUUID 为 `a0085546-4166-744a-a353-fca9272b8e48`。
布局和 PARTUUID 均是本板验证输入，不能用于未经核对的其他设备。

USB FES 下载功能由厂家 RAM loader 提供；这不代表主线 Linux 的 USB 驱动已经可用。
厂家 DDR、BL31、SCP、OP-TEE、loader、spare header 和 DTB 都需要外部取得。
本仓库仅记录用途、来源和哈希，不包含这些二进制。
