#include "oamr/audio/backend_factory.hpp"

#include "device_enumerator.h"
#include "rtp_opus_pipeline.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oamr::gstreamer {
namespace {

/**
 * Backend-owned route: adapts the internal RtpOpusPipeline to the
 * platform-neutral oamr::audio::AudioRoute contract.
 */
class GStreamerRoute final : public audio::AudioRoute {
public:
    bool start_sender(const audio::SenderSettings& settings) { return pipeline_.start_sender(settings); }
    bool start_receiver(const audio::ReceiverSettings& settings) { return pipeline_.start_receiver(settings); }
    bool start_network_mixer(const audio::NetworkMixerSettings& settings) { return pipeline_.start_network_mixer(settings); }
    bool start_loopback(const audio::LoopbackSettings& settings) { return pipeline_.start_loopback(settings); }
    bool start_fanout(const audio::FanoutSettings& settings) { return pipeline_.start_local_fanout(settings); }
    bool start_matrix(const audio::MatrixSettings& settings) { return pipeline_.start_local_matrix(settings); }

    void stop() noexcept override { pipeline_.stop(); }
    bool poll() override { return pipeline_.poll(); }
    [[nodiscard]] bool is_running() const noexcept override { return pipeline_.is_running(); }
    [[nodiscard]] const std::string& last_error() const noexcept override { return pipeline_.last_error(); }
    [[nodiscard]] std::optional<audio::TransportTelemetry> transport_telemetry() const noexcept override { return pipeline_.transport_telemetry(); }

private:
    RtpOpusPipeline pipeline_;
};

/**
 * Derives the render-loopback source selector for a playback selector.
 *
 * Windows WASAPI exposes loopback capture by opening wasapi2src with the
 * same endpoint GUID that wasapi2sink renders to. Other factories (PipeWire,
 * PulseAudio, ALSA) report monitor sources directly as capture devices, so
 * the empty string means "no synthesized source" there.
 */
std::string render_source_selector(const std::string& sink_selector) {
    constexpr std::string_view prefix{"wasapi2sink|"};
    return sink_selector.rfind(prefix, 0) == 0
        ? "wasapi2src|" + sink_selector.substr(prefix.size())
        : std::string{};
}

/** Best-effort link between PipeWire-style "Monitor of <sink>" sources and
 *  their playback device. Only used to populate loopback_of metadata for the
 *  Web layer; routing itself always uses the device selectors. */
std::string monitor_sink_selector(const audio::DeviceInfo& source,
                                  const std::vector<audio::DeviceInfo>& sinks) {
    constexpr std::string_view prefix{"Monitor of "};
    if (source.name.rfind(prefix, 0) != 0) return {};
    const std::string sink_name = source.name.substr(prefix.size());
    for (const auto& sink : sinks)
        if (sink.name == sink_name) return sink.id;
    return {};
}

class GStreamerBackend final : public audio::AudioBackend {
public:
    std::vector<audio::DeviceInfo> list_sources() const override {
        const auto captures = enumerator_.list_capture_devices();
        const auto sinks = enumerator_.list_playback_devices();

        std::unordered_set<std::string> playback_endpoints;
        for (const auto& sink : sinks)
            playback_endpoints.insert(audio::selector_device_id(sink.id));

        std::vector<audio::DeviceInfo> result;
        // On Windows the device monitor reports render endpoints under both
        // Audio/Source and Audio/Sink. Present each endpoint exactly once: as
        // the sink plus its explicit render-loopback source below.
        for (const auto& capture : captures) {
            if (playback_endpoints.contains(audio::selector_device_id(capture.id))) continue;
            audio::DeviceInfo info = capture;
            if (info.loopback_of.empty())
                info.loopback_of = monitor_sink_selector(info, sinks);
            result.push_back(std::move(info));
        }
        for (const auto& sink : sinks) {
            if (sink.is_default) continue;
            const std::string source_selector = render_source_selector(sink.id);
            if (source_selector.empty()) continue;
            result.push_back(audio::DeviceInfo{source_selector, sink.name,
                audio::PortDirection::Source, false, sink.id});
        }
        return result;
    }

    std::vector<audio::DeviceInfo> list_sinks() const override {
        return enumerator_.list_playback_devices();
    }

    std::unique_ptr<audio::AudioRoute> create_loopback(const audio::LoopbackSettings& settings) override {
        return start_route([&](GStreamerRoute& route) { return route.start_loopback(settings); });
    }
    std::unique_ptr<audio::AudioRoute> create_fanout(const audio::FanoutSettings& settings) override {
        return start_route([&](GStreamerRoute& route) { return route.start_fanout(settings); });
    }
    std::unique_ptr<audio::AudioRoute> create_matrix(const audio::MatrixSettings& settings) override {
        return start_route([&](GStreamerRoute& route) { return route.start_matrix(settings); });
    }
    std::unique_ptr<audio::AudioRoute> create_sender(const audio::SenderSettings& settings) override {
        return start_route([&](GStreamerRoute& route) { return route.start_sender(settings); });
    }
    std::unique_ptr<audio::AudioRoute> create_receiver(const audio::ReceiverSettings& settings) override {
        return start_route([&](GStreamerRoute& route) { return route.start_receiver(settings); });
    }
    std::unique_ptr<audio::AudioRoute> create_network_mixer(const audio::NetworkMixerSettings& settings) override {
        return start_route([&](GStreamerRoute& route) { return route.start_network_mixer(settings); });
    }

    [[nodiscard]] const std::string& last_error() const noexcept override { return error_; }

private:
    template <typename Starter>
    std::unique_ptr<audio::AudioRoute> start_route(Starter starter) {
        auto route = std::make_unique<GStreamerRoute>();
        if (!starter(*route)) {
            error_ = route->last_error();
            return nullptr;
        }
        error_.clear();
        return route;
    }

    DeviceEnumerator enumerator_;
    std::string error_;
};

} // namespace

} // namespace oamr::gstreamer

namespace oamr::audio {

std::unique_ptr<AudioBackend> create_audio_backend() {
    return std::make_unique<oamr::gstreamer::GStreamerBackend>();
}

} // namespace oamr::audio
