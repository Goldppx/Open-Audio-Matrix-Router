#include "oamr/core/audio_graph.hpp"
#include "oamr/audio/audio_types.hpp"
#include "oamr/diagnostics/log.hpp"

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

    // Network profile resolution and adaptation are shared with every backend.
    const auto low_latency = oamr::audio::resolve_network_profile(
        {oamr::audio::AudioQuality::High, 40, oamr::audio::LatencyMode::LowLatency});
    assert(low_latency && low_latency->opus_bitrate_bps == 160000 && low_latency->opus_frame_ms == 5);
    assert(low_latency->drop_on_latency && low_latency->jitter_buffer_ms <= 25);
    const auto stable = oamr::audio::resolve_network_profile(
        {oamr::audio::AudioQuality::Low, 150, oamr::audio::LatencyMode::Stable});
    assert(stable && stable->opus_bitrate_bps == 48000 && stable->inband_fec && !stable->drop_on_latency);
    assert(!oamr::audio::resolve_network_profile(
        {oamr::audio::AudioQuality::Medium, 75, oamr::audio::LatencyMode::Auto}));
    assert(oamr::audio::adapt_quality(oamr::audio::AudioQuality::High,
        {oamr::audio::AudioQuality::High, 60, oamr::audio::LatencyMode::Auto}, {61, 4, 0})
        == oamr::audio::AudioQuality::Medium);
    assert(oamr::audio::adapt_quality(oamr::audio::AudioQuality::Low,
        {oamr::audio::AudioQuality::High, 100, oamr::audio::LatencyMode::Auto}, {40, 3, 0})
        == oamr::audio::AudioQuality::Medium);

    diagnostics::write(diagnostics::Level::Info, "diagnostic-test");
    const auto diagnostic_entries = diagnostics::entries_after(0);
    assert(!diagnostic_entries.empty());
    assert(diagnostic_entries.back().message == "diagnostic-test");
    assert(std::string{diagnostics::level_name(diagnostic_entries.back().level)} == "INFO");
    assert(diagnostics::entries_after(diagnostic_entries.back().sequence).empty());
    std::cout << "AudioGraph tests passed.\n";
}
