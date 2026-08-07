#include "oamr/gstreamer/device_enumerator.hpp"

#include <gst/gst.h>

namespace oamr::gstreamer {
namespace {

void unref_gst_object(gpointer object) { gst_object_unref(object); }

std::string selector_for(GstElement* element) {
    if (element == nullptr) return {};
    GstElementFactory* factory = gst_element_get_factory(element);
    if (factory == nullptr) return {};
    std::string selector;
    GParamSpec* property = g_object_class_find_property(G_OBJECT_GET_CLASS(element), "device");
    if (property != nullptr && G_PARAM_SPEC_VALUE_TYPE(property) == G_TYPE_STRING) {
        gchar* value = nullptr;
        g_object_get(element, "device", &value, nullptr);
        if (value != nullptr) {
            selector = std::string(GST_OBJECT_NAME(factory)) + "|" + value;
            g_free(value);
        }
    }
    return selector;
}

std::vector<DiscoveredDevice> list_devices(const gchar* klass, PortDirection direction) {
    gst_init(nullptr, nullptr);
    GstDeviceMonitor* monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(monitor, klass, nullptr);
    gst_device_monitor_start(monitor);
    GList* devices = gst_device_monitor_get_devices(monitor);

    std::vector<DiscoveredDevice> result;
    for (GList* item = devices; item != nullptr; item = item->next) {
        auto* device = GST_DEVICE(item->data);
        const gchar* display_name = gst_device_get_display_name(device);
        GstElement* element = gst_device_create_element(device, nullptr);
        GstElementFactory* factory = element ? gst_element_get_factory(element) : nullptr;
        const gchar* factory_name = factory ? GST_OBJECT_NAME(factory) : "unknown";
        const std::string selector = selector_for(element);
        // Some plugins do not expose a string `device` property. They remain
        // discoverable but use the default endpoint until that backend gains a selector.
        result.push_back({selector.empty() ? std::string(factory_name) + "|" : selector,
                          display_name ? display_name : "Unnamed device", direction, !selector.empty()});
        if (element != nullptr) gst_object_unref(element);
    }
    g_list_free_full(devices, unref_gst_object);
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
    return result;
}

} // namespace

std::vector<DiscoveredDevice> DeviceEnumerator::list_capture_devices() const {
    return list_devices("Audio/Source", PortDirection::Source);
}

std::vector<DiscoveredDevice> DeviceEnumerator::list_playback_devices() const {
    return list_devices("Audio/Sink", PortDirection::Sink);
}

} // namespace oamr::gstreamer
