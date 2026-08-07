# OAMR Linux 适配交接文档

本文档是给后续实现 Linux 版本的模型或开发者的实施说明。目标是
**在不改变 OAMR 的路由、配对与 RTP/Opus 协议语义的前提下**，为现有
C++20/CMake 项目增加可靠的 Linux 音频设备支持。

当前项目仓库：<https://github.com/Goldppx/Open-Audio-Matrix-Router>

## 1. 目标与非目标

### 第一阶段目标

- Linux 上枚举真实录音设备和播放设备。
- 支持本机 `Source -> Sink`、一对多本机路由。
- 支持录音设备或播放回采（monitor）作为 RTP/Opus sender。
- 支持 RTP/Opus receiver 输出到选择的播放设备。
- 保持当前 Web UI、配对、遥测、路由表和远端矩阵协议兼容。
- 优先支持现代桌面 Linux：PipeWire + WirePlumber。

### 不属于第一阶段

- 创建 Linux 内核级虚拟声卡。
- 取代 PipeWire 的 session manager 或 graph manager。
- mDNS 自动发现、加密、WebRTC、NAT 穿透。
- 直接把 Windows WASAPI 设备 ID/属性复制到 Linux。

## 2. 现有项目结构与必须保留的边界

```text
include/oamr/audio/ 平台无关的 AudioBackend / AudioRoute 接口与设置结构体
src/core/        平台无关的 AudioNode / AudioPort / AudioGraph
src/gstreamer/   设备枚举、GStreamer 路由与 RTP/Opus 管线（后端内部实现）
src/pairing/     一次性码、已配对设备目录、遥测与远端路由请求
src/web/         仅回环 Web UI、路由表和矩阵 API
apps/oamr/       命令行入口
```

Web/CLI/测试只能使用 `include/oamr/audio/*` 的公共接口，通过
`oamr::audio::create_audio_backend()` 获得后端；不要引入 `src/gstreamer`
的内部头文件。不要把 Linux API 逻辑塞入 `src/web`、`src/pairing` 或
`src/core`。Linux 适配应集中在 `src/gstreamer`（或未来的独立 backend
目录），必要时新增小型平台辅助文件，例如：

```text
src/gstreamer/linux_device_backend.cpp
src/gstreamer/linux_device_backend.h
```

现有 Web/API 使用的设备选择器格式为：

```text
<gstreamer-factory>|<backend-device-id>
```

例如 Windows：

```text
wasapi2src|{device-guid}
wasapi2sink|{device-guid}
```

Linux 应继续返回相同格式，例如：

```text
pipewiresrc|<stable-node-or-device-id>
pipewiresink|<stable-node-or-device-id>
```

其中 `backend-device-id` 必须是能重新传回 GStreamer element 的稳定标识；
不要仅使用展示名称。

## 3. 推荐技术路线

### 3.1 PipeWire 优先

推荐运行依赖：

- PipeWire
- WirePlumber
- `pipewire-pulse`（兼容旧 PulseAudio 应用）
- GStreamer 1.20+
- GStreamer PipeWire plugin（通常提供 `pipewiresrc` / `pipewiresink`）

原因：PipeWire 已是多数现代 Linux 发行版的默认音频图基础设施，能统一
ALSA、PulseAudio 兼容层、蓝牙和专业音频场景。它也更适合播放 monitor
source（系统回采）的表示。

### 3.2 后备路线

如果目标发行版缺少 GStreamer PipeWire plugin：

1. 使用 `pulsesrc` / `pulsesink` 作为过渡实现。
2. 最后才考虑 `alsasrc` / `alsasink`。

ALSA 是硬件层，设备独占、混音、热插拔和“系统播放回采”体验都会更差。
不要把 ALSA 当作桌面 Linux 的首选实现。

## 4. 需要修改的现有代码

### 4.1 `DeviceEnumerator`

文件：

```text
src/gstreamer/device_enumerator.h
src/gstreamer/device_enumerator.cpp
```

`DeviceEnumerator` 已降级为 GStreamer 后端的内部实现，不再对外暴露公共头文件；
Web/CLI 通过 `oamr::audio::AudioBackend::list_sources()/list_sinks()` 消费设备
（见 `include/oamr/audio/audio_backend.hpp` 与 `audio_types.hpp` 的
`DeviceInfo`）。现状：设备监视器已能发现 GStreamer 设备，但枚举逻辑仍带有
WASAPI 名称、默认设备与 endpoint 的处理。

Linux 实现要求：

1. 使用 `GstDeviceMonitor` 枚举 `Audio/Source` 和 `Audio/Sink`。
2. 从 `GstDevice` 的 properties/caps 提取可回传给对应 element 的 device
   属性值。
3. 设备展示名应优先使用 PipeWire 的 description/nick，再退回 display-name。
4. 明确标记：
   - 普通 capture source；
   - 普通 playback sink；
   - PipeWire/PulseAudio monitor source（系统播放回采）。
5. 在设备 ID 变化或设备拔出时不要崩溃；返回清晰错误。

不要依赖：

- `wasapi2src` / `wasapi2sink` 名称；
- Windows GUID 格式；
- `Default Audio Capture Device` / `Default Audio Render Device` 名称；
- `loopback=true` WASAPI 属性。

### 4.2 设备目录与 render-loopback 转换

文件：

```text
src/gstreamer/audio_backend.cpp   （render_source_selector / list_sources）
src/web/web_server.cpp            （只消费 DeviceInfo 语义字段）
```

当前后端在 `render_source_selector()` 中把 `wasapi2sink|` 转换为
`wasapi2src|` 生成 render-loopback source。这段逻辑必须平台化：

- Windows：保留现有 WASAPI loopback 行为。
- Linux：由枚举器直接提供 monitor source；不要伪造 `pipewiresink` 到
  `pipewiresrc` 的字符串转换。

`audio::DeviceInfo` 已经用 `loopback_of` 字段标记 render-loopback source
（指向其监听的 sink 设备 id），Web 层只消费这些语义字段，不再依赖
factory 前缀推断。

### 4.3 GStreamer 管线

文件（后端内部实现）：

```text
src/gstreamer/rtp_opus_pipeline.cpp
src/gstreamer/audio_backend.cpp
```

公共契约见 `include/oamr/audio/audio_types.hpp`：sender/receiver 使用
`SenderSettings` / `ReceiverSettings`，网络参数统一由 `NetworkProfile`
经 `resolve_network_profile()` 解析。

当前代码已经把 source/sink selector 解析为：

```cpp
factory|device
```

这是 Linux 复用的主要入口。Linux 适配应：

- 让 `pipewiresrc` / `pipewiresink` 进入同一 `parse_selector()` 路径；
- 确认 element 的实际 device 属性名称和类型；
- 对 monitor source 不要设置 `loopback=true`；该属性仅适用于 Windows
  `wasapi2src`；
- 保留 PCM → Opus 的命名 capsfilter。不要重新引入字符串 caps 直接连接
  `opusenc` 的旧写法；它在 WASAPI 上已证明会产生静态协商问题；
- 继续允许最终 sink 自行协商本机支持的采样率/声道数。

### 4.4 配对和远端路由

以下模块不应因 Linux 适配而改变协议：

```text
src/pairing/pairing_service.cpp
src/web/web_server.cpp 的 /api/pair/* 与 /api/paired/route
```

配对目录会将 `backend_id` 发送给远端。因此 Linux 的 device ID 必须能在本机
被 `pipewiresrc` / `pipewiresink` 重新打开，且不能泄漏与本机无关的临时指针。

远端矩阵创建会要求另一台机器创建 sender 或 receiver。Linux 端必须完整支持：

- `RemoteRouteKind::Send`：选定 Linux source -> 本机 UDP sink；
- `RemoteRouteKind::Receive`：UDP source -> 选定 Linux sink。

## 5. Linux 构建与依赖检查

Ubuntu/Debian 起步依赖示例：

```bash
sudo apt install build-essential cmake pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  pipewire pipewire-pulse wireplumber
```

实现前必须检查实际环境：

```bash
gst-inspect-1.0 pipewiresrc pipewiresink
gst-device-monitor-1.0 Audio/Source Audio/Sink
wpctl status
```

不同发行版对 PipeWire GStreamer plugin 的拆包名称可能不同；不要把 Ubuntu
包名硬编码到 CMake。

## 6. 测试清单（完成标准）

### 设备与本机路由

- [ ] `oamr devices` 同时显示至少一个真实 source 和 sink。
- [ ] Web UI 能显示普通录音设备、播放设备和可用 monitor source。
- [ ] 麦克风 -> 扬声器可启动、暂停、恢复、删除。
- [ ] 一个 source -> 两个 sinks 可同时运行。
- [ ] monitor source -> 不同 playback sink 可工作，且不会写回同一 endpoint。
- [ ] 拔出 USB 音频设备后，错误明确且进程不崩溃。

### 网络与配对

- [ ] Linux sender -> Windows receiver。
- [ ] Windows sender -> Linux receiver。
- [ ] Linux ↔ Linux sender/receiver。
- [ ] Linux 与 Windows 配对后，双方都能看到对方开放设备。
- [ ] 从矩阵创建 Linux source -> Windows sink 远端路线。
- [ ] 从矩阵创建 Windows source -> Linux sink 远端路线。
- [ ] 修改网络路由质量/延迟/模式后，路由表能重建该管线。

### 自动化

保留现有 Core 和 GStreamer smoke tests，并至少新增：

- selector parser 对 `pipewiresrc|...`、`pipewiresink|...` 的测试；
- 不依赖实际硬件的 Opus capsfilter sender pipeline 测试；
- Linux 条件下的设备枚举 smoke test（无设备时允许 skip，但不能失败）。

## 7. 可接受的实现顺序

1. 在 Linux CI/开发机上构建现有项目，确保 Core test 通过。
2. 实现 PipeWire 设备枚举和稳定 selector。
3. 让本机 source -> sink 工作。
4. 实现 monitor source 并接入 Web 的 render-loopback 语义字段。
5. 验证 RTP sender/receiver。
6. 验证 Windows ↔ Linux 配对目录和远端矩阵。
7. 增加 Linux 安装文档和 CI（如有）。

## 8. 不要做的事

- 不要为了 Linux 支持删除 WASAPI 后端。
- 不要改变 RTP payload type 96 或现有配对 HTTP 参数，除非同时做显式
  协议版本升级。
- 不要把 Web UI 暴露到 `0.0.0.0`；它必须继续只绑定 `127.0.0.1`。
- 不要把 PipeWire node ID 当作跨机器 ID；它只对该 Linux 主机有意义。
- 不要假设所有 PipeWire source 都是硬件麦克风；monitor source 是合法路由源。
