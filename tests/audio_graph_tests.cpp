#include "oamr/core/audio_graph.hpp"
#include "oamr/gstreamer/network_profile.hpp"

#include <cassert>
#include <iostream>

int main() {
    using namespace oamr;
    AudioGraph graph;
    assert(graph.add_node({"microphone", "Microphone", {{"mic.out", "Mic out", PortDirection::Source, {48000, 2, 16}, "default"}}}));
    assert(graph.add_node({"network", "Network", {{"rtp.send", "RTP send", PortDirection::Sink, {48000, 2, 16}, "192.168.1.2:5004"}}}));
    assert(graph.connect({"mic-to-network", "mic.out", "rtp.send"}));
    assert(graph.connections().size() == 1);
    assert(!graph.connect({"wrong-way", "rtp.send", "mic.out"}));
    assert(graph.disconnect("mic-to-network"));
    assert(graph.connections().empty());

    using namespace oamr::gstreamer;
    const auto low_latency = resolve_network_profile({AudioQuality::High, 40, LatencyMode::LowLatency});
    assert(low_latency && low_latency->opus_bitrate_bps == 160000 && low_latency->opus_frame_ms == 5);
    assert(low_latency->drop_on_latency && low_latency->jitter_buffer_ms <= 25);
    const auto stable = resolve_network_profile({AudioQuality::Low, 150, LatencyMode::Stable});
    assert(stable && stable->opus_bitrate_bps == 48000 && stable->inband_fec && !stable->drop_on_latency);
    assert(!resolve_network_profile({AudioQuality::Medium, 75, LatencyMode::Auto}));
    assert(adapt_quality(AudioQuality::High, {AudioQuality::High, 60, LatencyMode::Auto}, {61, 4, 0}) == AudioQuality::Medium);
    assert(adapt_quality(AudioQuality::Low, {AudioQuality::High, 100, LatencyMode::Auto}, {40, 3, 0}) == AudioQuality::Medium);
    std::cout << "AudioGraph tests passed.\n";
}
