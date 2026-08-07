#include "oamr/gstreamer/rtp_opus_pipeline.hpp"

#include <gst/gst.h>

#include <sstream>
#include <utility>

namespace oamr::gstreamer {
namespace {

bool valid_pcm(const PcmSettings& pcm) {
    return pcm.sample_rate > 0 && pcm.channels > 0 && (pcm.opus_frame_ms == 5 || pcm.opus_frame_ms == 10 || pcm.opus_frame_ms == 20 || pcm.opus_frame_ms == 40 || pcm.opus_frame_ms == 60);
}

struct DeviceSelector { std::string factory; std::string device; };

DeviceSelector parse_selector(const std::string& selector, const char* fallback_factory) {
    const auto delimiter = selector.find('|');
    if (selector.empty() || delimiter == std::string::npos) return {fallback_factory, {}};
    const std::string factory = selector.substr(0, delimiter);
    if (factory.empty() || factory.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        return {fallback_factory, {}};
    return {factory, selector.substr(delimiter + 1)};
}

std::string sender_description(const SenderSettings& settings) {
    const DeviceSelector selected = parse_selector(settings.source_device, "autoaudiosrc");
    const auto network = *resolve_network_profile(settings.network);
    std::ostringstream pipeline;
    pipeline << selected.factory << " name=source";
    if (settings.capture_render_device && selected.factory == "wasapi2src")
        pipeline << " loopback=true low-latency=true";
    // The second converter is required after resampling: resampling changes
    // rate, not sample representation, and opusenc requires S16LE.
    pipeline << " ! audioconvert name=sender_convert_input ! audioresample name=sender_resample ! "
             << "audioconvert name=sender_convert_opus ! "
             // Do not force a channel count here. Real capture devices may be
             // mono or stereo; Opus and rtpopuspay carry that information.
             << "audio/x-raw,format=S16LE,layout=interleaved,rate=" << settings.pcm.sample_rate
             << " ! opusenc name=opus_encoder bitrate=" << network.opus_bitrate_bps << " frame-size=" << network.opus_frame_ms
             << " inband-fec=" << (network.inband_fec ? "true" : "false")
             << " packet-loss-percentage=" << (network.inband_fec ? 3 : 0)
             << " ! rtpopuspay name=rtp_opus_payloader pt=96 ! "
             << "udpsink host=" << settings.host << " port=" << settings.port << " sync=false async=false";
    return pipeline.str();
}

std::string receiver_description(const ReceiverSettings& settings) {
    const DeviceSelector selected = parse_selector(settings.sink_device, "autoaudiosink");
    const auto network = *resolve_network_profile(settings.network);
    std::ostringstream pipeline;
    pipeline << "udpsrc port=" << settings.port
             // Opus channel count is signalled by RTP/Opus itself. A fixed
             // encoding-params=2 rejects valid mono sources on other hosts.
             << " caps=\"application/x-rtp,media=audio,encoding-name=OPUS,payload=96,clock-rate=48000\" "
             << "! rtpjitterbuffer latency=" << network.jitter_buffer_ms
             << " drop-on-latency=" << (network.drop_on_latency ? "true" : "false")
             << " do-lost=true"
             << " ! rtpopusdepay ! opusdec ! audioconvert ! audioresample ! audioconvert ! "
             // Do not impose the network's PCM caps on a physical sink. The
             // final converter lets each Windows device negotiate its native
             // rate, channel count and sample representation.
             << selected.factory << " name=sink sync=true";
    return pipeline.str();
}

std::string loopback_description(const LoopbackSettings& settings) {
    const DeviceSelector source = parse_selector(settings.source_device, "autoaudiosrc");
    const DeviceSelector sink = parse_selector(settings.sink_device, "autoaudiosink");
    std::ostringstream pipeline;
    pipeline << source.factory << " name=source";
    // A Windows render endpoint must be opened in WASAPI loopback mode.  This
    // remains opt-in so ordinary microphone capture keeps working unchanged.
    if (settings.capture_render_device && source.factory == "wasapi2src")
        pipeline << " loopback=true low-latency=true";
    pipeline << " ! audioconvert ! audioresample ! audioconvert ! "
             << sink.factory << " name=sink sync=true";
    return pipeline.str();
}

std::string fanout_description(const LocalFanoutSettings& settings) {
    const DeviceSelector source = parse_selector(settings.source_device, "autoaudiosrc");
    std::ostringstream pipeline;
    pipeline << source.factory << " name=source ! audioconvert ! audioresample ! tee name=router ";
    for (std::size_t index = 0; index < settings.sink_devices.size(); ++index) {
        const DeviceSelector sink = parse_selector(settings.sink_devices[index], "autoaudiosink");
        pipeline << "router. ! queue ! audioconvert ! audioresample ! " << sink.factory << " name=sink" << index << " sync=true ";
    }
    return pipeline.str();
}

std::string matrix_description(const LocalMatrixSettings& settings) {
    std::ostringstream pipeline;
    for (std::size_t source = 0; source < settings.source_devices.size(); ++source) {
        const auto selected = parse_selector(settings.source_devices[source], "autoaudiosrc");
        pipeline << selected.factory << " name=source" << source;
        if (source < settings.source_is_render_loopback.size()
            && settings.source_is_render_loopback[source]
            && selected.factory == "wasapi2src")
            pipeline << " loopback=true low-latency=true";
        pipeline << " ! audioconvert ! audioresample ! tee name=split" << source << ' ';
    }
    for (const auto& route : settings.routes)
        pipeline << "split" << route.source_index << ". ! queue ! mix" << route.sink_index << ". ";
    for (std::size_t sink = 0; sink < settings.sink_devices.size(); ++sink) {
        const auto selected = parse_selector(settings.sink_devices[sink], "autoaudiosink");
        pipeline << "audiomixer name=mix" << sink << " ! audioconvert ! audioresample ! audioconvert ! "
                 << selected.factory << " name=sink" << sink << " sync=true ";
    }
    return pipeline.str();
}

void set_device_if_supported(GstElement* pipeline, const gchar* element_name, const std::string& device) {
    if (device.empty()) return;
    GstElement* element = gst_bin_get_by_name(GST_BIN(pipeline), element_name);
    if (!element) return;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "device") != nullptr)
        g_object_set(element, "device", device.c_str(), nullptr);
    gst_object_unref(element);
}

} // namespace

class RtpOpusPipeline::Impl {
public:
    GstElement* pipeline{nullptr};
    GstBus* bus{nullptr};
    std::string error;
    std::optional<ResolvedNetworkProfile> network_profile;

    struct DeviceBinding { std::string element_name; std::string device; };

    bool start(const std::string& description, const std::vector<DeviceBinding>& devices) {
        stop();
        GError* parse_error = nullptr;
        pipeline = gst_parse_launch(description.c_str(), &parse_error);
        if (parse_error != nullptr) {
            error = std::string(parse_error->message) + "\nPipeline: " + description;
            g_error_free(parse_error);
            pipeline = nullptr;
            return false;
        }
        for (const auto& binding : devices)
            set_device_if_supported(pipeline, binding.element_name.c_str(), binding.device);
        bus = gst_element_get_bus(pipeline);
        const GstStateChangeReturn state_result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (state_result == GST_STATE_CHANGE_FAILURE) {
            error = "GStreamer failed to start the pipeline.";
            stop();
            return false;
        }
        if (state_result == GST_STATE_CHANGE_ASYNC) {
            const GstStateChangeReturn wait_result = gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND);
            // A UDP receiver may remain asynchronous until its first RTP
            // packet arrives. Keep it alive unless the bus reports a real
            // device/negotiation failure.
            if (wait_result == GST_STATE_CHANGE_FAILURE || !poll()) {
                if (error.empty()) error = "GStreamer could not start the pipeline.";
                stop();
                return false;
            }
        }
        error.clear();
        return true;
    }

    bool start(const std::string& description, const std::string& device, const gchar* element_name,
               const std::string& secondary_device = {}, const gchar* secondary_element_name = nullptr) {
        std::vector<DeviceBinding> devices{{element_name, device}};
        if (secondary_element_name != nullptr) devices.push_back({secondary_element_name, secondary_device});
        return start(description, devices);
    }

    bool poll() {
        if (bus == nullptr) return pipeline != nullptr;
        GstMessage* message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (message == nullptr) return true;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* gst_error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &gst_error, &debug);
            error = gst_error ? gst_error->message : "Unknown GStreamer error.";
            if (debug != nullptr) g_free(debug);
            if (gst_error != nullptr) g_error_free(gst_error);
        } else {
            error = "GStreamer pipeline reached end of stream.";
        }
        gst_message_unref(message);
        return false;
    }

    void stop() noexcept {
        if (pipeline != nullptr) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        if (bus != nullptr) { gst_object_unref(bus); bus = nullptr; }
        network_profile.reset();
    }
};

RtpOpusPipeline::RtpOpusPipeline() : impl_(std::make_unique<Impl>()) { gst_init(nullptr, nullptr); }
RtpOpusPipeline::~RtpOpusPipeline() { stop(); }

bool RtpOpusPipeline::start_sender(const SenderSettings& settings) {
    const auto network = resolve_network_profile(settings.network);
    if (settings.host.empty() || settings.port == 0 || !valid_pcm(settings.pcm) || !network) {
        impl_->error = "Sender requires a host, non-zero port, and valid Opus PCM settings.";
        return false;
    }
    const bool started = impl_->start(sender_description(settings), parse_selector(settings.source_device, "autoaudiosrc").device, "source");
    if (started) impl_->network_profile = network;
    return started;
}

bool RtpOpusPipeline::start_receiver(const ReceiverSettings& settings) {
    const auto network = resolve_network_profile(settings.network);
    if (settings.port == 0 || !valid_pcm(settings.pcm) || !network) {
        impl_->error = "Receiver requires a non-zero port and valid Opus PCM settings.";
        return false;
    }
    const bool started = impl_->start(receiver_description(settings), parse_selector(settings.sink_device, "autoaudiosink").device, "sink");
    if (started) impl_->network_profile = network;
    return started;
}

bool RtpOpusPipeline::start_loopback(const LoopbackSettings& settings) {
    if (!valid_pcm(settings.pcm)) {
        impl_->error = "Loopback requires valid PCM settings.";
        return false;
    }
    const auto source = parse_selector(settings.source_device, "autoaudiosrc");
    const auto sink = parse_selector(settings.sink_device, "autoaudiosink");
    return impl_->start(loopback_description(settings), source.device, "source", sink.device, "sink");
}

bool RtpOpusPipeline::start_local_fanout(const LocalFanoutSettings& settings) {
    if (settings.sink_devices.empty() || !valid_pcm(settings.pcm)) {
        impl_->error = "Local fanout requires at least one sink and valid PCM settings.";
        return false;
    }
    std::vector<Impl::DeviceBinding> devices;
    devices.push_back({"source", parse_selector(settings.source_device, "autoaudiosrc").device});
    for (std::size_t index = 0; index < settings.sink_devices.size(); ++index)
        devices.push_back({"sink" + std::to_string(index), parse_selector(settings.sink_devices[index], "autoaudiosink").device});
    return impl_->start(fanout_description(settings), devices);
}

bool RtpOpusPipeline::start_local_matrix(const LocalMatrixSettings& settings) {
    if (settings.source_devices.empty() || settings.sink_devices.empty() || settings.routes.empty() || !valid_pcm(settings.pcm)) {
        impl_->error = "Local matrix requires sources, sinks, routes, and valid PCM settings.";
        return false;
    }
    std::vector<Impl::DeviceBinding> devices;
    for (std::size_t source = 0; source < settings.source_devices.size(); ++source)
        devices.push_back({"source" + std::to_string(source), parse_selector(settings.source_devices[source], "autoaudiosrc").device});
    for (std::size_t sink = 0; sink < settings.sink_devices.size(); ++sink)
        devices.push_back({"sink" + std::to_string(sink), parse_selector(settings.sink_devices[sink], "autoaudiosink").device});
    return impl_->start(matrix_description(settings), devices);
}

void RtpOpusPipeline::stop() noexcept { impl_->stop(); }
bool RtpOpusPipeline::poll() { return impl_->poll(); }
bool RtpOpusPipeline::is_running() const noexcept { return impl_->pipeline != nullptr; }
const std::string& RtpOpusPipeline::last_error() const noexcept { return impl_->error; }
std::optional<ResolvedNetworkProfile> RtpOpusPipeline::resolved_network_profile() const noexcept { return impl_->network_profile; }

} // namespace oamr::gstreamer
