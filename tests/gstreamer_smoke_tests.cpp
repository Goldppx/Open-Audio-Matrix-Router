#include "oamr/audio/backend_factory.hpp"

#include <cassert>
#include <iostream>

int main() {
    // Exercises the platform-neutral factory against the compiled backend.
    // GStreamer's test generator supplies the source, never physical audio
    // hardware; this catches sender caps regressions across every profile.
    auto backend = oamr::audio::create_audio_backend();
    assert(backend);
    for (const auto quality : {oamr::audio::AudioQuality::Low,
                               oamr::audio::AudioQuality::Medium,
                               oamr::audio::AudioQuality::High}) {
        for (const auto mode : {oamr::audio::LatencyMode::Stable,
                                oamr::audio::LatencyMode::Auto,
                                oamr::audio::LatencyMode::LowLatency}) {
            oamr::audio::SenderSettings settings;
            settings.host = "127.0.0.1";
            settings.port = 5999;
            settings.source_device = "audiotestsrc|";
            settings.network = {quality, 100, mode};
            auto route = backend->create_sender(settings);
            assert(route);
            assert(route->poll());
            route->stop();
        }
    }
    std::cout << "GStreamer RTP/Opus sender smoke tests passed.\n";
}
