#include "oamr/web/web_server.hpp"

#include "oamr/audio/audio_backend.hpp"
#include "oamr/audio/backend_factory.hpp"
#include "oamr/diagnostics/log.hpp"
#include "oamr/pairing/pairing_service.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace oamr::web {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { close(socket); }
#endif

std::string json_escape(const std::string& value) {
    std::string result;
    for (const unsigned char ch : value) {
        if (ch == '"') result += "\\\"";
        else if (ch == '\\') result += "\\\\";
        else if (ch < 0x20) result += ' ';
        else result += static_cast<char>(ch);
    }
    return result;
}

std::string url_decode(const std::string& value) {
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+' ) result += ' ';
        else if (value[index] == '%' && index + 2 < value.size()) {
            const auto hex = value.substr(index + 1, 2);
            result += static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16));
            index += 2;
        } else result += value[index];
    }
    return result;
}

std::unordered_map<std::string, std::string> query_params(const std::string& target) {
    std::unordered_map<std::string, std::string> result;
    const auto start = target.find('?');
    if (start == std::string::npos) return result;
    std::stringstream stream(target.substr(start + 1));
    std::string item;
    while (std::getline(stream, item, '&')) {
        const auto delimiter = item.find('=');
        result[url_decode(item.substr(0, delimiter))] = url_decode(delimiter == std::string::npos ? "" : item.substr(delimiter + 1));
    }
    return result;
}

std::string content_type_for(const std::filesystem::path& path) {
    const auto extension = path.extension().string();
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".js") return "text/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

std::optional<std::string> static_asset(const std::string& target, std::string& type) {
    const auto query = target.find('?');
    const std::string request_path = target.substr(0, query);
    if (request_path != "/" && request_path.rfind("/assets/", 0) != 0) return std::nullopt;

    const std::filesystem::path relative = request_path == "/" ? "index.html" : request_path.substr(1);
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized.is_absolute() || normalized.string().starts_with("..")) return std::nullopt;

    const auto file = std::filesystem::current_path() / "web" / normalized;
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) return std::nullopt;
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    type = content_type_for(file);
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

constexpr std::string_view kMissingWebAssetsPage = R"HTML(
<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OAMR Web UI unavailable</title>
<style>
  body { margin: 0; padding: 2rem; font: 16px system-ui; background: #fff8f4; color: #201b15; }
  main { max-width: 42rem; margin: 10vh auto; padding: 1.5rem; border-radius: 1.5rem; background: #f8ece2; }
  code { font-family: ui-monospace, monospace; }
</style>
<main>
  <h1>Web UI unavailable</h1>
  <p>The compiled Vite assets were not found beside OAMR.</p>
  <p>For a source build, run <code>npm ci &amp;&amp; npm run build</code> in the <code>web</code> directory, then rebuild OAMR.</p>
</main>
</html>)HTML";

bool parse_udp_port(const std::string& text, std::uint16_t& result) {
    unsigned value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || pointer != text.data() + text.size() || value == 0 || value > 65535) return false;
    result = static_cast<std::uint16_t>(value);
    return true;
}

bool network_profile_from(const std::unordered_map<std::string, std::string>& params,
                          audio::NetworkProfile& profile) {
    const auto quality = params.find("quality"), mode = params.find("mode"), latency = params.find("max-latency-ms");
    if (quality == params.end() || mode == params.end() || latency == params.end()) return false;
    if (quality->second == "low") profile.quality = audio::AudioQuality::Low;
    else if (quality->second == "medium") profile.quality = audio::AudioQuality::Medium;
    else if (quality->second == "high") profile.quality = audio::AudioQuality::High;
    else return false;
    if (mode->second == "stable") profile.mode = audio::LatencyMode::Stable;
    else if (mode->second == "auto") profile.mode = audio::LatencyMode::Auto;
    else if (mode->second == "low-latency") profile.mode = audio::LatencyMode::LowLatency;
    else return false;
    return parse_udp_port(latency->second, profile.max_latency_ms) && audio::valid_max_latency(profile.max_latency_ms);
}

std::string quality_name(audio::AudioQuality quality) {
    switch (quality) {
        case audio::AudioQuality::Low: return "low";
        case audio::AudioQuality::Medium: return "medium";
        case audio::AudioQuality::High: return "high";
    }
    return "unknown";
}

std::string mode_name(audio::LatencyMode mode) {
    switch (mode) {
        case audio::LatencyMode::Stable: return "stable";
        case audio::LatencyMode::Auto: return "auto";
        case audio::LatencyMode::LowLatency: return "low-latency";
    }
    return "unknown";
}

/** Settings of one active route; kept so a route can be rebuilt on resume or profile change. */
using RouteSettings = std::variant<audio::LoopbackSettings, audio::FanoutSettings,
                                   audio::MatrixSettings, audio::SenderSettings,
                                   audio::ReceiverSettings, audio::NetworkMixerSettings>;

} // namespace
class WebServer::Impl {
public:
    struct ActiveRoute {
        std::size_t id{};
        std::string label;
        bool enabled{true};
        RouteSettings settings;
        std::optional<audio::NetworkProfile> network_profile;
        std::unique_ptr<audio::AudioRoute> route;
    };

    std::atomic_bool stopping{false};
    std::string error;
    std::vector<ActiveRoute> routes;
    std::size_t next_route_id{1};
    pairing::PairingService pairing;
    std::unique_ptr<audio::AudioBackend> backend;
    std::chrono::steady_clock::time_point last_telemetry_sync{};

    Impl() : backend(audio::create_audio_backend()) {}

    std::unique_ptr<audio::AudioRoute> start_route(const RouteSettings& settings) {
        if (!backend) return nullptr;
        return std::visit([this](const auto& value) -> std::unique_ptr<audio::AudioRoute> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, audio::LoopbackSettings>) return backend->create_loopback(value);
            else if constexpr (std::is_same_v<T, audio::FanoutSettings>) return backend->create_fanout(value);
            else if constexpr (std::is_same_v<T, audio::MatrixSettings>) return backend->create_matrix(value);
            else if constexpr (std::is_same_v<T, audio::SenderSettings>) return backend->create_sender(value);
            else if constexpr (std::is_same_v<T, audio::ReceiverSettings>) return backend->create_receiver(value);
            else return backend->create_network_mixer(value);
        }, settings);
    }

    bool add_route(std::string label, RouteSettings settings) {
        if (!backend) { error = "No audio backend is available."; diagnostics::write(diagnostics::Level::Error, error); return false; }
        auto route = start_route(settings);
        if (!route) { error = backend->last_error(); diagnostics::write(diagnostics::Level::Error, "Could not start local route: " + error); return false; }
        diagnostics::write(diagnostics::Level::Info, "Started route: " + label);
        routes.push_back({next_route_id++, std::move(label), true, std::move(settings), std::nullopt, std::move(route)});
        return true;
    }

    bool add_network_route(std::string label, const audio::NetworkProfile& profile, RouteSettings settings) {
        if (!backend) { error = "No audio backend is available."; diagnostics::write(diagnostics::Level::Error, error); return false; }
        auto route = start_route(settings);
        if (!route) { error = backend->last_error(); diagnostics::write(diagnostics::Level::Error, "Could not start network route: " + error); return false; }
        diagnostics::write(diagnostics::Level::Info, "Started network route: " + label + " [" + quality_name(profile.quality) + ", " + std::to_string(profile.max_latency_ms) + " ms, " + mode_name(profile.mode) + "]");
        routes.push_back({next_route_id++, std::move(label), true, std::move(settings), profile, std::move(route)});
        return true;
    }

    bool set_route_enabled(std::size_t id, bool enable) {
        for (auto& item : routes) {
            if (item.id != id) continue;
            if (enable && !item.enabled) {
                auto route = start_route(item.settings);
                if (!route) { error = backend ? backend->last_error() : "No audio backend is available."; diagnostics::write(diagnostics::Level::Error, "Could not resume route " + std::to_string(id) + ": " + error); return false; }
                item.route = std::move(route);
            } else if (!enable && item.enabled && item.route) {
                item.route->stop();
            }
            item.enabled = enable;
            diagnostics::write(diagnostics::Level::Info, std::string(enable ? "Resumed route: " : "Paused route: ") + item.label);
            return true;
        }
        error = "Route " + std::to_string(id) + " was not found.";
        diagnostics::write(diagnostics::Level::Warning, error);
        return false;
    }

    bool update_network_route(std::size_t id, const audio::NetworkProfile& profile) {
        for (auto& item : routes) {
            if (item.id != id || !item.network_profile) continue;
            item.network_profile = profile;
            std::visit([&profile](auto& settings) {
                using T = std::decay_t<decltype(settings)>;
                if constexpr (std::is_same_v<T, audio::SenderSettings> || std::is_same_v<T, audio::ReceiverSettings>)
                    settings.network = profile;
                else if constexpr (std::is_same_v<T, audio::NetworkMixerSettings>)
                    for (auto& input : settings.inputs) input.network = profile;
            }, item.settings);
            if (!item.enabled) return true;
            item.route.reset();
            auto route = start_route(item.settings);
            if (!route) {
                error = backend ? backend->last_error() : "No audio backend is available.";
                item.enabled = false;
                diagnostics::write(diagnostics::Level::Error, "Could not restart route after profile update: " + error);
                return false;
            }
            item.route = std::move(route);
            diagnostics::write(diagnostics::Level::Info, "Updated network profile for route: " + item.label);
            return true;
        }
        error = "Network route " + std::to_string(id) + " was not found.";
        diagnostics::write(diagnostics::Level::Warning, error);
        return false;
    }

    bool erase_route(std::size_t id) {
        const auto old_size = routes.size();
        routes.erase(std::remove_if(routes.begin(), routes.end(), [id](ActiveRoute& item) {
            if (item.id != id) return false;
            if (item.route) item.route->stop();
            diagnostics::write(diagnostics::Level::Info, "Deleted route: " + item.label);
            return true;
        }), routes.end());
        if (routes.size() != old_size) return true;
        diagnostics::write(diagnostics::Level::Warning, "Could not delete route " + std::to_string(id) + ": route not found.");
        return false;
    }

    void stop_all_routes() {
        if (!routes.empty()) diagnostics::write(diagnostics::Level::Info, "Stopped and cleared " + std::to_string(routes.size()) + " route(s).");
        for (auto& item : routes) if (item.route) item.route->stop();
        routes.clear();
    }

    void poll_routes_and_sync_telemetry() {
        for (auto& item : routes) {
            if (item.enabled && item.route && !item.route->poll()) {
                error = item.route->last_error();
                diagnostics::write(diagnostics::Level::Error, "Route stopped after a runtime failure [" + item.label + "]: " + error);
                item.route->stop();
                item.enabled = false;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_telemetry_sync < std::chrono::seconds(2)) return;
        last_telemetry_sync = now;
        for (const auto& item : routes) {
            if (!item.enabled || !item.route || !item.network_profile) continue;
            const auto live = item.route->transport_telemetry();
            if (!live) continue;
            pairing::AudioTelemetry telemetry;
            telemetry.quality = quality_name(item.network_profile->quality);
            telemetry.target_latency_ms = item.network_profile->max_latency_ms;
            telemetry.packet_loss_percent = live->packet_loss_percent;
            telemetry.device_name = item.label;
            pairing.set_telemetry(std::move(telemetry));
            pairing.announce();
            return;
        }
    }

    bool start_remote_command(const pairing::RemoteRouteRequest& request, std::string& route_error) {
        audio::NetworkProfile profile;
        std::unordered_map<std::string, std::string> values{{"quality", request.quality},
                                                            {"mode", request.mode},
                                                            {"max-latency-ms", std::to_string(request.max_latency_ms)}};
        if (!network_profile_from(values, profile)) { route_error = "Invalid remote network profile."; diagnostics::write(diagnostics::Level::Warning, "Rejected remote route with an invalid network profile."); return false; }
        if (request.kind == pairing::RemoteRouteKind::Send) {
            audio::SenderSettings settings;
            settings.host = request.host;
            settings.port = request.port;
            settings.source_device = request.device_id;
            settings.capture_render_device = request.render_loopback;
            settings.network = profile;
            if (!add_network_route("Paired send: " + request.device_id + " → " + request.host + ":" + std::to_string(request.port), profile, settings)) { route_error = error; return false; }
        } else {
            audio::ReceiverSettings settings;
            settings.port = request.port;
            settings.sink_device = request.device_id;
            settings.network = profile;
            if (!add_network_route("Paired receive: :" + std::to_string(request.port) + " → " + request.device_id, profile, settings)) { route_error = error; return false; }
        }
        return true;
    }

    std::string devices_json() {
        std::ostringstream json;
        const auto sources = backend ? backend->list_sources() : std::vector<audio::DeviceInfo>{};
        const auto sinks = backend ? backend->list_sinks() : std::vector<audio::DeviceInfo>{};
        diagnostics::write(diagnostics::Level::Verbose, "Enumerated " + std::to_string(sources.size()) + " audio source(s) and " + std::to_string(sinks.size()) + " audio sink(s).");
        json << "{\"sources\":[";
        bool first = true;
        for (const auto& device : sources) {
            if (device.is_default) continue;
            if (!first) json << ','; first = false;
            const bool render_loopback = !device.loopback_of.empty();
            const std::string endpoint = audio::selector_device_id(render_loopback ? device.loopback_of : device.id);
            json << "{\"id\":\"" << json_escape(device.id) << "\",\"name\":\"";
            if (render_loopback) json << "\\u7cfb\\u7edf\\u64ad\\u653e \\u00b7 ";
            json << json_escape(device.name) << "\",\"renderLoopback\":" << (render_loopback ? "true" : "false")
                 << ",\"endpoint\":\"" << json_escape(endpoint) << "\"}";
        }
        json << "],\"renderSources\":[";
        first = true;
        for (const auto& device : sources) {
            if (device.is_default || device.loopback_of.empty()) continue;
            if (!first) json << ','; first = false;
            json << "{\"id\":\"" << json_escape(device.id) << "\",\"name\":\"" << json_escape(device.name) << "\"}";
        }
        json << "],\"sinks\":[";
        first = true;
        for (const auto& device : sinks) {
            if (device.is_default) continue;
            if (!first) json << ','; first = false;
            json << "{\"id\":\"" << json_escape(device.id) << "\",\"name\":\"" << json_escape(device.name)
                 << "\",\"endpoint\":\"" << json_escape(audio::selector_device_id(device.id)) << "\"}";
        }
        return json << "]}", json.str();
    }

    std::string handle(const std::string& method, const std::string& target, std::string& type) {
        type = "text/plain; charset=utf-8";
        if (method == "GET") {
            if (const auto asset = static_asset(target, type)) return *asset;
            if (target == "/" || target.starts_with("/?")) {
                type = "text/html; charset=utf-8";
                return std::string{kMissingWebAssetsPage};
            }
        }
        if (method == "GET" && target.rfind("/api/logs", 0) == 0) {
            type = "application/json; charset=utf-8";
            std::uint64_t after{};
            const auto query = query_params(target);
            if (const auto value = query.find("after"); value != query.end()) {
                const auto [pointer, parse_error] = std::from_chars(value->second.data(), value->second.data() + value->second.size(), after);
                if (parse_error != std::errc{} || pointer != value->second.data() + value->second.size()) after = 0;
            }
            std::ostringstream out; out << '['; bool first = true;
            for (const auto& entry : diagnostics::entries_after(after)) {
                if (!first) out << ','; first = false;
                const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(entry.timestamp.time_since_epoch()).count();
                out << "{\"sequence\":" << entry.sequence << ",\"timestamp\":" << timestamp
                    << ",\"level\":\"" << diagnostics::level_name(entry.level) << "\",\"message\":\""
                    << json_escape(entry.message) << "\"}";
            }
            return out << ']', out.str();
        }
        if (method == "GET" && target.rfind("/api/devices", 0) == 0) { type = "application/json; charset=utf-8"; return devices_json(); }
        if (method == "GET" && target.rfind("/api/routes", 0) == 0) {
            type = "application/json; charset=utf-8"; std::ostringstream out; out << '['; bool first = true;
            for (const auto& item : routes) { if (!first) out << ','; first = false; out << "{\"id\":" << item.id << ",\"label\":\"" << json_escape(item.label) << "\",\"enabled\":" << (item.enabled ? "true" : "false") << ",\"network\":" << (item.network_profile ? "true" : "false"); if (item.network_profile) out << ",\"quality\":\"" << quality_name(item.network_profile->quality) << "\",\"latency\":" << item.network_profile->max_latency_ms << ",\"mode\":\"" << mode_name(item.network_profile->mode) << "\""; out << "}"; }
            return out << ']', out.str();
        }
        if (method == "GET" && target.rfind("/api/pair/code", 0) == 0) return pairing.current_pair_code();
        if (method == "POST" && target.rfind("/api/pair/code", 0) == 0) return pairing.create_pair_code();
        if (method == "GET" && target.rfind("/api/pair/local", 0) == 0) { type = "application/json; charset=utf-8"; return "{\"alias\":\"" + json_escape(pairing.local_alias()) + "\"}"; }
        if (method == "GET" && target.rfind("/api/discovery", 0) == 0) {
            type = "application/json; charset=utf-8"; std::ostringstream out;
            out << "{\"enabled\":" << (pairing.discovery_enabled() ? "true" : "false") << ",\"devices\":["; bool first = true;
            for (const auto& device : pairing.discovered_peers()) { if (!first) out << ','; first = false; out << "{\"nodeId\":\"" << json_escape(device.node_id) << "\",\"alias\":\"" << json_escape(device.alias) << "\",\"host\":\"" << json_escape(device.host) << "\",\"port\":" << device.port << '}'; }
            return out << "]}", out.str();
        }
        if (method == "GET" && target.rfind("/api/pair/peers", 0) == 0) {
            type = "application/json; charset=utf-8"; std::ostringstream out; out << '['; bool first = true;
            for (const auto& peer : pairing.peers()) { if (!first) out << ','; first = false; out << "{\"nodeId\":\"" << json_escape(peer.node_id) << "\",\"alias\":\"" << json_escape(peer.alias) << "\",\"host\":\"" << json_escape(peer.host) << "\",\"port\":" << peer.port << ",\"telemetry\":{\"quality\":\"" << json_escape(peer.telemetry.quality) << "\",\"latencyMs\":" << peer.telemetry.target_latency_ms << ",\"packetLossPercent\":" << peer.telemetry.packet_loss_percent << ",\"deviceName\":\"" << json_escape(peer.telemetry.device_name) << "\"},\"endpoints\":["; bool endpoint_first = true; for (const auto& endpoint : peer.endpoints) { if (!endpoint_first) out << ','; endpoint_first = false; out << "{\"id\":\"" << json_escape(endpoint.backend_id) << "\",\"name\":\"" << json_escape(endpoint.name) << "\",\"direction\":\"" << (endpoint.direction == pairing::EndpointDirection::Source ? "source" : "sink") << "\"}"; } out << "]}"; } return out << ']', out.str();
        }
        if (method == "POST" && target.rfind("/api/pair/config", 0) == 0) {
            const auto query = query_params(target); const auto alias = query.find("alias"), endpoints = query.find("endpoints"); if (alias == query.end()) return "Missing local alias.";
            pairing.set_local_alias(alias->second); std::vector<pairing::ExposedEndpoint> exposed;
            if (endpoints != query.end()) {
                std::stringstream rows(endpoints->second); std::string row; while (std::getline(rows, row, '\n')) {
                    const auto first_tab = row.find('\t'); const auto second_tab = row.find('\t', first_tab + 1);
                    if (first_tab == std::string::npos || row.empty()) continue;
                    const auto id = row.substr(first_tab + 1, second_tab == std::string::npos ? std::string::npos : second_tab - first_tab - 1);
                    const auto name = second_tab == std::string::npos ? id : row.substr(second_tab + 1);
                    exposed.push_back({id, name, row[0] == 'S' ? pairing::EndpointDirection::Source : pairing::EndpointDirection::Sink});
                }
            }
            const auto endpoint_count = exposed.size();
            pairing.set_exposed_endpoints(std::move(exposed)); pairing.announce();
            diagnostics::write(diagnostics::Level::Info, "Saved pairing profile with " + std::to_string(endpoint_count) + " exposed endpoint(s).");
            return "Pairing profile saved and synchronized.";
        }
        if (method == "POST" && target.rfind("/api/discovery", 0) == 0) {
            const auto query = query_params(target); const auto enabled = query.find("enabled");
            if (enabled == query.end()) return "Missing discovery state.";
            if (enabled->second != "true" && enabled->second != "false") return "Invalid discovery state.";
            if (!pairing.set_discovery_enabled(enabled->second == "true")) { const auto message = "Could not change LAN discovery state: " + pairing.last_error(); diagnostics::write(diagnostics::Level::Error, message); return message; }
            diagnostics::write(diagnostics::Level::Info, enabled->second == "true" ? "LAN discovery enabled." : "LAN discovery disabled.");
            return enabled->second == "true" ? "LAN discovery enabled." : "LAN discovery disabled.";
        }
        if (method == "POST" && target.rfind("/api/pair/connect", 0) == 0) {
            const auto query = query_params(target); const auto host = query.find("host"), port = query.find("port"), alias = query.find("alias"), code = query.find("code"); std::uint16_t pairing_port{};
            if (host == query.end() || port == query.end() || alias == query.end() || code == query.end() || !parse_udp_port(port->second, pairing_port)) return "Invalid pairing request.";
            if (pairing.pair_remote(host->second, pairing_port, alias->second, code->second)) { diagnostics::write(diagnostics::Level::Info, "Paired with " + host->second + ":" + std::to_string(pairing_port) + "."); return "Pairing succeeded."; }
            const auto message = "Pairing failed for " + host->second + ":" + std::to_string(pairing_port) + ": " + pairing.last_error();
            diagnostics::write(diagnostics::Level::Error, message); return "Pairing failed: " + pairing.last_error();
        }
        if (method == "POST" && target.rfind("/api/pair/alias", 0) == 0) {
            const auto query = query_params(target); const auto node = query.find("node"), alias = query.find("alias");
            if (node == query.end() || alias == query.end() || !pairing.set_peer_alias(node->second, alias->second)) { diagnostics::write(diagnostics::Level::Warning, "Could not rename paired device: invalid or unknown node."); return "Could not rename paired device."; }
            diagnostics::write(diagnostics::Level::Info, "Renamed paired device to " + alias->second + ".");
            return "Paired device alias saved.";
        }
        if (method == "POST" && target.rfind("/api/pair/endpoint", 0) == 0) {
            const auto query = query_params(target); const auto node = query.find("node"), host = query.find("host"), port = query.find("port"); std::uint16_t pairing_port{};
            if (node == query.end() || host == query.end() || port == query.end() || !parse_udp_port(port->second, pairing_port) || !pairing.set_peer_endpoint(node->second, host->second, pairing_port)) { diagnostics::write(diagnostics::Level::Warning, "Could not update paired address: invalid endpoint or unknown node."); return "Could not update paired address."; }
            diagnostics::write(diagnostics::Level::Info, "Updated paired device address to " + host->second + ":" + std::to_string(pairing_port) + ".");
            return "Paired address saved.";
        }
        if (method == "POST" && target.rfind("/api/pair/delete", 0) == 0) {
            const auto query = query_params(target); const auto node = query.find("node");
            if (node == query.end() || !pairing.remove_peer(node->second)) { diagnostics::write(diagnostics::Level::Warning, "Could not remove paired device: unknown node."); return "Could not remove paired device."; }
            diagnostics::write(diagnostics::Level::Info, "Removed paired device " + node->second + ".");
            return "Paired device removed.";
        }
        if (method == "POST" && target.rfind("/api/paired/mixer", 0) == 0) {
            const auto query = query_params(target);
            const auto routes_param = query.find("routes");
            audio::NetworkProfile profile;
            if (routes_param == query.end() || !network_profile_from(query, profile)) return "Invalid network mixer request.";

            struct MixerRow { std::string peer_id; std::string remote_device; std::string local_sink; };
            std::vector<MixerRow> rows;
            std::stringstream stream(routes_param->second);
            std::string row;
            while (std::getline(stream, row, '\n')) {
                const auto first_tab = row.find('\t');
                const auto second_tab = row.find('\t', first_tab == std::string::npos ? 0 : first_tab + 1);
                if (first_tab == std::string::npos || second_tab == std::string::npos) continue;
                rows.push_back({row.substr(0, first_tab), row.substr(first_tab + 1, second_tab - first_tab - 1), row.substr(second_tab + 1)});
            }
            if (rows.size() < 2) return "A mixer needs at least two remote sources.";
            const std::string& sink = rows.front().local_sink;
            if (sink.empty() || std::any_of(rows.begin(), rows.end(), [&](const MixerRow& value) { return value.local_sink != sink || value.peer_id.empty() || value.remote_device.empty(); }))
                return "Mixer routes must use one valid local playback target.";

            const auto peer_list = pairing.peers();
            audio::NetworkMixerSettings settings;
            settings.sink_device = sink;
            for (std::size_t index = 0; index < rows.size(); ++index) {
                const auto peer = std::find_if(peer_list.begin(), peer_list.end(), [&](const auto& value) { return value.node_id == rows[index].peer_id; });
                if (peer == peer_list.end()) return "A selected paired source is no longer available.";
                const std::uint16_t media_port = static_cast<std::uint16_t>(52000 + ((next_route_id + index) % 1000));
                pairing::RemoteRouteRequest remote;
                remote.kind = pairing::RemoteRouteKind::Send;
                remote.device_id = rows[index].remote_device;
                remote.port = media_port;
                remote.quality = quality_name(profile.quality);
                remote.max_latency_ms = profile.max_latency_ms;
                remote.mode = mode_name(profile.mode);
                if (!pairing.request_remote_route(rows[index].peer_id, remote)) {
                    diagnostics::write(diagnostics::Level::Error, "Remote mixer sender rejected by " + peer->alias + ": " + pairing.last_error());
                    return "Remote mixer sender failed: " + pairing.last_error();
                }
                settings.inputs.push_back({media_port, profile});
            }
            if (!add_network_route("Network mixer (" + std::to_string(settings.inputs.size()) + " sources) → " + sink, profile, settings))
                return "Network mixer failed: " + error;
            return "Network mixer route added.";
        }
        if (method == "POST" && target.rfind("/api/paired/route", 0) == 0) {
            const auto query = query_params(target); const auto peer_id = query.find("peer"), kind = query.find("kind"), local_device = query.find("local"), remote_device = query.find("remote"), loopback = query.find("loopback");
            if (peer_id == query.end() || kind == query.end() || local_device == query.end() || remote_device == query.end()) return "Invalid paired matrix route.";
            audio::NetworkProfile profile; if (!network_profile_from(query, profile)) return "Invalid paired route profile.";
            const auto peer_list = pairing.peers(); const auto peer = std::find_if(peer_list.begin(), peer_list.end(), [&](const auto& value) { return value.node_id == peer_id->second; }); if (peer == peer_list.end()) return "Paired device not found.";
            const std::uint16_t media_port = static_cast<std::uint16_t>(52000 + (next_route_id % 1000));
            pairing::RemoteRouteRequest remote; remote.device_id = remote_device->second; remote.port = media_port; remote.quality = quality_name(profile.quality); remote.max_latency_ms = profile.max_latency_ms; remote.mode = mode_name(profile.mode);
            if (kind->second == "send") {
                remote.kind = pairing::RemoteRouteKind::Receive;
                if (!pairing.request_remote_route(peer_id->second, remote)) return "Remote receiver failed: " + pairing.last_error();
                audio::SenderSettings settings; settings.host = peer->host; settings.port = media_port; settings.source_device = local_device->second; settings.capture_render_device = loopback != query.end() && loopback->second == "true"; settings.network = profile;
                if (!add_network_route("Matrix send → " + peer->alias + ": " + remote.device_id, profile, settings)) return "Local sender failed: " + error;
            } else if (kind->second == "receive") {
                // Ask the source computer first. This makes the operation
                // transactional from the UI's perspective: no local table
                // entry is retained when the paired computer rejects it.
                remote.kind = pairing::RemoteRouteKind::Send;
                if (!pairing.request_remote_route(peer_id->second, remote)) return "Remote sender failed: " + pairing.last_error();
                audio::ReceiverSettings settings; settings.port = media_port; settings.sink_device = local_device->second; settings.network = profile;
                if (!add_network_route("Matrix receive → " + peer->alias + ": " + remote.device_id, profile, settings)) return "Local receiver failed: " + error;
            } else return "Invalid paired matrix direction.";
            return "Paired matrix route added.";
        }
        if (method == "POST" && target.rfind("/api/network/send", 0) == 0) {
            const auto params = query_params(target);
            const auto host = params.find("host"), port = params.find("port"), source = params.find("source");
            std::uint16_t udp_port{}; audio::NetworkProfile profile;
            if (host == params.end() || source == params.end() || port == params.end() || !parse_udp_port(port->second, udp_port)
                || !network_profile_from(params, profile)) return "Invalid network sender settings.";
            audio::SenderSettings settings;
            settings.host = host->second; settings.port = udp_port; settings.source_device = source->second;
            settings.capture_render_device = params.contains("loopback") && params.at("loopback") == "true";
            settings.network = profile;
            if (add_network_route("Send: " + source->second + " → " + settings.host + ":" + std::to_string(settings.port), profile, settings)) { pairing.set_telemetry({quality_name(profile.quality), profile.max_latency_ms, -1.0, source->second}); pairing.announce(); return "Network sender route added and telemetry synchronized."; }
            return "Sender failed: " + error;
        }
        if (method == "POST" && target.rfind("/api/network/receive", 0) == 0) {
            const auto params = query_params(target);
            const auto port = params.find("port"), sink = params.find("sink");
            std::uint16_t udp_port{}; audio::NetworkProfile profile;
            if (sink == params.end() || port == params.end() || !parse_udp_port(port->second, udp_port)
                || !network_profile_from(params, profile)) return "Invalid network receiver settings.";
            audio::ReceiverSettings settings;
            settings.port = udp_port; settings.sink_device = sink->second; settings.network = profile;
            if (add_network_route("Receive: :" + std::to_string(settings.port) + " → " + sink->second, profile, settings)) { pairing.set_telemetry({quality_name(profile.quality), profile.max_latency_ms, -1.0, sink->second}); pairing.announce(); return "Network receiver route added and telemetry synchronized."; }
            return "Receiver failed: " + error;
        }
        if (method == "POST" && target.rfind("/api/matrix", 0) == 0) {
            const auto params = query_params(target);
            const auto routes_param = params.find("routes");
            if (routes_param == params.end() || routes_param->second.empty()) return "Select at least one matrix route.";
            audio::MatrixSettings settings;
            std::unordered_map<std::string, std::size_t> sources, sinks;
            std::stringstream stream(routes_param->second); std::string line;
            while (std::getline(stream, line, '\n')) {
                const auto first_separator = line.find('\t');
                const auto second_separator = line.find('\t', first_separator + 1);
                if (first_separator == std::string::npos || second_separator == std::string::npos) continue;
                const std::string source = line.substr(0, first_separator);
                const bool is_render_loopback = line.substr(first_separator + 1, second_separator - first_separator - 1) == "true";
                const std::string sink = line.substr(second_separator + 1);
                if (is_render_loopback && audio::selector_device_id(source) == audio::selector_device_id(sink)) continue;
                const auto [source_it, source_new] = sources.emplace(source, sources.size());
                if (source_new) { settings.source_devices.push_back(source); settings.source_is_render_loopback.push_back(is_render_loopback); }
                const auto [sink_it, sink_new] = sinks.emplace(sink, sinks.size());
                if (sink_new) settings.sink_devices.push_back(sink);
                settings.routes.push_back({source_it->second, sink_it->second});
            }
            if (add_route("Local matrix (" + std::to_string(settings.routes.size()) + " links)", settings)) return "Matrix route added.";
            return "Matrix failed: " + error;
        }
        if (method == "POST" && target.rfind("/api/routes/", 0) == 0) {
            const auto slash = target.find('/', std::string_view{"/api/routes/"}.size()); const auto id_text = target.substr(std::string_view{"/api/routes/"}.size(), slash - std::string_view{"/api/routes/"}.size()); std::size_t id{};
            try { id = std::stoull(id_text); } catch (...) { return "Invalid route id."; }
            if (target.find("/toggle", slash) != std::string::npos) { const auto enabled = query_params(target).contains("enabled") && query_params(target).at("enabled") == "true"; return set_route_enabled(id, enabled) ? "Route updated." : "Could not update route: " + error; }
            if (target.find("/profile", slash) != std::string::npos) { audio::NetworkProfile profile; const auto query = query_params(target); return network_profile_from(query, profile) && update_network_route(id, profile) ? "Route properties updated." : "Could not update route properties: " + error; }
            if (target.find("/delete", slash) != std::string::npos) return erase_route(id) ? "Route deleted." : "Route not found.";
        }
        if (method == "POST" && target.rfind("/api/stop", 0) == 0) { stop_all_routes(); pairing.set_telemetry({}); pairing.announce(); return "All routes stopped and telemetry synchronized."; }
        return "Not found";
    }
};

WebServer::WebServer() : impl_(std::make_unique<Impl>()) {
    impl_->pairing.set_remote_route_handler([this](const pairing::RemoteRouteRequest& request, std::string& error) { return impl_->start_remote_command(request, error); });
}
WebServer::~WebServer() { stop(); }

bool WebServer::serve(std::uint16_t port) {
    if (!impl_->backend) { impl_->error = "No audio backend is available."; diagnostics::write(diagnostics::Level::Error, impl_->error); return false; }
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { impl_->error = "Windows sockets could not start."; diagnostics::write(diagnostics::Level::Error, impl_->error); return false; }
#endif
    if (!impl_->pairing.start(8791)) { impl_->error = "Could not start pairing service: " + impl_->pairing.last_error(); diagnostics::write(diagnostics::Level::Error, impl_->error); return false; }
    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) { impl_->error = "Could not create HTTP socket."; diagnostics::write(diagnostics::Level::Error, impl_->error); return false; }
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
        impl_->error = "Could not bind 127.0.0.1:" + std::to_string(port) + "."; diagnostics::write(diagnostics::Level::Error, impl_->error); close_socket(listener); impl_->pairing.stop(); return false;
    }
    diagnostics::write(diagnostics::Level::Info, "OAMR Web UI listening on 127.0.0.1:" + std::to_string(port) + ".");
    std::cout << "OAMR Web UI: http://127.0.0.1:" << port << "\nPress Ctrl+C to stop.\n";
    while (!impl_->stopping) {
        fd_set set; FD_ZERO(&set); FD_SET(listener, &set); timeval timeout{0, 200000};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) > 0) {
            Socket client = accept(listener, nullptr, nullptr); if (client == kInvalidSocket) continue;
            char buffer[8192]{}; const int length = recv(client, buffer, sizeof(buffer) - 1, 0);
            std::string first_line(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
            const auto end = first_line.find("\r\n"); first_line.resize(end == std::string::npos ? first_line.size() : end);
            std::istringstream request(first_line); std::string method, target; request >> method >> target;
            std::string type; const std::string body = impl_->handle(method, target, type);
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            send(client, response.data(), static_cast<int>(response.size()), 0); close_socket(client);
        }
        impl_->poll_routes_and_sync_telemetry();
    }
    close_socket(listener);
    impl_->pairing.stop();
    diagnostics::write(diagnostics::Level::Info, "OAMR Web UI stopped.");
#ifdef _WIN32
    WSACleanup();
#endif
    return true;
}

void WebServer::stop() noexcept { impl_->stopping = true; impl_->stop_all_routes(); impl_->pairing.stop(); }
const std::string& WebServer::last_error() const noexcept { return impl_->error; }

} // namespace oamr::web
