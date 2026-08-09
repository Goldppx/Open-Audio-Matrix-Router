#include "oamr/audio/backend_factory.hpp"

#include <cassert>
#include <gst/gst.h>
#include <iostream>

int main() {
    gst_init(nullptr, nullptr);
    // Minimal Linux installations can have the GStreamer development headers
    // while omitting the runtime RTP/UDP plugins. This is an environment
    // prerequisite, not a backend regression; CTest maps 77 to SKIPPED.
    for (const char* factory_name : {"opusenc", "rtpopuspay", "udpsink", "rtpjitterbuffer", "rtpopusdepay", "fakesink", "audiotestsrc"}) {
        GstElementFactory* factory = gst_element_factory_find(factory_name);
        if (factory == nullptr) {
            std::cout << "SKIP: required GStreamer factory is unavailable: " << factory_name << "\n";
            return 77;
        }
        gst_object_unref(factory);
    }

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

    // The local matrix is also a mixer: two independent inputs share one
    // audiomixer and one render endpoint. Fake elements keep this test free
    // from physical audio hardware.
    oamr::audio::MatrixSettings local_mixer;
    local_mixer.source_devices = {"audiotestsrc|", "audiotestsrc|"};
    local_mixer.sink_devices = {"fakesink|"};
    local_mixer.routes = {{0, 0}, {1, 0}};
    auto local_route = backend->create_matrix(local_mixer);
    assert(local_route);
    assert(local_route->poll());
    assert(local_route->set_mixer_input_gain(0, 0.35));
    assert(!local_route->set_mixer_input_gain(2, 1.0));
    local_route->stop();

    // A shared RTP mixer owns one sink, while each UDP input keeps its own
    // jitter buffer. It may not receive packets in a smoke test, but it must
    // enter a valid running state without opening a real device.
    oamr::audio::NetworkMixerSettings network_mixer;
    network_mixer.sink_device = "fakesink|";
    network_mixer.inputs = {{56001, {}}, {56002, {}}};
    auto network_route = backend->create_network_mixer(network_mixer);
    assert(network_route);
    assert(network_route->poll());
    assert(network_route->set_mixer_input_gain(1, 1.25));
    network_route->stop();
    std::cout << "GStreamer RTP/Opus sender smoke tests passed.\n";
}
