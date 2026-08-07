#pragma once

#include "oamr/gstreamer/network_profile.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oamr::gstreamer {

struct PcmSettings {
    std::uint32_t sample_rate{48000};
    std::uint16_t channels{2};
    std::uint16_t opus_frame_ms{20};
};

struct SenderSettings {
    std::string host;
    std::uint16_t port{};
    std::string source_device; // Empty selects the operating system default.
    /** Capture a Windows render endpoint through WASAPI loopback. */
    bool capture_render_device{false};
    PcmSettings pcm{};
    NetworkAudioProfile network{};
};

struct ReceiverSettings {
    std::uint16_t port{};
    std::string sink_device; // Empty selects the operating system default.
    std::uint16_t jitter_buffer_ms{60};
    PcmSettings pcm{};
    NetworkAudioProfile network{};
};

struct LoopbackSettings {
    std::string source_device;
    std::string sink_device;
    /** Capture a playback endpoint through WASAPI loopback (Windows only). */
    bool capture_render_device{false};
    PcmSettings pcm{};
};

/** One local capture endpoint duplicated to one or more local playback endpoints. */
struct LocalFanoutSettings {
    std::string source_device;
    std::vector<std::string> sink_devices;
    PcmSettings pcm{};
};

struct LocalMatrixRoute { std::size_t source_index; std::size_t sink_index; };
struct LocalMatrixSettings {
    std::vector<std::string> source_devices;
    std::vector<std::string> sink_devices;
    std::vector<LocalMatrixRoute> routes;
    /** Per-source WASAPI render-loopback flags; false for ordinary capture. */
    std::vector<bool> source_is_render_loopback;
    PcmSettings pcm{};
};

/**
 * Owns one GStreamer pipeline. Reconfigure by stop(), then start() with new
 * settings; this is the MVP's safe runtime device-switching contract.
 */
class RtpOpusPipeline {
public:
    RtpOpusPipeline();
    ~RtpOpusPipeline();
    RtpOpusPipeline(const RtpOpusPipeline&) = delete;
    RtpOpusPipeline& operator=(const RtpOpusPipeline&) = delete;

    bool start_sender(const SenderSettings& settings);
    bool start_receiver(const ReceiverSettings& settings);
    bool start_loopback(const LoopbackSettings& settings);
    bool start_local_fanout(const LocalFanoutSettings& settings);
    bool start_local_matrix(const LocalMatrixSettings& settings);
    void stop() noexcept;
    /** Returns false after a GStreamer ERROR or EOS message and updates last_error(). */
    bool poll();
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;
    [[nodiscard]] std::optional<ResolvedNetworkProfile> resolved_network_profile() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oamr::gstreamer
