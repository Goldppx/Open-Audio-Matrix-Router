#include "oamr/pairing/pairing_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace oamr::pairing {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
constexpr std::uint16_t kDiscoveryPort = 8792;
constexpr char kDiscoveryGroup[] = "239.255.79.77";
#ifdef _WIN32
using Socket = SOCKET; constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
#else
using Socket = int; constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { close(socket); }
#endif

struct IPv4Interface {
    in_addr address{};
    in_addr netmask{};
};

std::vector<IPv4Interface> active_ipv4_interfaces(Socket socket) {
    std::vector<IPv4Interface> result;
#ifdef _WIN32
    INTERFACE_INFO interfaces[64]{};
    DWORD bytes{};
    if (WSAIoctl(socket, SIO_GET_INTERFACE_LIST, nullptr, 0, interfaces,
                 sizeof(interfaces), &bytes, nullptr, nullptr) == SOCKET_ERROR) return result;
    const auto count = bytes / sizeof(INTERFACE_INFO);
    for (DWORD index = 0; index < count; ++index) {
        const auto flags = interfaces[index].iiFlags;
        if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0) continue;
        const auto* address = reinterpret_cast<const sockaddr_in*>(&interfaces[index].iiAddress);
        const auto* netmask = reinterpret_cast<const sockaddr_in*>(&interfaces[index].iiNetmask);
        if (address->sin_family == AF_INET && address->sin_addr.s_addr != INADDR_ANY)
            result.push_back({address->sin_addr, netmask->sin_addr});
    }
#else
    ifaddrs* first{};
    if (getifaddrs(&first) != 0) return result;
    for (auto* entry = first; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) continue;
        const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        const auto* netmask = reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask);
        result.push_back({address->sin_addr, netmask ? netmask->sin_addr : in_addr{}});
    }
    freeifaddrs(first);
#endif
    return result;
}

std::string env_or(const char* name, const char* fallback) {
#ifdef _WIN32
    char* value = nullptr; std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) == 0 && value) { std::string result(value); std::free(value); return result; }
    return fallback;
#else
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : std::string(fallback);
#endif
}

std::string escape(const std::string& value) {
    std::ostringstream out; out << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out << c;
        else out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return out.str();
}
std::string unescape(const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) { unsigned n{}; std::istringstream(value.substr(i + 1, 2)) >> std::hex >> n; out += static_cast<char>(n); i += 2; }
        else if (value[i] == '+') out += ' '; else out += value[i];
    }
    return out;
}
std::map<std::string, std::string> params(const std::string& target) {
    std::map<std::string, std::string> result; const auto q = target.find('?'); if (q == std::string::npos) return result;
    std::stringstream stream(target.substr(q + 1)); std::string part;
    while (std::getline(stream, part, '&')) { const auto p = part.find('='); result[unescape(part.substr(0, p))] = unescape(p == std::string::npos ? "" : part.substr(p + 1)); }
    return result;
}
std::string random_hex(std::size_t length) {
    std::random_device rd; std::mt19937 gen(rd()); std::uniform_int_distribution<int> dist(0, 15); std::string result; result.reserve(length);
    constexpr char digits[] = "0123456789abcdef"; while (result.size() < length) result += digits[dist(gen)]; return result;
}
std::string serialize(const std::vector<ExposedEndpoint>& endpoints) {
    std::ostringstream out; bool first = true;
    for (const auto& endpoint : endpoints) { if (!first) out << ';'; first = false; out << (endpoint.direction == EndpointDirection::Source ? 'S' : 'K') << ',' << escape(endpoint.name) << ',' << escape(endpoint.backend_id); }
    return out.str();
}
std::vector<ExposedEndpoint> deserialize(const std::string& text) {
    std::vector<ExposedEndpoint> result; std::stringstream rows(text); std::string row;
    while (std::getline(rows, row, ';')) { const auto a = row.find(','), b = row.find(',', a + 1); if (a == std::string::npos || b == std::string::npos) continue;
        result.push_back({unescape(row.substr(b + 1)), unescape(row.substr(a + 1, b - a - 1)), row[0] == 'S' ? EndpointDirection::Source : EndpointDirection::Sink}); }
    return result;
}
std::string serialize_telemetry(const AudioTelemetry& telemetry) {
    std::ostringstream out;
    out << escape(telemetry.quality) << ',' << telemetry.target_latency_ms << ',' << telemetry.packet_loss_percent << ',' << escape(telemetry.device_name);
    return out.str();
}
AudioTelemetry deserialize_telemetry(const std::string& text) {
    AudioTelemetry telemetry; std::stringstream fields(text); std::string quality, latency, loss, name;
    std::getline(fields, quality, ','); std::getline(fields, latency, ','); std::getline(fields, loss, ','); std::getline(fields, name);
    telemetry.quality = unescape(quality); telemetry.device_name = unescape(name);
    try { telemetry.target_latency_ms = static_cast<std::uint16_t>(std::stoul(latency)); telemetry.packet_loss_percent = std::stod(loss); } catch (...) {}
    return telemetry;
}

struct HttpReply {
    bool ok{};
    std::string body;
    std::string error;
};

HttpReply post_control(const std::string& host, std::uint16_t port, const std::string& target) {
    try {
        asio::io_context context;
        tcp::resolver resolver(context);
        beast::tcp_stream stream(context);
        stream.expires_after(std::chrono::seconds(3));
        const auto endpoints = resolver.resolve(host, std::to_string(port));
        stream.connect(endpoints);

        http::request<http::empty_body> request{http::verb::post, target, 11};
        request.set(http::field::host, host);
        request.set(http::field::user_agent, "OAMR-Control/1");
        request.set(http::field::connection, "close");
        stream.expires_after(std::chrono::seconds(3));
        http::write(stream, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        stream.expires_after(std::chrono::seconds(8));
        http::read(stream, buffer, response);
        beast::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        if (response.result_int() < 200 || response.result_int() >= 300)
            return {false, response.body(), "Remote control returned HTTP " + std::to_string(response.result_int()) + "."};
        return {true, response.body(), {}};
    } catch (const std::exception& error) {
        return {false, {}, error.what()};
    }
}
}

class PairingService::Impl {
public:
    mutable std::mutex mutex;
    std::atomic_bool stopping{false}; std::thread worker; Socket listener{kInvalidSocket}; std::uint16_t listen_port{};
    std::atomic_bool discovery_stopping{true}; std::thread discovery_worker; Socket discovery_socket{kInvalidSocket}; bool discovery_enabled{};
    std::vector<IPv4Interface> discovery_interfaces;
    std::string error, node_id{random_hex(24)}, alias{env_or("COMPUTERNAME", "This computer")}, pair_code; std::chrono::steady_clock::time_point code_expiry{};
    std::vector<ExposedEndpoint> exposed; AudioTelemetry local_telemetry; std::map<std::string, RemotePeer> known_peers;
    struct DiscoveryEntry { DiscoveredPeer peer; std::chrono::steady_clock::time_point seen; };
    mutable std::map<std::string, DiscoveryEntry> discovered;
    std::function<bool(const RemoteRouteRequest&, std::string&)> route_handler;

    std::string generate_pair_code_unlocked() {
        pair_code = random_hex(6);
        std::transform(pair_code.begin(), pair_code.end(), pair_code.begin(), [](unsigned char character) {
            return static_cast<char>(::toupper(character));
        });
        code_expiry = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        return pair_code;
    }

    std::filesystem::path state_path() const { return std::filesystem::current_path() / "oamr-pairing-state.txt"; }
    void save_unlocked() const {
        std::ofstream file(state_path(), std::ios::trunc);
        if (!file) return;
        file << "node\t" << escape(node_id) << "\n";
        file << "alias\t" << escape(alias) << "\n";
        file << "discovery\t" << (discovery_enabled ? "1" : "0") << "\n";
        for (const auto& endpoint : exposed)
            file << "endpoint\t" << (endpoint.direction == EndpointDirection::Source ? 'S' : 'K') << '\t' << escape(endpoint.backend_id) << '\t' << escape(endpoint.name) << "\n";
        file << "telemetry\t" << escape(serialize_telemetry(local_telemetry)) << "\n";
        for (const auto& [id, peer] : known_peers)
            file << "peer\t" << escape(id) << '\t' << escape(peer.alias) << '\t' << escape(peer.host) << '\t' << peer.port << '\t' << escape(serialize(peer.endpoints)) << '\t' << escape(serialize_telemetry(peer.telemetry)) << "\n";
    }
    void load() {
        std::ifstream file(state_path()); std::string row;
        while (std::getline(file, row)) {
            std::vector<std::string> fields; std::stringstream parts(row); std::string part;
            while (std::getline(parts, part, '\t')) fields.push_back(part);
            if (fields.size() >= 2 && fields[0] == "node") node_id = unescape(fields[1]);
            else if (fields.size() >= 2 && fields[0] == "alias") alias = unescape(fields[1]);
            else if (fields.size() >= 2 && fields[0] == "discovery") discovery_enabled = fields[1] == "1";
            else if (fields.size() >= 4 && fields[0] == "endpoint") exposed.push_back({unescape(fields[2]), unescape(fields[3]), fields[1] == "S" ? EndpointDirection::Source : EndpointDirection::Sink});
            else if (fields.size() >= 2 && fields[0] == "telemetry") local_telemetry = deserialize_telemetry(unescape(fields[1]));
            else if (fields.size() >= 6 && fields[0] == "peer") { try { RemotePeer peer{unescape(fields[1]), unescape(fields[2]), unescape(fields[3]), static_cast<std::uint16_t>(std::stoul(fields[4])), deserialize(unescape(fields[5]))}; if (fields.size() >= 7) peer.telemetry = deserialize_telemetry(unescape(fields[6])); known_peers[peer.node_id] = std::move(peer); } catch (...) {} }
        }
    }

    void discovery_loop() {
        auto next_announcement = std::chrono::steady_clock::time_point{};
        while (!discovery_stopping) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_announcement) {
                std::string node, local_alias; std::uint16_t port{};
                { std::lock_guard lock(mutex); node = node_id; local_alias = alias; port = listen_port; }
                const std::string message = "OAMR-DISCOVERY/1?node=" + escape(node) + "&alias=" + escape(local_alias) + "&port=" + std::to_string(port);
                sockaddr_in target{}; target.sin_family = AF_INET; target.sin_port = htons(kDiscoveryPort); inet_pton(AF_INET, kDiscoveryGroup, &target.sin_addr);
                for (const auto& interface : discovery_interfaces) {
                    setsockopt(discovery_socket, IPPROTO_IP, IP_MULTICAST_IF,
                               reinterpret_cast<const char*>(&interface.address), sizeof(interface.address));
                    sendto(discovery_socket, message.data(), static_cast<int>(message.size()), 0,
                           reinterpret_cast<sockaddr*>(&target), sizeof(target));

                    // Directed broadcast is a fallback for LANs or Windows network
                    // profiles that filter multicast while still allowing UDP broadcast.
                    if (interface.netmask.s_addr != INADDR_ANY) {
                        sockaddr_in broadcast = target;
                        broadcast.sin_addr.s_addr = interface.address.s_addr | ~interface.netmask.s_addr;
                        sendto(discovery_socket, message.data(), static_cast<int>(message.size()), 0,
                               reinterpret_cast<sockaddr*>(&broadcast), sizeof(broadcast));
                    }
                }
                next_announcement = now + std::chrono::seconds(2);
            }
            fd_set set; FD_ZERO(&set); FD_SET(discovery_socket, &set); timeval timeout{0, 200000};
            if (select(static_cast<int>(discovery_socket + 1), &set, nullptr, nullptr, &timeout) <= 0) continue;
            char buffer[1024]{}; sockaddr_in sender{}; socklen_t size = sizeof(sender);
            const int received = recvfrom(discovery_socket, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&sender), &size);
            if (received <= 0) continue;
            const std::string message(buffer, static_cast<std::size_t>(received));
            constexpr std::string_view prefix{"OAMR-DISCOVERY/1?"};
            if (!message.starts_with(prefix)) continue;
            const auto query = params("?" + message.substr(prefix.size()));
            const auto node = query.find("node"), remote_alias = query.find("alias"), remote_port = query.find("port");
            if (node == query.end() || remote_alias == query.end() || remote_port == query.end()) continue;
            unsigned port{}; try { port = static_cast<unsigned>(std::stoul(remote_port->second)); } catch (...) { continue; }
            if (port == 0 || port > 65535) continue;
            char host[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET, &sender.sin_addr, host, sizeof(host));
            std::lock_guard lock(mutex);
            if (node->second != node_id) discovered[node->second] = {{node->second, remote_alias->second, host, static_cast<std::uint16_t>(port)}, std::chrono::steady_clock::now()};
        }
    }

    bool start_discovery() {
        if (discovery_socket != kInvalidSocket) return true;
        discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (discovery_socket == kInvalidSocket) { error = "Could not create discovery socket."; return false; }
        int reuse = 1; setsockopt(discovery_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        int broadcast = 1; setsockopt(discovery_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(kDiscoveryPort); address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(discovery_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { error = "Could not bind discovery port."; close_socket(discovery_socket); discovery_socket = kInvalidSocket; return false; }
        discovery_interfaces = active_ipv4_interfaces(discovery_socket);
        in_addr group{}; inet_pton(AF_INET, kDiscoveryGroup, &group);
        std::size_t joined{};
        for (const auto& interface : discovery_interfaces) {
            ip_mreq membership{group, interface.address};
            if (setsockopt(discovery_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           reinterpret_cast<const char*>(&membership), sizeof(membership)) == 0) ++joined;
        }
        if (discovery_interfaces.empty() || joined == 0) {
            in_addr any{};
            any.s_addr = htonl(INADDR_ANY);
            ip_mreq membership{group, any};
            if (setsockopt(discovery_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0) {
                error = "Could not join discovery multicast group."; close_socket(discovery_socket); discovery_socket = kInvalidSocket; return false;
            }
            discovery_interfaces.push_back({any, any});
        }
        unsigned char ttl = 1; setsockopt(discovery_socket, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
        discovery_stopping = false; discovery_worker = std::thread([this] { discovery_loop(); }); return true;
    }

    void stop_discovery() noexcept {
        discovery_stopping = true;
        if (discovery_socket != kInvalidSocket) { close_socket(discovery_socket); discovery_socket = kInvalidSocket; }
        if (discovery_worker.joinable()) discovery_worker.join();
        discovery_interfaces.clear();
        std::lock_guard lock(mutex); discovered.clear();
    }

    std::string handle(const std::string& target, const std::string& host) {
        const auto query = params(target);
        if (target.rfind("/update?", 0) == 0) {
            const auto node = query.find("node"), peer_alias = query.find("alias"), peer_port = query.find("port"), catalog = query.find("catalog"), telemetry = query.find("telemetry");
            if (node == query.end() || peer_alias == query.end() || peer_port == query.end() || catalog == query.end() || telemetry == query.end()) return "error=invalid-update";
            std::lock_guard lock(mutex); const auto it = known_peers.find(node->second);
            if (it == known_peers.end()) return "error=unpaired-peer";
            try { it->second.alias = peer_alias->second; it->second.host = host; it->second.port = static_cast<std::uint16_t>(std::stoul(peer_port->second)); it->second.endpoints = deserialize(catalog->second); it->second.telemetry = deserialize_telemetry(telemetry->second); save_unlocked(); return "ok"; } catch (...) { return "error=invalid-update"; }
        }
        if (target.rfind("/route?", 0) == 0) {
            const auto node = query.find("node"), kind = query.find("kind"), device = query.find("device"), port = query.find("port"), quality = query.find("quality"), latency = query.find("latency"), mode = query.find("mode");
            if (node == query.end() || kind == query.end() || device == query.end() || port == query.end() || quality == query.end() || latency == query.end() || mode == query.end()) return "error=invalid-route";
            RemoteRouteRequest request; std::function<bool(const RemoteRouteRequest&, std::string&)> handler;
            { std::lock_guard lock(mutex); const auto peer = known_peers.find(node->second); if (peer == known_peers.end()) return "error=unpaired-peer"; try { request.kind = kind->second == "send" ? RemoteRouteKind::Send : RemoteRouteKind::Receive; request.device_id = device->second; request.host = peer->second.host; request.port = static_cast<std::uint16_t>(std::stoul(port->second)); request.quality = quality->second; request.max_latency_ms = static_cast<std::uint16_t>(std::stoul(latency->second)); request.mode = mode->second; request.render_loopback = query.contains("loopback") && query.at("loopback") == "true"; handler = route_handler; } catch (...) { return "error=invalid-route"; } }
            std::string failure; return handler && handler(request, failure) ? "ok" : "error=" + escape(failure.empty() ? "route-not-available" : failure);
        }
        if (target.rfind("/unpair?", 0) == 0) {
            const auto node = query.find("node"); if (node == query.end()) return "error=invalid-request";
            std::lock_guard lock(mutex); known_peers.erase(node->second); save_unlocked(); return "ok";
        }
        if (target.rfind("/pair?", 0) != 0) return "error=not-found";
        const auto code = query.find("code"), node = query.find("node"), peer_alias = query.find("alias"), peer_port = query.find("port");
        if (code == query.end() || node == query.end() || peer_alias == query.end() || peer_port == query.end()) return "error=invalid-request";
        std::lock_guard lock(mutex);
        if (pair_code.empty() || std::chrono::steady_clock::now() > code_expiry || code->second != pair_code) return "error=invalid-or-expired-code";
        unsigned port{}; try { port = static_cast<unsigned>(std::stoul(peer_port->second)); } catch (...) { return "error=invalid-port"; }
        if (port == 0 || port > 65535) return "error=invalid-port";
        pair_code.clear();
        known_peers[node->second] = {node->second, peer_alias->second, host, static_cast<std::uint16_t>(port), {}};
        save_unlocked();
        return "node=" + escape(node_id) + "&alias=" + escape(alias) + "&catalog=" + escape(serialize(exposed));
    }

    void serve() {
        while (!stopping) {
            fd_set set; FD_ZERO(&set); FD_SET(listener, &set); timeval timeout{0, 200000};
            if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0) continue;
            sockaddr_in peer{}; socklen_t size = sizeof(peer); Socket client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &size); if (client == kInvalidSocket) continue;
            char buffer[4096]{}; const int received = recv(client, buffer, sizeof(buffer) - 1, 0); std::string line(buffer, received > 0 ? static_cast<std::size_t>(received) : 0);
            const auto end = line.find("\r\n"); line.resize(end == std::string::npos ? line.size() : end); std::istringstream request(line); std::string method, target; request >> method >> target;
            char host_text[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET, &peer.sin_addr, host_text, sizeof(host_text));
            const std::string body = method == "POST" ? handle(target, host_text) : "error=method";
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            send(client, response.data(), static_cast<int>(response.size()), 0); close_socket(client);
        }
    }
};

PairingService::PairingService() : impl_(std::make_unique<Impl>()) { impl_->load(); }
PairingService::~PairingService() { stop(); }
bool PairingService::start(std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { impl_->error = "Windows sockets could not start."; return false; }
#endif
    impl_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (impl_->listener == kInvalidSocket) { impl_->error = "Could not create pairing socket."; return false; }
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(impl_->listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(impl_->listener, 8) != 0) { impl_->error = "Could not bind pairing port."; close_socket(impl_->listener); impl_->listener = kInvalidSocket; return false; }
    impl_->listen_port = port; impl_->stopping = false; impl_->worker = std::thread([this] { impl_->serve(); });
    if (impl_->discovery_enabled && !impl_->start_discovery()) impl_->discovery_enabled = false;
    return true;
}
void PairingService::stop() noexcept { impl_->stop_discovery(); impl_->stopping = true; if (impl_->listener != kInvalidSocket) { close_socket(impl_->listener); impl_->listener = kInvalidSocket; } if (impl_->worker.joinable()) impl_->worker.join(); }
const std::string& PairingService::last_error() const noexcept { return impl_->error; }
std::uint16_t PairingService::port() const noexcept { return impl_->listen_port; }
void PairingService::set_local_alias(std::string alias) { std::lock_guard lock(impl_->mutex); impl_->alias = std::move(alias); impl_->save_unlocked(); }
std::string PairingService::local_alias() const { std::lock_guard lock(impl_->mutex); return impl_->alias; }
void PairingService::set_exposed_endpoints(std::vector<ExposedEndpoint> endpoints) { std::lock_guard lock(impl_->mutex); impl_->exposed = std::move(endpoints); impl_->save_unlocked(); }
std::vector<ExposedEndpoint> PairingService::exposed_endpoints() const { std::lock_guard lock(impl_->mutex); return impl_->exposed; }
std::string PairingService::current_pair_code() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->pair_code.empty() && std::chrono::steady_clock::now() <= impl_->code_expiry) return impl_->pair_code;
    return impl_->generate_pair_code_unlocked();
}
std::string PairingService::create_pair_code() { std::lock_guard lock(impl_->mutex); return impl_->generate_pair_code_unlocked(); }
std::vector<RemotePeer> PairingService::peers() const { std::lock_guard lock(impl_->mutex); std::vector<RemotePeer> result; for (const auto& [_, peer] : impl_->known_peers) result.push_back(peer); return result; }
bool PairingService::set_discovery_enabled(bool enabled) {
    if (enabled) {
        { std::lock_guard lock(impl_->mutex); impl_->discovery_enabled = true; impl_->save_unlocked(); }
        if (impl_->listener == kInvalidSocket || impl_->start_discovery()) return true;
        std::lock_guard lock(impl_->mutex); impl_->discovery_enabled = false; impl_->save_unlocked(); return false;
    }
    impl_->stop_discovery();
    std::lock_guard lock(impl_->mutex); impl_->discovery_enabled = false; impl_->save_unlocked(); return true;
}
bool PairingService::discovery_enabled() const { std::lock_guard lock(impl_->mutex); return impl_->discovery_enabled; }
std::vector<DiscoveredPeer> PairingService::discovered_peers() const {
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(7);
    std::lock_guard lock(impl_->mutex); std::vector<DiscoveredPeer> result;
    for (auto it = impl_->discovered.begin(); it != impl_->discovered.end();) {
        if (it->second.seen < cutoff) it = impl_->discovered.erase(it);
        else { result.push_back(it->second.peer); ++it; }
    }
    return result;
}
void PairingService::set_telemetry(AudioTelemetry telemetry) { std::lock_guard lock(impl_->mutex); impl_->local_telemetry = std::move(telemetry); impl_->save_unlocked(); }
AudioTelemetry PairingService::telemetry() const { std::lock_guard lock(impl_->mutex); return impl_->local_telemetry; }

void PairingService::announce() {
    std::string node, alias, catalog, telemetry; std::uint16_t listen_port{}; std::vector<RemotePeer> peers;
    { std::lock_guard lock(impl_->mutex); node = impl_->node_id; alias = impl_->alias; catalog = serialize(impl_->exposed); telemetry = serialize_telemetry(impl_->local_telemetry); listen_port = impl_->listen_port; for (const auto& [_, peer] : impl_->known_peers) peers.push_back(peer); }
    for (const auto& peer : peers) {
        const std::string target = "/update?node=" + escape(node) + "&alias=" + escape(alias) + "&port=" + std::to_string(listen_port) + "&catalog=" + escape(catalog) + "&telemetry=" + escape(telemetry);
        // Catalog/telemetry synchronization must never make a local UI action
        // wait for an offline peer. Beast also gives every operation a timeout.
        std::thread([host = peer.host, port = peer.port, target] { (void)post_control(host, port, target); }).detach();
    }
}

bool PairingService::pair_remote(const std::string& host, std::uint16_t port, const std::string& alias, const std::string& code) {
    std::string node; { std::lock_guard lock(impl_->mutex); node = impl_->node_id; }
    const std::string target = "/pair?code=" + escape(code) + "&node=" + escape(node) + "&alias=" + escape(alias) + "&port=" + std::to_string(impl_->listen_port);
    const auto response = post_control(host, port, target);
    if (!response.ok) { impl_->error = "Could not pair with host: " + response.error; return false; }
    const auto reply = params("?" + response.body);
    if (reply.contains("error") || !reply.contains("node")) { impl_->error = reply.contains("error") ? reply.at("error") : "Invalid pairing response."; return false; }
    const auto remote_alias = reply.contains("alias") && !reply.at("alias").empty() ? reply.at("alias") : host;
    { std::lock_guard lock(impl_->mutex); impl_->known_peers[reply.at("node")] = {reply.at("node"), remote_alias, host, port, deserialize(reply.contains("catalog") ? reply.at("catalog") : "")}; impl_->save_unlocked(); }
    announce();
    return true;
}
bool PairingService::set_peer_alias(const std::string& node_id, std::string alias) {
    std::lock_guard lock(impl_->mutex); const auto it = impl_->known_peers.find(node_id);
    if (it == impl_->known_peers.end() || alias.empty()) return false;
    it->second.alias = std::move(alias); impl_->save_unlocked(); return true;
}
bool PairingService::set_peer_endpoint(const std::string& node_id, std::string host, std::uint16_t port) {
    std::lock_guard lock(impl_->mutex); const auto it = impl_->known_peers.find(node_id);
    if (it == impl_->known_peers.end() || host.empty() || port == 0) return false;
    it->second.host = std::move(host); it->second.port = port; impl_->save_unlocked(); return true;
}
bool PairingService::remove_peer(const std::string& node_id) {
    RemotePeer peer; std::string local_node;
    { std::lock_guard lock(impl_->mutex); const auto it = impl_->known_peers.find(node_id); if (it == impl_->known_peers.end()) return false; peer = it->second; local_node = impl_->node_id; impl_->known_peers.erase(it); impl_->save_unlocked(); }
    std::thread([host = peer.host, port = peer.port, target = "/unpair?node=" + escape(local_node)] { (void)post_control(host, port, target); }).detach();
    return true;
}
void PairingService::set_remote_route_handler(std::function<bool(const RemoteRouteRequest&, std::string&)> handler) { std::lock_guard lock(impl_->mutex); impl_->route_handler = std::move(handler); }
bool PairingService::request_remote_route(const std::string& node_id, const RemoteRouteRequest& request) {
    RemotePeer peer; std::string local_node;
    { std::lock_guard lock(impl_->mutex); const auto it = impl_->known_peers.find(node_id); if (it == impl_->known_peers.end()) { impl_->error = "Paired device not found."; return false; } peer = it->second; local_node = impl_->node_id; }
    const std::string target = "/route?node=" + escape(local_node) + "&kind=" + (request.kind == RemoteRouteKind::Send ? "send" : "receive") + "&device=" + escape(request.device_id) + "&port=" + std::to_string(request.port) + "&quality=" + escape(request.quality) + "&latency=" + std::to_string(request.max_latency_ms) + "&mode=" + escape(request.mode) + "&loopback=" + (request.render_loopback ? "true" : "false");
    const auto response = post_control(peer.host, peer.port, target);
    if (!response.ok) { impl_->error = "Remote control failed: " + response.error; return false; }
    if (response.body != "ok") { impl_->error = response.body.empty() ? "Paired device rejected route." : unescape(response.body.substr(response.body.rfind('=') + 1)); return false; }
    return true;
}

} // namespace oamr::pairing
