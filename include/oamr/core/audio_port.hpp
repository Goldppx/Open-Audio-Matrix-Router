#pragma once

#include "oamr/core/audio_format.hpp"

#include <string>

namespace oamr {

enum class PortDirection { Source, Sink };

/**
 * A routable endpoint. A physical device may expose either or both directions.
 * Network receive streams are Sources; network send streams are Sinks.
 */
struct AudioPort {
    std::string id;
    std::string name;
    PortDirection direction{PortDirection::Source};
    AudioFormat format{};
    std::string backend_id;
};

} // namespace oamr
