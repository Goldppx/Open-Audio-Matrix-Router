#include "oamr/core/audio_graph.hpp"

#include <algorithm>
#include <utility>

namespace oamr {

bool AudioNode::has_port(const std::string& port_id) const { return find_port(port_id) != nullptr; }

const AudioPort* AudioNode::find_port(const std::string& port_id) const {
    const auto it = std::find_if(ports.begin(), ports.end(), [&port_id](const AudioPort& port) { return port.id == port_id; });
    return it == ports.end() ? nullptr : &*it;
}

GraphResult AudioGraph::add_node(AudioNode node) {
    if (node.id.empty()) return {GraphError::EmptyId, "Node id must not be empty."};
    if (std::any_of(nodes_.begin(), nodes_.end(), [&node](const AudioNode& current) { return current.id == node.id; }))
        return {GraphError::DuplicateNode, "A node with this id already exists."};
    for (const auto& port : node.ports) {
        if (port.id.empty()) return {GraphError::EmptyId, "Port id must not be empty."};
        if (!port.format.is_valid()) return {GraphError::InvalidFormat, "Port audio format is invalid."};
        if (find_port(port.id) || std::count_if(node.ports.begin(), node.ports.end(), [&port](const AudioPort& candidate) { return candidate.id == port.id; }) != 1)
            return {GraphError::DuplicatePort, "Port ids must be globally unique."};
    }
    nodes_.push_back(std::move(node));
    return {};
}

GraphResult AudioGraph::remove_node(const std::string& node_id) {
    const auto node_it = std::find_if(nodes_.begin(), nodes_.end(), [&node_id](const AudioNode& node) { return node.id == node_id; });
    if (node_it == nodes_.end()) return {GraphError::NodeNotFound, "Node was not found."};
    std::vector<std::string> port_ids;
    for (const auto& port : node_it->ports) port_ids.push_back(port.id);
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(), [&port_ids](const AudioConnection& connection) {
        return std::find(port_ids.begin(), port_ids.end(), connection.source_port_id) != port_ids.end() || std::find(port_ids.begin(), port_ids.end(), connection.sink_port_id) != port_ids.end();
    }), connections_.end());
    nodes_.erase(node_it);
    return {};
}

GraphResult AudioGraph::connect(AudioConnection connection) {
    if (connection.id.empty()) return {GraphError::EmptyId, "Connection id must not be empty."};
    if (connection.source_port_id == connection.sink_port_id) return {GraphError::SelfConnection, "A port cannot connect to itself."};
    if (std::any_of(connections_.begin(), connections_.end(), [&connection](const AudioConnection& current) { return current.id == connection.id; }))
        return {GraphError::DuplicateConnection, "A connection with this id already exists."};
    const auto* source = find_port(connection.source_port_id);
    if (!source) return {GraphError::SourceNotFound, "Source port was not found."};
    if (source->direction != PortDirection::Source) return {GraphError::InvalidSourceDirection, "The source endpoint must be a Source port."};
    const auto* sink = find_port(connection.sink_port_id);
    if (!sink) return {GraphError::SinkNotFound, "Sink port was not found."};
    if (sink->direction != PortDirection::Sink) return {GraphError::InvalidSinkDirection, "The destination endpoint must be a Sink port."};
    connections_.push_back(std::move(connection));
    return {};
}

GraphResult AudioGraph::disconnect(const std::string& connection_id) {
    const auto it = std::find_if(connections_.begin(), connections_.end(), [&connection_id](const AudioConnection& connection) { return connection.id == connection_id; });
    if (it == connections_.end()) return {GraphError::ConnectionNotFound, "Connection was not found."};
    connections_.erase(it);
    return {};
}

const AudioPort* AudioGraph::find_port(const std::string& port_id) const {
    for (const auto& node : nodes_) if (const auto* port = node.find_port(port_id)) return port;
    return nullptr;
}

std::optional<AudioNode> AudioGraph::find_node(const std::string& node_id) const {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(), [&node_id](const AudioNode& node) { return node.id == node_id; });
    return it == nodes_.end() ? std::nullopt : std::optional<AudioNode>{*it};
}

} // namespace oamr
