#pragma once

#include "oamr/core/audio_connection.hpp"
#include "oamr/core/audio_node.hpp"

#include <optional>
#include <string>
#include <vector>

namespace oamr {

enum class GraphError {
    None,
    EmptyId,
    DuplicateNode,
    DuplicatePort,
    DuplicateConnection,
    NodeNotFound,
    ConnectionNotFound,
    SourceNotFound,
    SinkNotFound,
    InvalidSourceDirection,
    InvalidSinkDirection,
    InvalidFormat,
    SelfConnection
};

struct GraphResult {
    GraphError error{GraphError::None};
    std::string message;
    [[nodiscard]] explicit operator bool() const noexcept { return error == GraphError::None; }
};

/** Platform-neutral desired-state graph. It owns no audio threads or devices. */
class AudioGraph {
public:
    GraphResult add_node(AudioNode node);
    GraphResult remove_node(const std::string& node_id);
    GraphResult connect(AudioConnection connection);
    GraphResult disconnect(const std::string& connection_id);

    [[nodiscard]] const std::vector<AudioNode>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<AudioConnection>& connections() const noexcept { return connections_; }
    [[nodiscard]] const AudioPort* find_port(const std::string& port_id) const;
    [[nodiscard]] std::optional<AudioNode> find_node(const std::string& node_id) const;

private:
    std::vector<AudioNode> nodes_;
    std::vector<AudioConnection> connections_;
};

} // namespace oamr
