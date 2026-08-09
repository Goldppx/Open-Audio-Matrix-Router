#pragma once

#include "oamr/audio/audio_types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace oamr::gstreamer {

/**
 * Internal GStreamer route engine: owns one pipeline per active route.
 *
 * This is an implementation detail of the GStreamer backend and should not
 * be used outside this directory. Reconfigure by stop(), then start() with
 * new settings; this is the MVP's safe runtime device-switching contract.
 */
class RtpOpusPipeline {
public:
    RtpOpusPipeline();
    ~RtpOpusPipeline();
    RtpOpusPipeline(const RtpOpusPipeline&) = delete;
    RtpOpusPipeline& operator=(const RtpOpusPipeline&) = delete;

    bool start_sender(const audio::SenderSettings& settings);
    bool start_receiver(const audio::ReceiverSettings& settings);
    bool start_loopback(const audio::LoopbackSettings& settings);
    bool start_local_fanout(const audio::FanoutSettings& settings);
    bool start_local_matrix(const audio::MatrixSettings& settings);
    void stop() noexcept;
    /** Returns false after a GStreamer ERROR or EOS message and updates last_error(). */
    bool poll();
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;
    [[nodiscard]] std::optional<audio::TransportTelemetry> transport_telemetry() const noexcept;
    [[nodiscard]] std::optional<audio::ResolvedNetworkProfile> resolved_network_profile() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oamr::gstreamer
