#include "oamr/audio/audio_backend.hpp"
#include "oamr/audio/backend_factory.hpp"
#include "oamr/web/web_server.hpp"

#include <csignal>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
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

bool parse_network_profile(int argc, char** argv, oamr::audio::NetworkProfile& profile) {
    std::string value;
    if (option_value(argc, argv, "--quality", value)) {
        if (value == "low") profile.quality = oamr::audio::AudioQuality::Low;
        else if (value == "medium") profile.quality = oamr::audio::AudioQuality::Medium;
        else if (value == "high") profile.quality = oamr::audio::AudioQuality::High;
        else return false;
    }
    if (option_value(argc, argv, "--mode", value)) {
        if (value == "stable") profile.mode = oamr::audio::LatencyMode::Stable;
        else if (value == "auto") profile.mode = oamr::audio::LatencyMode::Auto;
        else if (value == "low-latency") profile.mode = oamr::audio::LatencyMode::LowLatency;
        else return false;
    }
    if (option_value(argc, argv, "--max-latency-ms", value)) {
        std::uint16_t latency{};
        if (!parse_port(value, latency) || !oamr::audio::valid_max_latency(latency)) return false;
        profile.max_latency_ms = latency;
    }
    return true;
}

std::unique_ptr<oamr::audio::AudioBackend> load_backend() {
    auto backend = oamr::audio::create_audio_backend();
    if (!backend) std::cerr << "No audio backend is available in this build.\n";
    return backend;
}

void run_route(std::unique_ptr<oamr::audio::AudioRoute> route) {
    if (!route) return;
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    while (keep_running && route->poll()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!route->last_error().empty()) std::cerr << "Stopped: " << route->last_error() << "\n";
    route->stop();
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
        auto backend = load_backend();
        if (!backend) return 2;
        for (const auto& device : backend->list_sources())
            std::cout << "source  " << device.name << (device.is_default ? " (default only)" : "") << "\n  id: " << device.id << "\n";
        for (const auto& device : backend->list_sinks())
            std::cout << "sink    " << device.name << (device.is_default ? " (default only)" : "") << "\n  id: " << device.id << "\n";
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

    auto backend = load_backend();
    if (!backend) return 2;

    if (command == "loopback") {
        std::string source_device, sink_device;
        option_value(argc, argv, "--source-device", source_device);
        option_value(argc, argv, "--sink-device", sink_device);
        oamr::audio::LoopbackSettings settings;
        settings.source_device = source_device;
        settings.sink_device = sink_device;
        settings.capture_render_device = has_option(argc, argv, "--render-loopback");
        auto route = backend->create_loopback(settings);
        if (!route) {
            std::cerr << "Could not start: " << backend->last_error() << "\n";
            return 2;
        }
        std::cout << "Running. Press Ctrl+C to stop.\n";
        run_route(std::move(route));
        return 0;
    }

    if (command == "fanout") {
        std::string source_device;
        option_value(argc, argv, "--source-device", source_device);
        const auto sink_devices = option_values(argc, argv, "--sink-device");
        if (sink_devices.empty()) { usage(); return 1; }
        oamr::audio::FanoutSettings settings;
        settings.source_device = source_device;
        settings.sink_devices = sink_devices;
        auto route = backend->create_fanout(settings);
        if (!route) {
            std::cerr << "Could not start: " << backend->last_error() << "\n";
            return 2;
        }
        std::cout << "Running local fanout to " << sink_devices.size() << " sink(s). Press Ctrl+C to stop.\n";
        run_route(std::move(route));
        return 0;
    }

    std::string port_text;
    if (!option_value(argc, argv, "--port", port_text)) { usage(); return 1; }
    std::uint16_t port{};
    if (!parse_port(port_text, port)) { std::cerr << "Invalid UDP port.\n"; return 1; }
    std::string device;
    option_value(argc, argv, "--device", device);
    oamr::audio::NetworkProfile network;
    if (!parse_network_profile(argc, argv, network)) {
        std::cerr << "Invalid network profile.\n";
        return 1;
    }

    std::unique_ptr<oamr::audio::AudioRoute> route;
    if (command == "send") {
        std::string host;
        if (!option_value(argc, argv, "--host", host)) { usage(); return 1; }
        oamr::audio::SenderSettings settings;
        settings.host = host;
        settings.port = port;
        settings.source_device = device;
        settings.network = network;
        route = backend->create_sender(settings);
    } else if (command == "receive") {
        oamr::audio::ReceiverSettings settings;
        settings.port = port;
        settings.sink_device = device;
        settings.network = network;
        route = backend->create_receiver(settings);
    } else { usage(); return 1; }

    if (!route) { std::cerr << "Could not start: " << backend->last_error() << "\n"; return 2; }
    if (const auto resolved = oamr::audio::resolve_network_profile(network))
        std::cout << "Network: " << resolved->opus_bitrate_bps << " bps, " << resolved->opus_frame_ms
                  << " ms frames, " << resolved->jitter_buffer_ms << " ms jitter buffer.\n";
    std::cout << "Running. Press Ctrl+C to stop.\n";
    run_route(std::move(route));
    return 0;
}
