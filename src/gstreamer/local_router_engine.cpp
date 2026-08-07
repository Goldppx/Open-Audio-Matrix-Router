#include "oamr/gstreamer/local_router_engine.hpp"

#include <map>
#include <set>

namespace oamr::gstreamer {
namespace {

bool is_local_device(const AudioPort& port) {
    // GStreamer device selectors use factory|device-id. Empty remains the
    // explicit request for the operating system's default local endpoint.
    return port.backend_id.empty() || port.backend_id.find('|') != std::string::npos;
}

} // namespace

bool LocalRouterEngine::apply(const AudioGraph& graph) {
    stop();
    std::map<std::string, std::vector<const AudioConnection*>> groups;
    std::set<std::string> assigned_sinks;
    for (const auto& connection : graph.connections()) {
        const AudioPort* source = graph.find_port(connection.source_port_id);
        const AudioPort* sink = graph.find_port(connection.sink_port_id);
        if (!connection.enabled || source == nullptr || sink == nullptr) continue;
        if (!is_local_device(*source) || !is_local_device(*sink)) {
            error_ = "LocalRouterEngine only accepts local device ports.";
            return false;
        }
        if (!assigned_sinks.insert(sink->id).second) {
            error_ = "Multiple Sources target one Sink; mixing is not implemented yet.";
            return false;
        }
        groups[source->id].push_back(&connection);
    }

    for (const auto& [source_id, connections] : groups) {
        const AudioPort* source = graph.find_port(source_id);
        LocalFanoutSettings settings;
        settings.source_device = source->backend_id;
        settings.pcm = {source->format.sample_rate, source->format.channels, 20};
        for (const auto* connection : connections) {
            const AudioPort* sink = graph.find_port(connection->sink_port_id);
            settings.sink_devices.push_back(sink->backend_id);
        }
        auto pipeline = std::make_unique<RtpOpusPipeline>();
        if (!pipeline->start_local_fanout(settings)) {
            error_ = "Could not start route from '" + source->name + "': " + pipeline->last_error();
            stop();
            return false;
        }
        pipelines_.push_back(std::move(pipeline));
    }
    error_.clear();
    return true;
}

void LocalRouterEngine::stop() noexcept {
    for (auto& pipeline : pipelines_) pipeline->stop();
    pipelines_.clear();
}

bool LocalRouterEngine::poll() {
    for (const auto& pipeline : pipelines_) {
        if (!pipeline->poll()) {
            error_ = pipeline->last_error();
            return false;
        }
    }
    return true;
}

} // namespace oamr::gstreamer
