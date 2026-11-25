# BMW CIC CarLife

为 BMW CIC HIGH 验证百度 CarLife 有线投屏的实验项目。第一阶段是 Android 手机作为发送端、CIC 作为 USB Host 接收 CarLife 的 H.264 画面。

## CIC 的接入点

[bmw-cic-androidauto 的源码](https://github.com/josemalm32/bmw-cic-androidauto/tree/6d8d5754e3e580e83143b94cac9a0b1096ef71a0) 把 Android Auto 协议、HMI 和视频解码分开。HmiMain 加载插件和 `libcicaa.so`。协议模块把 AVC 配置和样本送入共享环，独立的 `cicaalive` 再经 `dspipc` 把视频交给 TI DM642。插件使用 CIC 原生 DVD 视频源和合成层显示。固件调用依赖 HmiMain 版本签名。上游记录了 CIC HIGH C1A 和部分 C16A 的测试，F02 仍需要按实际 HmiMain 构建号和签名核对。详见 [上游架构](https://github.com/josemalm32/bmw-cic-androidauto/blob/6d8d5754e3e580e83143b94cac9a0b1096ef71a0/docs/ARQUITECTURA.md)。

本项目实现 CarLife AOA 消息分帧、版本交换、设备信息、视频初始化和 H.264 接收。先请求 640×480、30 fps，接受同尺寸且不超过 30 fps 的协商结果，再检查实际 SPS 是否为 640×480、逐行 Baseline、Level ≤ 3.1。这样可以在接入 DM642 前排除常见的 720p 和更高规格。首帧要求有效的 SPS、PPS 和 IDR。QNX 适配层把通过检查的 Annex B 流送给上游 AVC 适配器，并处理上游读端的 IDR 请求。

| 组件 | 入口 |
| --- | --- |
| 独立 CarLife 车机协议 | `src/carlife.c` |
| H.264 SPS 与访问单元检查 | `src/avc.c` |
| CIC 插件 ABI、io-usb 和视频桥接 | `src/qnx_bridge.c` |
| QNX SH4 模块构建 | `tools/build_qnx.py` |
| Windows 或 POSIX 离线回放与验证 | `src/replay.c`、`tests/replay_test.py` |

## 本机验证

从项目根目录，在 Visual Studio Developer PowerShell 里编译。测试需要带 `libx264` 的 FFmpeg。

```powershell
New-Item -ItemType Directory -Force out | Out-Null
Push-Location out
cl /nologo /std:c11 /W4 /WX /O2 /D_CRT_SECURE_NO_WARNINGS ..\src\carlife.c ..\src\avc.c ..\src\replay.c /Fe:cic-carlife-replay.exe
Pop-Location
python tests\replay_test.py out\cic-carlife-replay.exe --ffmpeg ffmpeg
```

回放程序读取 **手机到车机** 的 AOA 字节流，写出 H.264 Annex B 和 **车机到手机** 的 AOA 字节流。两个输出必须用新路径。记录生成的包使用协议字段编码，测试把真实编码的逐帧视频装入这些包，并解码比对接收前后的逐帧校验值。

本机测试通过 1.0/2.0 回放，每个用例的 12 帧全部一致，也通过 SPS/PPS 分开到达、丢弃首个非 IDR 帧和异常包检查。适配层另以模拟 USB 调用上游真实的 AVC 队列，验证了 HMI 入口、USB 头和正文分开发送、avcC 与 length-prefixed 样本、断开释放和首帧校验值。该适配测试可在 Windows 用 Zig 的 C 编译器复跑。下方假定上游已经按 QNX 构建章节检出。

```powershell
$up = (Resolve-Path ..\bmw-cic-androidauto).Path
$src = (Resolve-Path src).Path
$test = (Resolve-Path tests\qnx_adapter_check.c).Path
$bridge = Join-Path (Resolve-Path out).Path qnx-adapter-check.exe
zig cc -target x86_64-windows-gnu -std=gnu99 -D CICAA_HOST_TEST "-I$up/src/libcicaa" "-I$up/src/common" "$src/qnx_bridge.c" "$src/carlife.c" "$src/avc.c" "$up/src/libcicaa/aa_video.c" $test -lpthread -o $bridge
python tests\replay_test.py out\cic-carlife-replay.exe --bridge $bridge --ffmpeg ffmpeg
```

## QNX 6.3 SH4 构建

准备 QNX Momentics 6.3 for Windows 的 SH4 工具链，设置 `QNX_HOST` 和 `QNX_TARGET`，然后用另一个目录里的上游 checkout 构建。

```powershell
git clone https://github.com/josemalm32/bmw-cic-androidauto.git ..\bmw-cic-androidauto
git -C ..\bmw-cic-androidauto checkout 6d8d5754e3e580e83143b94cac9a0b1096ef71a0
python tools\build_qnx.py --upstream ..\bmw-cic-androidauto
```

输出为 `out/qnx/libcicaa.so`，供同版本上游 HMI 插件和播放器在 CIC 台架评估。构建读取上游的 QNX USB 驱动和 AVC 发布代码，在临时副本中改用 CarLife 的 Android USB accessory 标识。生成的模块沿用上游文件名、导出符号和共享内存协议，因此会占用同一个插件入口。上游默认视频裁剪针对 Android Auto，需要在台架上用 640×480 画面核对裁剪和比例。项目当前交付源码和构建入口，尚无对应的安装包。

当前实现的是 1.0/2.0 的基础视频协商，QNX 适配默认试 2.0。要求加密、OEM 激活或额外鉴权的手机会停在对应协商阶段。旋钮、USB 音频、麦克风、iPhone，以及百度 CarLife+ 或厂商手机车联的兼容性都待验证。台架验收依次看 USB 重枚举、手机协议响应、640×480 和 SPS、IDR、CIC 屏幕出图、退出后恢复原车画面、断开重连。

## 参考与依赖

- [Apollo/DuerOS CarLife Vehicle Lib 存档](https://github.com/674809/carlife/tree/f9522db31a5244b89a2f3f555b2e0ad882b48674/CarLife-Vehicle-Lib) 提供协议编号和 protobuf 字段。另一份上游存档保留了 [Baidu AOA Host 实现](https://github.com/milesking/apollo-DuerOS/blob/6a07419818f46aadda34c5f55e63d4a1fc8136ef/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/connect/AOAHostSetup.java)。
- [CarLife HeadUnit Emulator 的兼容性记录](https://github.com/SadykovIV/HSD-HeadUnit-Emulator/blob/2c125d801e582b48083d9deabc642ec59d23f454/docs/CARLIFE_AUTH.md) 说明较新手机可能需要的协议分支。
- 本仓库的独立协议实现和离线回放可自行编译。QNX 模块会编入 `bmw-cic-androidauto` 的代码，适用上游 [PolyForm Noncommercial 1.0.0](https://github.com/josemalm32/bmw-cic-androidauto/blob/6d8d5754e3e580e83143b94cac9a0b1096ef71a0/LICENSE.md)，构建输出保留原作者 [Required Notice](https://github.com/josemalm32/bmw-cic-androidauto/blob/6d8d5754e3e580e83143b94cac9a0b1096ef71a0/NOTICE.md)。
