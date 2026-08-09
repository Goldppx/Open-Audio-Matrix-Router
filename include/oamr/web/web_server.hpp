#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace oamr::web {

/** HTTP control surface for the command-line MVP. Defaults to local-only. */
class WebServer {
public:
    WebServer();
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    /**
     * Blocks while serving the supplied IPv4 interface address and port.
     * The default deliberately remains loopback-only; callers must explicitly
     * request a LAN interface address before the UI is exposed to the network.
     */
    bool serve(std::uint16_t port = 8787, const std::string& bind_address = "127.0.0.1");
    void stop() noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oamr::web
