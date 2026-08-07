#pragma once

#include <string>

namespace oamr {

/** A directed route. Source-to-many-sink routing is represented by many entries. */
struct AudioConnection {
    std::string id;
    std::string source_port_id;
    std::string sink_port_id;
    bool enabled{true};
};

} // namespace oamr
