# OAMR architecture

## Layer boundaries

The Core contains no WASAPI, ALSA, CoreAudio, Android, GStreamer, socket, or thread ownership. It describes ports, nodes, connections, and validates route direction. This gives every future backend one stable routing contract.

The GStreamer backend is replaceable. It currently owns these transient pipelines:

- sender: `audio source -> convert/resample -> opusenc -> rtpopuspay -> udpsink`
- receiver: `udpsrc -> rtpjitterbuffer -> rtpopusdepay -> opusdec -> convert/resample -> audio sink`
- loopback: `audio source -> convert/resample -> audio sink`

Both are configured as 48 kHz, stereo, signed 16-bit PCM at the backend boundary. GStreamer carries out required format conversion. The sender requests 20 ms Opus frames; the receiver uses a 60 ms jitter buffer by default.

`LocalRouterEngine` groups all enabled connections from one local Source into one `tee` pipeline and adds a `queue` before each Sink. This is the first live one-to-many implementation. It intentionally rejects multiple Sources targeting one Sink until a mixer with explicit gain/format policy is added.

## Graph semantics

Ports are intentionally narrower than hardware devices. A device may publish any number of Source and Sink ports. A connection may only join a Source to a Sink. One Source may have many connections; that is the one-to-many MVP representation. Several Sources converging on one Sink require an explicit mixer worker, which will be introduced after the base transport is verified.

To represent output capture (`A1 -> B0`), expose the content of A's output as a distinct Source port, often backed by a loopback capture or virtual device. The original physical output remains a Sink port. This avoids direction ambiguities and keeps cyclic feedback policies explicit.

## Planned evolution

1. Add a RouterEngine that watches `AudioGraph` changes and creates/stops backend workers atomically.
2. Add a RouterEngine worker that turns local Source-to-Sink and Source-to-many-Sink graph connections into active pipelines.
3. Add mixer, gain, mute, channel maps, and feedback-loop safety policy.
4. Add mDNS/ZeroConf control-plane discovery and SDP/session metadata.
5. Add Qt control matrix and platform-specific virtual-device integrations.
6. Add low-latency tuning, WebRTC/SRTP, then Android's Oboe/AAudio backend.

Android-specific audio code belongs behind the same backend interface, never in `oamr_core`.
