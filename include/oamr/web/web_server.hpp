#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace oamr::web {

/** Local-only HTTP control surface for the command-line MVP. */
class WebServer {
public:
    WebServer();
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    /** Blocks while serving http://127.0.0.1:<port>. */
    bool serve(std::uint16_t port = 8787);
    void stop() noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oamr::web
