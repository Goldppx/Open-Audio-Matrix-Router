#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace oamr::gstreamer {

/** User-facing Opus quality selection. The PCM graph remains 48 kHz. */
enum class AudioQuality { Low, Medium, High };
/** Stable protects continuity; Auto adapts; LowLatency protects recency. */
enum class LatencyMode { Stable, Auto, LowLatency };

struct NetworkAudioProfile {
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

inline bool valid_max_latency(std::uint16_t value) {
    return value == 40 || value == 60 || value == 100 || value == 150;
}

/** Resolves a small, predictable initial profile before live feedback arrives. */
inline std::optional<ResolvedNetworkProfile> resolve_network_profile(const NetworkAudioProfile& profile) {
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

/** One-step adaptation policy, deliberately rate-limited by its caller. */
inline AudioQuality adapt_quality(AudioQuality current, const NetworkAudioProfile& profile,
                                 const NetworkFeedback& feedback) {
    const bool pressured = feedback.observed_latency_ms >= profile.max_latency_ms || feedback.packet_loss_percent >= 3;
    if (pressured) return current == AudioQuality::High ? AudioQuality::Medium : AudioQuality::Low;
    const bool healthy = feedback.observed_latency_ms + 20 < profile.max_latency_ms
        && feedback.jitter_ms <= 5 && feedback.packet_loss_percent == 0;
    if (profile.mode == LatencyMode::Auto && healthy)
        return current == AudioQuality::Low ? AudioQuality::Medium : AudioQuality::High;
    return current;
}

} // namespace oamr::gstreamer
