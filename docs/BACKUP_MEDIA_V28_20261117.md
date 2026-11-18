# T736 media-v28 启动包与实板验收备份

日期：2026-11-17（Asia/Shanghai）

这份记录把本轮已经完成的 media-v28 启动链和实板结果固定下来，作为源码公开备份的入口。它不发布完整 eMMC raw、内核 deb、厂家 DDR 库、训练固件、登录凭据或控制服务会话。

## 已验收结果

| 项目 | 结果 | 记录 |
|---|---|---|
| 启动包 | 已写入 eMMC LBA 24576，长度 637 sectors；板上直接读回 SHA256 一致 | `evidence/media-v28-final-acceptance-20261117.json`、`evidence/boot-package-readback-20261117.log` |
| 冷启动 | 通过，进入 Armbian，内核 `6.18.63-t736-media-v28` | `evidence/media-v28-final-acceptance-20261117.json` |
| eMMC | HS400、8-bit、1.8 V、200 MHz；HS200/HS400 调谐窗口已复核 | 同上 |
| GPU/PVR | PVR BVNC `36.56.104.183` 注册，GPU 400 MHz，900000 uV | 同上 |
| 视频引擎 | `VE_DEC` 400 MHz，PPU 开启，DEC0/DEC1 bypass 已验证 | 同上 |
| HDMI | 冷启动完成 2560x1440@60 配置 | 同上 |
| 网口 | `end0` 仍为 100 Mb/s 全双工 | 同上 |
| Wi-Fi/蓝牙 | 当前 DTB/内核没有枚举设备，仍待补齐 | 同上 |

启动包文件仍保存在构建工作区：
`mainline/output/t736-source-boot-package-media-v28.bin`。

启动包 SHA256 为
`4742e0d36c2f35c9b58e4b70fad657fb7d265248451c3f618b46ab07b84385f0`；
对应布局和载荷哈希见 `evidence/t736-source-boot-package-media-v28.manifest.json`。公开仓库只记录 manifest 与验收文本，不把二进制启动包提交到源码备份。

公开的验收 JSON 已移除串口会话标识和登录凭据；本地原始记录仍保留在工作区。

## 源码入口

- `boards/candidates/media-v28-400mhz/`：VE 400 MHz、电源只读回读和候选设备树修改。
- `tools/build_media_v28_candidate.py`：构建 media-v28 候选内核与 PVR 绑定产物。
- `tools/validation/update_t736_media_v28.py`：版本化安装、回退和启动别名保护检查。
- `tools/pack_t736_source_boot.py`：启动包布局、载荷地址和 CRC 校验。
- `config/boot-policy.json`：授权 DDR 库边界及启动链策略。

## 公开范围与限制

源码、配置、补丁、测试和脱敏文本证据随本仓库备份；完整镜像、deb/ko、厂家二进制、原始登录或隧道记录留在私有构建机。该记录只证明本轮指定板卡和指定启动包的冷启动结果，不把 100 Mb/s 网口或未枚举的 Wi-Fi/蓝牙写成已完成，也不替代纯源码 LPDDR4 ZCal 的独立实现。\n