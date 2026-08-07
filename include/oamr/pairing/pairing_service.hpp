#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oamr::pairing {

enum class EndpointDirection { Source, Sink };
struct ExposedEndpoint {
    std::string backend_id;
    std::string name;
    EndpointDirection direction{};
};
/** Latest peer-reported transport state. Empty device_name means idle. */
struct AudioTelemetry {
    std::string quality{"idle"};
    std::uint16_t target_latency_ms{};
    double packet_loss_percent{};
    std::string device_name;
};
struct RemotePeer {
    std::string node_id;
    std::string alias;
    std::string host;
    std::uint16_t port{};
    std::vector<ExposedEndpoint> endpoints;
    AudioTelemetry telemetry;
};
enum class RemoteRouteKind { Send, Receive };
struct RemoteRouteRequest {
    RemoteRouteKind kind{};
    std::string device_id;
    std::string host;
    std::uint16_t port{};
    std::string quality{"medium"};
    std::uint16_t max_latency_ms{100};
    std::string mode{"auto"};
    bool render_loopback{};
};

/**
 * LAN-only control plane. Web UI stays loopback-only; this service binds to
 * the configured pairing port and accepts only code-authorized peers.
 */
class PairingService {
public:
    PairingService();
    ~PairingService();
    PairingService(const PairingService&) = delete;
    PairingService& operator=(const PairingService&) = delete;

    bool start(std::uint16_t port = 8791);
    void stop() noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

    void set_local_alias(std::string alias);
    [[nodiscard]] std::string local_alias() const;
    void set_exposed_endpoints(std::vector<ExposedEndpoint> endpoints);
    [[nodiscard]] std::vector<ExposedEndpoint> exposed_endpoints() const;
    void set_telemetry(AudioTelemetry telemetry);
    [[nodiscard]] AudioTelemetry telemetry() const;
    /** Pushes the complete current catalog and telemetry to every paired peer. */
    void announce();

    /** Generates a six-digit code valid for ten minutes and one use. */
    [[nodiscard]] std::string create_pair_code();
    /** Calls a remote pairing port with its current one-time code. */
    bool pair_remote(const std::string& host, std::uint16_t port, const std::string& alias, const std::string& code);
    bool set_peer_alias(const std::string& node_id, std::string alias);
    bool set_peer_endpoint(const std::string& node_id, std::string host, std::uint16_t port);
    void set_remote_route_handler(std::function<bool(const RemoteRouteRequest&, std::string&)> handler);
    bool request_remote_route(const std::string& node_id, const RemoteRouteRequest& request);
    [[nodiscard]] std::vector<RemotePeer> peers() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oamr::pairing
