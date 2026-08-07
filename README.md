# Open Audio Matrix Router (OAMR)

OAMR is an Apache-2.0, C++20 audio-routing prototype for local devices and
trusted LANs. It combines a local device matrix, RTP/Opus transport, and a
loopback-only Web UI. The current production target is Windows x64; Linux is
kept as a build target through the GStreamer backend abstraction.

## What works today

- Enumerate WASAPI capture and playback devices through GStreamer.
- Local microphone/render-loopback to one or more playback routes.
- RTP/UDP + Opus audio send and receive with 48 kHz PCM conversion.
- Multiple independently startable routes, with pause, resume and deletion.
- Per-network-route quality, latency target and mode adjustment.
- LAN pairing using one-time codes, aliases and exposed-device catalogs.
- Paired-device telemetry: active device, quality, target latency and packet
  loss field.
- Local Web UI at `127.0.0.1`; only the pairing control channel binds to the
  LAN on TCP 8791.
- Portable Windows ZIP containing the private GStreamer subset and MSVC
  runtime—no system-wide GStreamer or VC++ redistributable installation.

## Important limitations

This is an MVP, not a Dante-compatible production system. Pairing control has
no encryption or persistent authentication, discovery is manual, Auto mode
does not yet consume measured feedback, and Windows virtual audio devices are
not implemented. Use it only on a trusted LAN. Remote matrix routing requires
both computers to run the same recent OAMR portable release.

## Quick start: Windows portable package

1. Download the latest archive from `dist/`, extract it anywhere, and run:

   ```bat
   run-oamr.cmd web --port 8790
   ```

2. Open <http://127.0.0.1:8790>.
3. On each computer, select the devices to expose and save the pairing profile.
4. Generate a one-time pairing code on one computer; enter its IP, TCP 8791,
   alias and code on the other computer.
5. Use **音频矩阵** to create local or paired RTP routes. Modify quality,
   target latency and mode afterwards in **路由表**.

Allow inbound TCP 8791 on a Private Windows network. RTP media ports are
selected dynamically from the 52000 range for paired matrix routes; permit the
corresponding UDP traffic between the two trusted machines.

## Build from source

Prerequisites:

- CMake 3.24+
- C++20 compiler
- GStreamer 1.20+ development/runtime packages with audio, RTP and Opus
- On Windows, the official x64 MSVC GStreamer SDK

Windows example:

```powershell
cmake -S . -B build-release -G "NMake Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DGSTREAMER_ROOT="C:/Program Files/GStreamer/1.0/msvc_x86_64"
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

For the repeatable release workflow:

```powershell
.\scripts\build-and-package.ps1 -Version 0.2.4-dev
```

It configures the Release build, executes the tests, and writes a portable ZIP
to `dist/`.

## Architecture

```text
Loopback-only Web UI ──► route table ──► GStreamer pipelines
          │                    │
          └── pairing TCP 8791 ┴── paired catalog / remote route request

WASAPI source ─► convert/resample ─► Opus/RTP/UDP ─► jitter buffer ─► sink
```

- `src/core`: platform-neutral node, port and graph model.
- `src/gstreamer`: device discovery and local/RTP pipeline implementation.
- `src/pairing`: one-time-code pairing, catalog synchronization and remote
  route control.
- `src/web`: loopback-only HTTP API and embedded Web UI.
- `scripts`: portable runtime packaging and one-command build workflow.

More detail is in [docs/architecture.md](docs/architecture.md).

## License

Apache-2.0. See [LICENSE](LICENSE).
