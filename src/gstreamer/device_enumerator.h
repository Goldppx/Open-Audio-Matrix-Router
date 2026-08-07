#pragma once

#include "oamr/audio/audio_types.hpp"

#include <vector>

namespace oamr::gstreamer {

/**
 * Internal device discovery through GstDeviceMonitor.
 *
 * This is an implementation detail of the GStreamer backend. Consumers must
 * use oamr::audio::AudioBackend::list_sources()/list_sinks() instead.
 */
class DeviceEnumerator {
public:
    [[nodiscard]] std::vector<audio::DeviceInfo> list_capture_devices() const;
    [[nodiscard]] std::vector<audio::DeviceInfo> list_playback_devices() const;
};

} // namespace oamr::gstreamer