# OAMR architecture

## Layer boundaries

`oamr_core` contains no WASAPI, ALSA, CoreAudio, Android, GStreamer, socket,
or thread ownership. It describes ports, nodes, connections, and validates
route direction. This gives every future backend one stable routing contract.

Consumers (CLI, Web UI, tests) only ever see `oamr::audio::AudioBackend` and
`oamr::audio::AudioRoute` from `include/oamr/audio/*`. The concrete backend is
created by `oamr::audio::create_audio_backend()`; no consumer includes
backend- or operating-system-specific headers. Settings (`LoopbackSettings`,
`FanoutSettings`, `MatrixSettings`, `SenderSettings`, `ReceiverSettings`) and
network profiles are plain structs, so a new backend (PipeWire/PulseAudio/ALSA
on Linux, Oboe/AAudio on Android) only has to implement the interface.

The GStreamer backend (`oamr_gstreamer`) implements that interface. Each
running route is one transient pipeline owned by an `AudioRoute`:

- sender: `audio source -> convert/resample -> opusenc -> rtpopuspay -> udpsink`
- receiver: `udpsrc -> rtpjitterbuffer -> rtpopusdepay -> opusdec -> convert/resample -> audio sink`
- loopback: `audio source -> convert/resample -> audio sink`
- fanout/matrix: one capture pipeline per source, teed into per-sink
  `queue` branches (matrix also mixes multiple sources per sink)

Pipelines are configured as 48 kHz, stereo, signed 16-bit PCM at the backend
boundary; GStreamer carries out required format conversion. Route properties
are changed by stopping and recreating the route; that stop/start contract is
the MVP's safe runtime device-switching mechanism. Backends must make
`AudioRoute` destruction safe from any thread.

## Graph semantics

Ports are intentionally narrower than hardware devices. A device may publish any number of Source and Sink ports. A connection may only join a Source to a Sink. One Source may have many connections; that is the one-to-many MVP representation. Several Sources converging on one Sink require an explicit mixer worker, which will be introduced after the base transport is verified.

To represent output capture (`A1 -> B0`), expose the content of A's output as a distinct Source port, often backed by a loopback capture or virtual device. The original physical output remains a Sink port. This avoids direction ambiguities and keeps cyclic feedback policies explicit.

## Planned evolution

1. Add a RouterEngine that watches `AudioGraph` changes and creates/stops
   `AudioRoute`s atomically through the same `AudioBackend` interface.
2. Add mixer, gain, mute, channel maps, and feedback-loop safety policy.
3. Add mDNS/ZeroConf control-plane discovery and SDP/session metadata.
4. Add Qt control matrix and platform-specific virtual-device integrations.
5. Add low-latency tuning, WebRTC/SRTP, then Android's Oboe/AAudio backend.

Android-specific audio code belongs behind the same backend interface, never
in `oamr_core` or the Web/CLI layers.
