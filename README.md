# Open Audio Matrix Router (OAMR)

OAMR is an Apache-2.0 open-source, cross-platform audio matrix router. Its first milestone is a command-line RTP/Opus link: microphone on computer A to speaker on computer B. The core is C++20 and platform-neutral; GStreamer supplies the current device and network backend.

## MVP scope

- Windows and Linux first; macOS and Android remain backend-extension targets.
- Manual IPv4/DNS host and UDP port configuration on a trusted LAN.
- RTP/UDP with Opus, 48 kHz, stereo, 20 ms frames, RTP payload type 96.
- One-to-one and one-to-many graph representation. Mixing is intentionally deferred.
- Default audio devices work first. Non-default selection is supported where the installed GStreamer source/sink plugin exposes a string `device` property; unsupported plugins fall back to the default endpoint.

Not in this milestone: encryption, discovery, NAT traversal, virtual device drivers, WebRTC, GUI, or Android packaging.

## Architecture

`AudioNode` groups one device or virtual endpoint's ports. Every `AudioPort` has exactly one direction:

- **Source** produces audio for the router: microphone, RTP receiver, or an output that has been intentionally republished as a virtual input.
- **Sink** consumes router audio: speaker, recorder, RTP sender, or a virtual output.

`AudioConnection` is always `Source -> Sink`. A node can contain Source, Sink, or both kinds of ports, so the model can represent the requested `A1 -> B0` path without giving one port contradictory directions. `AudioGraph` is desired state only; future router workers will reconcile it with live GStreamer pipelines.

```text
Computer A                                  Computer B
mic Source -> GStreamer -> Opus/RTP/UDP  -> RTP/Opus -> GStreamer -> speaker Sink
```

See [docs/architecture.md](docs/architecture.md) for module responsibilities and extension points.

## Dependencies

Install these before configuring the full MVP:

- CMake 3.24 or newer
- A C++20 compiler (MSVC 2022, GCC 12+, or Clang 15+ recommended)
- GStreamer 1.20+ runtime **and development** packages
- GStreamer base, good, and bad plugin sets, including `opus`, `rtp`, `udp`, and a platform audio source/sink
- `pkg-config` / `pkgconf` on Linux and other Unix-like development hosts. On Windows, CMake can directly use the official GStreamer MSVC installation; set `-DGSTREAMER_ROOT=C:/path/to/msvc_x86_64` only when it is not installed in the default location.

On Ubuntu 24.04, a suitable starting point is:

```bash
sudo apt install build-essential cmake pkg-config libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev gstreamer1.0-tools \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
```

On Windows, install the official GStreamer MSVC development and runtime packages matching the compiler architecture. Ensure `pkg-config` can find the package `.pc` files via `PKG_CONFIG_PATH`, and put GStreamer's `bin` directory on `PATH` when running `oamr`.

## Build

```bash
cmake -S . -B build -DOAMR_ENABLE_GSTREAMER=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

For a Core-only build without GStreamer:

```bash
cmake -S . -B build-core -DOAMR_ENABLE_GSTREAMER=OFF
cmake --build build-core --config Debug
ctest --test-dir build-core -C Debug --output-on-failure
```

## Run the network demo

## Local Web UI

Run a browser-based local control page with no Qt dependency:

```bat
build-gstreamer-check\run-oamr.cmd web
```

Then open [http://127.0.0.1:8787](http://127.0.0.1:8787). The server binds only to loopback, lists GStreamer devices, and can start/stop one local Source-to-Sink route. It is deliberately not exposed to the LAN and does not yet replace the full matrix UI.

On Windows development builds, use the generated `run-oamr.cmd` launcher rather than launching `oamr.exe` directly. It provides the GStreamer DLL and plugin paths without changing the system-wide `PATH`:

```bat
build-gstreamer-check\run-oamr.cmd devices
```

The release installer will package a private runtime instead; this launcher is only for in-tree development.

## Windows portable package

Create a Release build first, then create a self-contained ZIP containing
OAMR, its minimal GStreamer runtime closure, required WASAPI/RTP/Opus plugins,
and the MSVC x64 app-local redistributable DLLs:

```powershell
cmake -S . -B build-portable-release -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DGSTREAMER_ROOT="C:/Program Files/GStreamer/1.0/msvc_x86_64"
cmake --build build-portable-release
.\scripts\package-portable.ps1 -BuildDirectory build-portable-release -Version 0.1.0-dev
```

The generated file is placed under `dist/`. On another Windows x64 device,
extract it anywhere and run `run-oamr.cmd web --port 8792`. The package keeps
its GStreamer DLL, plugins, and required MSVC runtime DLLs private; it does
not modify system `PATH` or require a separate VC++ runtime installation. It
targets supported Windows x64 systems, which provide the Windows Universal CRT.

The minimal runtime intentionally includes only WASAPI device access, audio
conversion/resampling/mixing, Opus, RTP, RTP jitter buffering, UDP, and their
dependency DLLs. Adding a codec or platform backend requires updating the
allowlist in `scripts/package-portable.ps1` and rerunning the extracted-package
smoke test.

On computer B (the speaker), start the receiver. The network profile is set
independently on each endpoint; use the same latency choice on both ends for
predictable results:

```bash
oamr receive --port 5004 --quality high --max-latency-ms 100 --mode auto
```

On computer A (the microphone), send to B's LAN address:

```bash
oamr send --host 192.168.1.20 --port 5004 --quality high --max-latency-ms 100 --mode auto
```

### Network profile

`--quality` accepts `low`, `medium`, or `high`; it selects a 48, 96, or
160 kbps stereo Opus starting point. `--max-latency-ms` accepts `40`, `60`,
`100`, or `150`. `--mode` accepts:

- `stable`: 10/20 ms frames, FEC enabled, and the receiver preserves audio
  continuity even when this can briefly exceed the target.
- `auto`: 5/10 ms frames and a bounded jitter buffer. The profile includes a
  deterministic quality adaptation policy ready to consume remote feedback.
- `low-latency`: 5 ms frames and a small jitter buffer; late packets are
  discarded rather than increasing playout delay. This costs more CPU and is
  less tolerant of loss.

The current transport applies the selected initial profile to GStreamer. The
next transport increment adds the receiver-to-sender LAN feedback channel that
supplies measured loss, jitter, and playout delay to Auto mode.

For same-computer microphone-to-speaker testing:

```bash
oamr loopback
```

For a single local source copied to multiple local sinks, repeat `--sink-device`:

```bat
build-gstreamer-check\run-oamr.cmd fanout --source-device "wasapi2src|<capture-device-id>" ^
  --sink-device "wasapi2sink|<speaker-a-id>" --sink-device "wasapi2sink|<speaker-b-id>"
```

Use IDs printed by `devices`. Avoid selecting a render-loopback source and the same physical speaker as its sink, because that can create audible feedback.

Use `oamr devices` to inspect GStreamer-discovered devices. Copy a selectable device ID (quote it in PowerShell because it contains `|`) into `--device`, `--source-device`, or `--sink-device`. Press Ctrl+C to stop. The default receiver jitter buffer is 60 ms, targeting LAN end-to-end latency under 100 ms under normal conditions.

`oamr` currently monitors the GStreamer bus after startup and reports negotiation, permission, device, and end-of-stream failures instead of continuing to claim it is running. The live pipeline is stopped and recreated when a caller changes device settings; an interactive device-switch command will arrive with the RouterEngine.

## Repository layout

```text
apps/oamr/       Command-line MVP entry point
include/oamr/    Public Core and GStreamer backend interfaces
src/core/        Platform-neutral graph model
src/gstreamer/   Current device discovery and RTP/Opus backend
tests/           Dependency-free Core tests
docs/            Architecture and development decisions
cmake/           CMake dependency discovery
```
