#pragma once

#include "oamr/core/audio_node.hpp"

#include <string>
#include <vector>

namespace oamr::gstreamer {

/**
 * A device discovered through GstDeviceMonitor. backend_id is an opaque
 * `factory|device-property` selector accepted by RtpOpusPipeline.
 */
struct DiscoveredDevice {
    std::string backend_id;
    std::string name;
    PortDirection direction;
    bool selectable{false};
};

/** Enumerates capture and playback endpoints without exposing OS-specific APIs. */
class DeviceEnumerator {
public:
    [[nodiscard]] std::vector<DiscoveredDevice> list_capture_devices() const;
    [[nodiscard]] std::vector<DiscoveredDevice> list_playback_devices() const;
};

} // namespace oamr::gstreamer
