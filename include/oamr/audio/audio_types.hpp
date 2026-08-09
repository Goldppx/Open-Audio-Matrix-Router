#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oamr::audio {

/**
 * Backend-neutral settings and results shared by every audio backend.
 *
 * Device selectors use the `factory|device` convention established by the
 * GStreamer backend (for example `wasapi2src|{guid}` or `audiotestsrc|`).
 * Consumers must treat the whole selector as opaque and pass it back to the
 * same backend; use selector_device_id() only for display or protocol fields.
 */
enum class PortDirection { Source, Sink };

/** A routable endpoint discovered by a backend. */
struct DeviceInfo {
    /** Opaque, backend-stable selector that can be passed back to the backend. */
    std::string id;
    std::string name;
    PortDirection direction{PortDirection::Source};
    /** True when this is the operating system's default endpoint. */
    bool is_default{false};
    /**
     * For render-loopback sources ("capture what this device plays"), the
     * device id of the sink whose playback is mirrored. Empty for ordinary
     * capture devices and for sinks.
     */
    std::string loopback_of;
};

/** PCM parameters used at the backend boundary. Backends convert as needed. */
struct PcmSettings {
    std::uint32_t sample_rate{48000};
    std::uint16_t channels{2};
    std::uint16_t opus_frame_ms{20};
};

/** User-facing Opus quality selection. The PCM graph remains 48 kHz. */
enum class AudioQuality { Low, Medium, High };
/** Stable protects continuity; Auto adapts; LowLatency protects recency. */
enum class LatencyMode { Stable, Auto, LowLatency };

struct NetworkProfile {
    AudioQuality quality{AudioQuality::Medium};
    std::uint16_t max_latency_ms{100};
    LatencyMode mode{LatencyMode::Auto};
};

struct ResolvedNetworkProfile {
    std::uint32_t opus_bitrate_bps{};
    std::uint16_t opus_frame_ms{};
    std::uint16_t jitter_buffer_ms{};
    bool inband_fec{};
    bool drop_on_latency{};
};

/** One local capture endpoint routed to one local playback endpoint. */
struct LoopbackSettings {
    std::string source_device;
    std::string sink_device;
    /** Open the source in render-loopback mode (capture its playback). */
    bool capture_render_device{false};
    PcmSettings pcm{};
};

/** One local capture endpoint duplicated to several local playback endpoints. */
struct FanoutSettings {
    std::string source_device;
    std::vector<std::string> sink_devices;
    PcmSettings pcm{};
};

struct MatrixRoute {
    std::size_t source_index{};
    std::size_t sink_index{};
    /** Linear gain, where 1.0 is unity (100%) and 0.0 is muted. */
    double gain{1.0};
};

struct MatrixSettings {
    std::vector<std::string> source_devices;
    std::vector<std::string> sink_devices;
    std::vector<MatrixRoute> routes;
    /** Per-source render-loopback flags; parallel to source_devices. */
    std::vector<bool> source_is_render_loopback;
    PcmSettings pcm{};
};

struct SenderSettings {
    std::string host;
    std::uint16_t port{};
    std::string source_device;  // Empty selects the operating system default.
    /** Open the source in render-loopback mode (capture its playback). */
    bool capture_render_device{false};
    PcmSettings pcm{};
    NetworkProfile network{};
};

struct ReceiverSettings {
    std::uint16_t port{};
    std::string sink_device;  // Empty selects the operating system default.
    PcmSettings pcm{};
    NetworkProfile network{};
};

/** One RTP/Opus receive leg feeding a shared local playback mixer. */
struct NetworkMixerInput {
    std::uint16_t port{};
    NetworkProfile network{};
    /** Linear gain, where 1.0 is unity (100%) and 0.0 is muted. */
    double gain{1.0};
};

/** Mixes several RTP/Opus receive legs into one local playback endpoint. */
struct NetworkMixerSettings {
    std::vector<NetworkMixerInput> inputs;
    std::string sink_device;  // Empty selects the operating system default.
    PcmSettings pcm{};
};

/**
 * Extracts the backend device part of a `factory|device` selector. Returns
 * the whole string when it contains no separator, and an empty string when
 * the selector is a bare factory (the backend's default endpoint).
 */
inline std::string selector_device_id(const std::string& selector) {
    const auto delimiter = selector.find('|');
    return delimiter == std::string::npos ? selector : selector.substr(delimiter + 1);
}

inline bool valid_max_latency(std::uint16_t value) {
    return value == 40 || value == 60 || value == 100 || value == 150;
}

/** Resolves a small, predictable initial profile before live feedback arrives. */
inline std::optional<ResolvedNetworkProfile> resolve_network_profile(const NetworkProfile& profile) {
    if (!valid_max_latency(profile.max_latency_ms)) return std::nullopt;
    const std::uint32_t bitrate = profile.quality == AudioQuality::Low ? 48000
        : profile.quality == AudioQuality::Medium ? 96000 : 160000;
    const std::uint16_t frame = profile.mode == LatencyMode::LowLatency ? 5
        : profile.mode == LatencyMode::Auto ? (profile.max_latency_ms <= 60 ? 5 : 10)
        : (profile.max_latency_ms <= 60 ? 10 : 20);
    // Reserve a little room for capture/render scheduling. This is a target,
    // not a promise: a congested network can still force late-packet drops.
    const std::uint16_t reserve = static_cast<std::uint16_t>(frame + 10);
    const std::uint16_t available = profile.max_latency_ms > reserve
        ? static_cast<std::uint16_t>(profile.max_latency_ms - reserve) : 5;
    const std::uint16_t jitter = profile.mode == LatencyMode::Stable
        ? std::clamp<std::uint16_t>(available, 15, 100)
        : profile.mode == LatencyMode::Auto
            ? std::clamp<std::uint16_t>(static_cast<std::uint16_t>(available * 3 / 4), 10, 60)
            : std::clamp<std::uint16_t>(static_cast<std::uint16_t>(available / 2), 5, 25);
    return ResolvedNetworkProfile{bitrate, frame, jitter,
        profile.mode != LatencyMode::LowLatency, profile.mode != LatencyMode::Stable};
}

/** Remote feedback needed by the later UDP control channel / RTCP bridge. */
struct NetworkFeedback {
    std::uint16_t observed_latency_ms{};
    std::uint16_t jitter_ms{};
    std::uint8_t packet_loss_percent{};
};

/** Live receiver-side RTP statistics. An empty value means the route has no
 * RTP receive leg, so transport loss cannot be observed locally. */
struct TransportTelemetry {
    std::uint64_t received_packets{};
    std::uint64_t lost_packets{};
    double packet_loss_percent{};
};

/** One-step adaptation policy, deliberately rate-limited by its caller. */
inline AudioQuality adapt_quality(AudioQuality current, const NetworkProfile& profile,
                                 const NetworkFeedback& feedback) {
    const bool pressured = feedback.observed_latency_ms >= profile.max_latency_ms || feedback.packet_loss_percent >= 3;
    if (pressured) return current == AudioQuality::High ? AudioQuality::Medium : AudioQuality::Low;
    const bool healthy = feedback.observed_latency_ms + 20 < profile.max_latency_ms
        && feedback.jitter_ms <= 5 && feedback.packet_loss_percent == 0;
    if (profile.mode == LatencyMode::Auto && healthy)
        return current == AudioQuality::Low ? AudioQuality::Medium : AudioQuality::High;
    return current;
}

} // namespace oamr::audio
