#include "oamr/pairing/pairing_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace oamr::pairing {
namespace {
#ifdef _WIN32
using Socket = SOCKET; constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
#else
using Socket = int; constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { close(socket); }
#endif

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
}

class PairingService::Impl {
public:
    mutable std::mutex mutex;
    std::atomic_bool stopping{false}; std::thread worker; Socket listener{kInvalidSocket}; std::uint16_t listen_port{};
    std::string error, node_id{random_hex(24)}, alias{[] { const char* name = std::getenv("COMPUTERNAME"); return name && *name ? std::string{name} : std::string{"This computer"}; }()}, pair_code; std::chrono::steady_clock::time_point code_expiry{};
    std::vector<ExposedEndpoint> exposed; AudioTelemetry local_telemetry; std::map<std::string, RemotePeer> known_peers;

    std::filesystem::path state_path() const { return std::filesystem::current_path() / "oamr-pairing-state.txt"; }
    void save_unlocked() const {
        std::ofstream file(state_path(), std::ios::trunc);
        if (!file) return;
        file << "node\t" << escape(node_id) << "\n";
        file << "alias\t" << escape(alias) << "\n";
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
            else if (fields.size() >= 4 && fields[0] == "endpoint") exposed.push_back({unescape(fields[2]), unescape(fields[3]), fields[1] == "S" ? EndpointDirection::Source : EndpointDirection::Sink});
            else if (fields.size() >= 2 && fields[0] == "telemetry") local_telemetry = deserialize_telemetry(unescape(fields[1]));
            else if (fields.size() >= 6 && fields[0] == "peer") { try { RemotePeer peer{unescape(fields[1]), unescape(fields[2]), unescape(fields[3]), static_cast<std::uint16_t>(std::stoul(fields[4])), deserialize(unescape(fields[5]))}; if (fields.size() >= 7) peer.telemetry = deserialize_telemetry(unescape(fields[6])); known_peers[peer.node_id] = std::move(peer); } catch (...) {} }
        }
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
    impl_->listen_port = port; impl_->stopping = false; impl_->worker = std::thread([this] { impl_->serve(); }); return true;
}
void PairingService::stop() noexcept { impl_->stopping = true; if (impl_->listener != kInvalidSocket) { close_socket(impl_->listener); impl_->listener = kInvalidSocket; } if (impl_->worker.joinable()) impl_->worker.join(); }
const std::string& PairingService::last_error() const noexcept { return impl_->error; }
std::uint16_t PairingService::port() const noexcept { return impl_->listen_port; }
void PairingService::set_local_alias(std::string alias) { std::lock_guard lock(impl_->mutex); impl_->alias = std::move(alias); impl_->save_unlocked(); }
std::string PairingService::local_alias() const { std::lock_guard lock(impl_->mutex); return impl_->alias; }
void PairingService::set_exposed_endpoints(std::vector<ExposedEndpoint> endpoints) { std::lock_guard lock(impl_->mutex); impl_->exposed = std::move(endpoints); impl_->save_unlocked(); }
std::vector<ExposedEndpoint> PairingService::exposed_endpoints() const { std::lock_guard lock(impl_->mutex); return impl_->exposed; }
std::string PairingService::create_pair_code() { std::lock_guard lock(impl_->mutex); impl_->pair_code = random_hex(6); std::transform(impl_->pair_code.begin(), impl_->pair_code.end(), impl_->pair_code.begin(), ::toupper); impl_->code_expiry = std::chrono::steady_clock::now() + std::chrono::minutes(10); return impl_->pair_code; }
std::vector<RemotePeer> PairingService::peers() const { std::lock_guard lock(impl_->mutex); std::vector<RemotePeer> result; for (const auto& [_, peer] : impl_->known_peers) result.push_back(peer); return result; }
void PairingService::set_telemetry(AudioTelemetry telemetry) { std::lock_guard lock(impl_->mutex); impl_->local_telemetry = std::move(telemetry); impl_->save_unlocked(); }
AudioTelemetry PairingService::telemetry() const { std::lock_guard lock(impl_->mutex); return impl_->local_telemetry; }

void PairingService::announce() {
    std::string node, alias, catalog, telemetry; std::uint16_t listen_port{}; std::vector<RemotePeer> peers;
    { std::lock_guard lock(impl_->mutex); node = impl_->node_id; alias = impl_->alias; catalog = serialize(impl_->exposed); telemetry = serialize_telemetry(impl_->local_telemetry); listen_port = impl_->listen_port; for (const auto& [_, peer] : impl_->known_peers) peers.push_back(peer); }
    for (const auto& peer : peers) {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; addrinfo* addresses{};
        if (getaddrinfo(peer.host.c_str(), std::to_string(peer.port).c_str(), &hints, &addresses) != 0) continue;
        Socket socket_fd = socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
        if (socket_fd == kInvalidSocket || connect(socket_fd, addresses->ai_addr, static_cast<int>(addresses->ai_addrlen)) != 0) { if (socket_fd != kInvalidSocket) close_socket(socket_fd); freeaddrinfo(addresses); continue; }
        freeaddrinfo(addresses);
        const std::string target = "/update?node=" + escape(node) + "&alias=" + escape(alias) + "&port=" + std::to_string(listen_port) + "&catalog=" + escape(catalog) + "&telemetry=" + escape(telemetry);
        const std::string request = "POST " + target + " HTTP/1.1\r\nHost: " + peer.host + "\r\nConnection: close\r\n\r\n";
        send(socket_fd, request.data(), static_cast<int>(request.size()), 0); close_socket(socket_fd);
    }
}

bool PairingService::pair_remote(const std::string& host, std::uint16_t port, const std::string& alias, const std::string& code) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; addrinfo* addresses{};
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses) != 0) { impl_->error = "Could not resolve pairing host."; return false; }
    Socket socket_fd = socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol); if (socket_fd == kInvalidSocket || connect(socket_fd, addresses->ai_addr, static_cast<int>(addresses->ai_addrlen)) != 0) { if (socket_fd != kInvalidSocket) close_socket(socket_fd); freeaddrinfo(addresses); impl_->error = "Could not connect to pairing host."; return false; }
    freeaddrinfo(addresses); std::string node; { std::lock_guard lock(impl_->mutex); node = impl_->node_id; }
    const std::string target = "/pair?code=" + escape(code) + "&node=" + escape(node) + "&alias=" + escape(alias) + "&port=" + std::to_string(impl_->listen_port);
    const std::string request = "POST " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n"; send(socket_fd, request.data(), static_cast<int>(request.size()), 0);
    char buffer[8192]{}; const int received = recv(socket_fd, buffer, sizeof(buffer) - 1, 0); close_socket(socket_fd); if (received <= 0) { impl_->error = "Pairing host sent no response."; return false; }
    std::string response(buffer, received); const auto body_at = response.find("\r\n\r\n"); const auto reply = params("?" + response.substr(body_at == std::string::npos ? 0 : body_at + 4));
    if (reply.contains("error") || !reply.contains("node")) { impl_->error = reply.contains("error") ? reply.at("error") : "Invalid pairing response."; return false; }
    { std::lock_guard lock(impl_->mutex); impl_->known_peers[reply.at("node")] = {reply.at("node"), alias, host, port, deserialize(reply.contains("catalog") ? reply.at("catalog") : "")}; impl_->save_unlocked(); }
    announce();
    return true;
}
bool PairingService::set_peer_alias(const std::string& node_id, std::string alias) {
    std::lock_guard lock(impl_->mutex); const auto it = impl_->known_peers.find(node_id);
    if (it == impl_->known_peers.end() || alias.empty()) return false;
    it->second.alias = std::move(alias); impl_->save_unlocked(); return true;
}

} // namespace oamr::pairing
