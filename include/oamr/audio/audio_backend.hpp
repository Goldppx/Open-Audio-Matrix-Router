#pragma once

#include "oamr/audio/audio_types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace oamr::audio {

/**
 * A running audio route owned by the backend and controlled by the consumer.
 *
 * Backends must make routes safe to destroy from any thread: destroying a
 * route stops its audio graph. Runtime failures are reported through poll();
 * stop() is idempotent.
 */
class AudioRoute {
public:
    virtual ~AudioRoute() = default;

    /** Stops the route and releases backend resources. Idempotent. */
    virtual void stop() noexcept = 0;

    /**
     * Returns false after a runtime failure or end-of-stream and updates
     * last_error(). Backends should call this frequently from the serving
     * loop so hardware changes surface promptly.
     */
    virtual bool poll() = 0;

    [[nodiscard]] virtual bool is_running() const noexcept = 0;
    [[nodiscard]] virtual const std::string& last_error() const noexcept = 0;
    [[nodiscard]] virtual std::optional<TransportTelemetry> transport_telemetry() const noexcept { return std::nullopt; }
};

/**
 * Platform-neutral audio backend.
 *
 * Implementations translate the settings below into concrete audio graphs:
 * GStreamer on Windows/Linux/macOS, Oboe/AAudio on Android, and so on.
 * Consumers (Web UI, CLI) must only ever see this interface, never backend
 * or operating-system headers.
 */
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    /**
     * Routable capture endpoints, including render-loopback sources that
     * mirror a sink's playback (DeviceInfo::loopback_of is set for those).
     * Default endpoints may be included with is_default set.
     */
    [[nodiscard]] virtual std::vector<DeviceInfo> list_sources() const = 0;
    [[nodiscard]] virtual std::vector<DeviceInfo> list_sinks() const = 0;

    /** The five route kinds the MVP supports. Each returns nullptr on
     *  failure and records the reason in last_error(). */
    virtual std::unique_ptr<AudioRoute> create_loopback(const LoopbackSettings& settings) = 0;
    virtual std::unique_ptr<AudioRoute> create_fanout(const FanoutSettings& settings) = 0;
    virtual std::unique_ptr<AudioRoute> create_matrix(const MatrixSettings& settings) = 0;
    virtual std::unique_ptr<AudioRoute> create_sender(const SenderSettings& settings) = 0;
    virtual std::unique_ptr<AudioRoute> create_receiver(const ReceiverSettings& settings) = 0;

    /** Reason for the last failed create_* call. */
    [[nodiscard]] virtual const std::string& last_error() const noexcept = 0;
};

} // namespace oamr::audio
