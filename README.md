# Open Audio Matrix Router (OAMR)

OAMR is an Apache-2.0, C++20 audio-routing prototype for local devices and
trusted LANs. It combines a local device matrix, RTP/Opus transport, and a
local-first Web UI. The current production target is Windows x64; Linux is
kept as a build target through the GStreamer backend abstraction.

## What works today

- Enumerate WASAPI capture and playback devices through GStreamer.
- Local microphone/render-loopback to one or more playback routes.
- Mixer routes: multiple local sources, or multiple paired RTP sources, into
  one playback endpoint.
- Live per-source mixer level controls (0–200%) from the Route table, without
  restarting the audio route.
- RTP/UDP + Opus audio send and receive with 48 kHz PCM conversion.
- Multiple independently startable routes, with pause, resume and deletion.
- Per-network-route quality, latency target and mode adjustment.
- LAN pairing using one-time codes, aliases and exposed-device catalogs.
- Paired-device telemetry: active device, quality, target latency and packet
  loss field.
- Material Design 3 Web UI binds to `127.0.0.1` by default. It can be
  explicitly bound to one trusted-LAN IPv4 address when needed.
- Incremental, level-filtered diagnostics for routes, GStreamer failures,
  pairing, device enumeration and multi-adapter LAN discovery.
- Portable Windows ZIP containing the private GStreamer subset and MSVC
  runtime—no system-wide GStreamer or VC++ redistributable installation.

## Important limitations

This is an MVP, not a Dante-compatible production system. Pairing control has
no encryption or persistent authentication, Auto mode does not yet consume
measured feedback, and Windows virtual audio devices are not implemented. Use
it only on a trusted LAN. Remote matrix routing and discovery require both
computers to run the same recent OAMR portable release.

## Quick start: Windows portable package

1. Download the latest archive from `dist/`, extract it anywhere, and run:

   ```bat
   run-oamr.cmd web --port 8790
   ```

2. Open <http://127.0.0.1:8790>.
3. On each computer, select the devices to expose and save the pairing profile.
4. Generate a one-time pairing code on one computer; enter its IP, TCP 8791,
   alias and code on the other computer.
5. Use **音频矩阵** to create local or paired RTP routes. Select multiple
   sources in one output column to create a mixer. Modify quality, target
   latency and mode afterwards in **路由表**.

Allow inbound TCP 8791 and discovery UDP 8792 on a Private Windows network. RTP media ports are
selected dynamically from the 52000 range for paired matrix routes; permit the
corresponding UDP traffic between the two trusted machines.

### Optional LAN Web UI access

The Web UI stays private to the local machine by default. To make it reachable
from another device on a trusted LAN, explicitly bind it to the computer's LAN
IPv4 address (do not use `0.0.0.0`):

```bat
run-oamr.cmd web --hostname 192.168.31.99 --port 8790
```

Then open `http://192.168.31.99:8790` from that LAN device. This mode has **no
HTTP authentication yet**: anyone who can reach the address can control audio
routes and pairing settings. Keep it disabled unless you need it, and use only
on a trusted private network.

## Build from source

Prerequisites:

- CMake 3.24+
- C++20 compiler
- GStreamer 1.20+ development/runtime packages with audio, RTP and Opus
- On Windows, the official x64 MSVC GStreamer SDK
- Node.js 22+ and npm (development/build only; never required by end users)

Build the offline Web UI once before configuring CMake:

```powershell
cd web
npm ci
npm run build
cd ..
```

The TypeScript/Vite build is written to `web/dist`. CMake copies it beside
`oamr.exe`; the executable serves it locally. Node.js, Vite, a CDN, and a
separate Node HTTP server are not part of the portable runtime.

Windows example:

```powershell
cmake -S . -B build/windows -G "NMake Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DGSTREAMER_ROOT="C:/Program Files/GStreamer/1.0/msvc_x86_64"
cmake --build build/windows
ctest --test-dir build/windows --output-on-failure
```

For the repeatable release workflow:

```powershell
.\scripts\build-and-package.ps1 -Version 0.2.4-dev
```

It configures `build/windows`, executes the tests, and writes a portable ZIP
to `dist/`. Linux build trees belong under `build/linux`; CMake build products
are intentionally ignored by Git.

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
- `src/web`: local-first HTTP API and static Vite asset server, with explicit
  single-interface LAN binding support.
- `web`: TypeScript + Vite + Material Web (Material Design 3) frontend.
- `scripts`: portable runtime packaging and one-command build workflow.

More detail is in [docs/architecture.md](docs/architecture.md).
For a handoff-ready Linux implementation plan, see
[docs/linux-porting-guide.md](docs/linux-porting-guide.md).

## License

Apache-2.0. See [LICENSE](LICENSE).

The desktop UI uses Material Web 2.5.0 under Apache-2.0; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
