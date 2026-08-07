#include "oamr/gstreamer/device_enumerator.hpp"
#include "oamr/gstreamer/rtp_opus_pipeline.hpp"
#include "oamr/web/web_server.hpp"

#include <csignal>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
volatile std::sig_atomic_t keep_running = 1;
void stop_handler(int) { keep_running = 0; }

void usage() {
    std::cout << "OAMR command-line MVP\n\n"
              << "  oamr devices\n"
              << "  oamr web [--port <1-65535>]\n"
              << "  oamr loopback [--source-device <backend-device>] [--sink-device <backend-device>] [--render-loopback]\n"
              << "  oamr fanout --sink-device <backend-device> [--sink-device <backend-device> ...] [--source-device <backend-device>]\n"
              << "  oamr send --host <IPv4-or-DNS> --port <1-65535> [--device <backend-device>] [network options]\n"
              << "  oamr receive --port <1-65535> [--device <backend-device>] [network options]\n\n"
              << "  network options: --quality <low|medium|high> --max-latency-ms <40|60|100|150>\n"
              << "                   --mode <stable|auto|low-latency>\n";
}

bool option_value(int argc, char** argv, const std::string& option, std::string& value) {
    for (int index = 2; index + 1 < argc; ++index) {
        if (argv[index] == option) { value = argv[index + 1]; return true; }
    }
    return false;
}

std::vector<std::string> option_values(int argc, char** argv, const std::string& option) {
    std::vector<std::string> values;
    for (int index = 2; index + 1 < argc; ++index)
        if (argv[index] == option) values.emplace_back(argv[index + 1]);
    return values;
}

bool has_option(int argc, char** argv, const std::string& option) {
    for (int index = 2; index < argc; ++index)
        if (argv[index] == option) return true;
    return false;
}

bool parse_port(const std::string& text, std::uint16_t& port) {
    try {
        const unsigned long number = std::stoul(text);
        if (number == 0 || number > 65535) return false;
        port = static_cast<std::uint16_t>(number);
        return true;
    } catch (...) { return false; }
}

bool parse_network_profile(int argc, char** argv, oamr::gstreamer::NetworkAudioProfile& profile) {
    std::string value;
    if (option_value(argc, argv, "--quality", value)) {
        if (value == "low") profile.quality = oamr::gstreamer::AudioQuality::Low;
        else if (value == "medium") profile.quality = oamr::gstreamer::AudioQuality::Medium;
        else if (value == "high") profile.quality = oamr::gstreamer::AudioQuality::High;
        else return false;
    }
    if (option_value(argc, argv, "--mode", value)) {
        if (value == "stable") profile.mode = oamr::gstreamer::LatencyMode::Stable;
        else if (value == "auto") profile.mode = oamr::gstreamer::LatencyMode::Auto;
        else if (value == "low-latency") profile.mode = oamr::gstreamer::LatencyMode::LowLatency;
        else return false;
    }
    if (option_value(argc, argv, "--max-latency-ms", value)) {
        std::uint16_t latency{};
        if (!parse_port(value, latency) || !oamr::gstreamer::valid_max_latency(latency)) return false;
        profile.max_latency_ms = latency;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // GStreamer reports device labels as UTF-8. This also covers callers that
    // invoke oamr.exe directly instead of using the development launcher.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    if (argc < 2) { usage(); return 1; }
    const std::string command = argv[1];
    if (command == "devices") {
        oamr::gstreamer::DeviceEnumerator enumerator;
        for (const auto& device : enumerator.list_capture_devices())
            std::cout << "source  " << device.name << (device.selectable ? "" : " (default only)") << "\n  id: " << device.backend_id << "\n";
        for (const auto& device : enumerator.list_playback_devices())
            std::cout << "sink    " << device.name << (device.selectable ? "" : " (default only)") << "\n  id: " << device.backend_id << "\n";
        return 0;
    }

    if (command == "web") {
        std::string port_text;
        std::uint16_t port = 8787;
        if (option_value(argc, argv, "--port", port_text) && !parse_port(port_text, port)) {
            std::cerr << "Invalid HTTP port.\n";
            return 1;
        }
        oamr::web::WebServer server;
        if (!server.serve(port)) {
            std::cerr << "Could not start web UI: " << server.last_error() << "\n";
            return 2;
        }
        return 0;
    }

    if (command == "loopback") {
        std::string source_device, sink_device;
        option_value(argc, argv, "--source-device", source_device);
        option_value(argc, argv, "--sink-device", sink_device);
        const bool capture_render_device = has_option(argc, argv, "--render-loopback");
        oamr::gstreamer::RtpOpusPipeline pipeline;
        if (!pipeline.start_loopback({source_device, sink_device, capture_render_device})) {
            std::cerr << "Could not start: " << pipeline.last_error() << "\n";
            return 2;
        }
        std::cout << "Running. Press Ctrl+C to stop.\n";
        std::signal(SIGINT, stop_handler);
        std::signal(SIGTERM, stop_handler);
        while (keep_running && pipeline.poll()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        if (!pipeline.last_error().empty()) std::cerr << "Stopped: " << pipeline.last_error() << "\n";
        pipeline.stop();
        return 0;
    }

    if (command == "fanout") {
        std::string source_device;
        option_value(argc, argv, "--source-device", source_device);
        const auto sink_devices = option_values(argc, argv, "--sink-device");
        if (sink_devices.empty()) { usage(); return 1; }
        oamr::gstreamer::RtpOpusPipeline pipeline;
        if (!pipeline.start_local_fanout({source_device, sink_devices})) {
            std::cerr << "Could not start: " << pipeline.last_error() << "\n";
            return 2;
        }
        std::cout << "Running local fanout to " << sink_devices.size() << " sink(s). Press Ctrl+C to stop.\n";
        std::signal(SIGINT, stop_handler);
        std::signal(SIGTERM, stop_handler);
        while (keep_running && pipeline.poll()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        if (!pipeline.last_error().empty()) std::cerr << "Stopped: " << pipeline.last_error() << "\n";
        pipeline.stop();
        return 0;
    }

    std::string port_text;
    if (!option_value(argc, argv, "--port", port_text)) { usage(); return 1; }
    std::uint16_t port{};
    if (!parse_port(port_text, port)) { std::cerr << "Invalid UDP port.\n"; return 1; }
    std::string device;
    option_value(argc, argv, "--device", device);
    oamr::gstreamer::NetworkAudioProfile network;
    if (!parse_network_profile(argc, argv, network)) {
        std::cerr << "Invalid network profile.\n";
        return 1;
    }

    oamr::gstreamer::RtpOpusPipeline pipeline;
    bool started = false;
    if (command == "send") {
        std::string host;
        if (!option_value(argc, argv, "--host", host)) { usage(); return 1; }
        oamr::gstreamer::SenderSettings settings{host, port, device};
        settings.network = network;
        started = pipeline.start_sender(settings);
    } else if (command == "receive") {
        std::string latency_text;
        oamr::gstreamer::ReceiverSettings settings{port, device};
        settings.network = network;
        if (option_value(argc, argv, "--latency-ms", latency_text)) {
            std::uint16_t latency{};
            if (!parse_port(latency_text, latency)) { std::cerr << "Invalid latency.\n"; return 1; }
            settings.jitter_buffer_ms = latency;
        }
        started = pipeline.start_receiver(settings);
    } else { usage(); return 1; }

    if (!started) { std::cerr << "Could not start: " << pipeline.last_error() << "\n"; return 2; }
    if (const auto resolved = pipeline.resolved_network_profile())
        std::cout << "Network: " << resolved->opus_bitrate_bps << " bps, " << resolved->opus_frame_ms
                  << " ms frames, " << resolved->jitter_buffer_ms << " ms jitter buffer.\n";
    std::cout << "Running. Press Ctrl+C to stop.\n";
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    while (keep_running && pipeline.poll()) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    if (!pipeline.last_error().empty()) std::cerr << "Stopped: " << pipeline.last_error() << "\n";
    pipeline.stop();
    return 0;
}
