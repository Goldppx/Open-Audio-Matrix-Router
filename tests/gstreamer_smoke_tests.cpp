#include "oamr/gstreamer/rtp_opus_pipeline.hpp"

#include <cassert>
#include <iostream>

int main() {
    using namespace oamr::gstreamer;
    // This uses GStreamer's test generator, never physical audio hardware.
    // It catches sender caps regressions across every exposed profile.
    for (const auto quality : {AudioQuality::Low, AudioQuality::Medium, AudioQuality::High}) {
        for (const auto mode : {LatencyMode::Stable, LatencyMode::Auto, LatencyMode::LowLatency}) {
            SenderSettings settings;
            settings.host = "127.0.0.1";
            settings.port = 5999;
            settings.source_device = "audiotestsrc|";
            settings.network = {quality, 100, mode};
            RtpOpusPipeline pipeline;
            assert(pipeline.start_sender(settings));
            assert(pipeline.poll());
            pipeline.stop();
        }
    }
    std::cout << "GStreamer RTP/Opus sender smoke tests passed.\n";
}
