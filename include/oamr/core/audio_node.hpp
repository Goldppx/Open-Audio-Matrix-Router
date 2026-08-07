#pragma once

#include "oamr/core/audio_port.hpp"

#include <string>
#include <vector>

namespace oamr {

/** Groups related ports, for example a USB interface, PC, or virtual device. */
class AudioNode {
public:
    std::string id;
    std::string name;
    std::vector<AudioPort> ports;

    [[nodiscard]] bool has_port(const std::string& port_id) const;
    [[nodiscard]] const AudioPort* find_port(const std::string& port_id) const;
};

} // namespace oamr
