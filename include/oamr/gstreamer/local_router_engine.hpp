#pragma once

#include "oamr/core/audio_graph.hpp"
#include "oamr/gstreamer/rtp_opus_pipeline.hpp"

#include <memory>
#include <string>
#include <vector>

namespace oamr::gstreamer {

/** Reconciles local AudioGraph routes into one capture-and-fanout pipeline per Source. */
class LocalRouterEngine {
public:
    /**
     * Replaces the active local route set atomically from the caller's view.
     * Multiple Sources targeting one Sink are rejected until the Mixer exists.
     */
    bool apply(const AudioGraph& graph);
    void stop() noexcept;
    [[nodiscard]] bool poll();
    [[nodiscard]] const std::string& last_error() const noexcept { return error_; }
    [[nodiscard]] std::size_t active_route_groups() const noexcept { return pipelines_.size(); }

private:
    std::vector<std::unique_ptr<RtpOpusPipeline>> pipelines_;
    std::string error_;
};

} // namespace oamr::gstreamer
