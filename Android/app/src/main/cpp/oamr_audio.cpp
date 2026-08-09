#include <jni.h>
#include <oboe/Oboe.h>
#include <opus.h>

#include <android/log.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kPacketFrames = 960; // 20 ms: standard Opus/RTP packet duration.
constexpr int kMaxOpusPacket = 1500;

jstring result(JNIEnv* environment, const std::string& message) {
    return environment->NewStringUTF(message.c_str());
}

class SampleQueue {
public:
    explicit SampleQueue(size_t capacity) : data_(capacity) {}

    void push(const float* samples, int count) {
        std::lock_guard lock(mutex_);
        for (int index = 0; index < count; ++index) {
            if (size_ == data_.size()) { read_ = (read_ + 1) % data_.size(); --size_; }
            data_[(read_ + size_) % data_.size()] = samples[index];
            ++size_;
        }
    }

    int pop(float* output, int count) {
        std::lock_guard lock(mutex_);
        const int available = std::min<int>(count, size_);
        for (int index = 0; index < available; ++index) {
            output[index] = data_[read_];
            read_ = (read_ + 1) % data_.size();
        }
        size_ -= available;
        return available;
    }

private:
    std::vector<float> data_;
    size_t read_ = 0;
    size_t size_ = 0;
    std::mutex mutex_;
};

class RtpSenderCallback final : public oboe::AudioStreamCallback {
public:
    RtpSenderCallback(std::string host, int port) : host_(std::move(host)), port_(port) {}
    ~RtpSenderCallback() override { close(); }

    bool open(std::string& error) {
        encoder_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_AUDIO, &opus_error_);
        if (encoder_ == nullptr || opus_error_ != OPUS_OK) { error = "could not create Opus encoder"; return false; }
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(96000));

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* addresses = nullptr;
        const auto lookup = getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &addresses);
        if (lookup != 0 || addresses == nullptr) { error = "could not resolve receiver host"; return false; }
        socket_ = socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
        if (socket_ >= 0) { std::memcpy(&address_, addresses->ai_addr, addresses->ai_addrlen); address_length_ = addresses->ai_addrlen; }
        freeaddrinfo(addresses);
        if (socket_ < 0) { error = "could not open UDP socket"; return false; }
        return true;
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* audio_data, int32_t frames) override {
        const auto* samples = static_cast<const float*>(audio_data);
        std::lock_guard lock(mutex_);
        pending_.insert(pending_.end(), samples, samples + frames);
        while (pending_.size() >= kPacketFrames) {
            unsigned char payload[kMaxOpusPacket];
            const int payload_size = opus_encode_float(encoder_, pending_.data(), kPacketFrames, payload, kMaxOpusPacket);
            pending_.erase(pending_.begin(), pending_.begin() + kPacketFrames);
            if (payload_size <= 0 || socket_ < 0) continue;
            unsigned char rtp[12 + kMaxOpusPacket];
            rtp[0] = 0x80; rtp[1] = 96;
            const uint16_t network_sequence = htons(sequence_++);
            const uint32_t network_timestamp = htonl(timestamp_);
            const uint32_t network_ssrc = htonl(ssrc_);
            std::memcpy(rtp + 2, &network_sequence, 2);
            std::memcpy(rtp + 4, &network_timestamp, 4);
            std::memcpy(rtp + 8, &network_ssrc, 4);
            std::memcpy(rtp + 12, payload, payload_size);
            sendto(socket_, rtp, 12 + payload_size, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&address_), address_length_);
            timestamp_ += kPacketFrames;
        }
        return oboe::DataCallbackResult::Continue;
    }

    void close() {
        if (socket_ >= 0) { ::close(socket_); socket_ = -1; }
        if (encoder_ != nullptr) { opus_encoder_destroy(encoder_); encoder_ = nullptr; }
    }

private:
    std::string host_;
    int port_;
    OpusEncoder* encoder_ = nullptr;
    int opus_error_ = OPUS_OK;
    int socket_ = -1;
    sockaddr_storage address_{};
    socklen_t address_length_ = 0;
    uint16_t sequence_ = 0;
    uint32_t timestamp_ = 0;
    uint32_t ssrc_ = 0x4f414d52; // "OAMR"
    std::vector<float> pending_;
    std::mutex mutex_;
};

class OutputCallback final : public oboe::AudioStreamCallback {
public:
    explicit OutputCallback(SampleQueue& queue) : queue_(queue) {}
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* audio_data, int32_t frames) override {
        auto* output = static_cast<float*>(audio_data);
        const int received = queue_.pop(output, frames);
        std::fill(output + received, output + frames, 0.0f);
        return oboe::DataCallbackResult::Continue;
    }
private:
    SampleQueue& queue_;
};

struct NetworkState {
    std::shared_ptr<oboe::AudioStream> input_stream;
    std::shared_ptr<oboe::AudioStream> output_stream;
    std::shared_ptr<RtpSenderCallback> sender_callback;
    std::shared_ptr<OutputCallback> output_callback;
    std::atomic<bool> receiver_running = false;
    std::thread receiver_thread;
    int receiver_socket = -1;
    SampleQueue received_samples{static_cast<size_t>(kSampleRate * 2)};
    std::mutex mutex;

    void stop() {
        receiver_running = false;
        if (receiver_socket >= 0) { shutdown(receiver_socket, SHUT_RDWR); close(receiver_socket); receiver_socket = -1; }
        if (receiver_thread.joinable()) receiver_thread.join();
        if (input_stream) { input_stream->requestStop(); input_stream->close(); input_stream.reset(); }
        if (output_stream) { output_stream->requestStop(); output_stream->close(); output_stream.reset(); }
        sender_callback.reset();
        output_callback.reset();
    }
};

std::shared_ptr<oboe::AudioStream> monitor_input_stream;
NetworkState network;

std::string start_sender(const std::string& host, int port) {
    std::lock_guard lock(network.mutex);
    network.stop();
    auto callback = std::make_shared<RtpSenderCallback>(host, port);
    std::string error;
    if (!callback->open(error)) return "error=" + error;
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Shared);
    builder.setSampleRate(kSampleRate);
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(kChannels);
    builder.setDataCallback(callback);
    std::shared_ptr<oboe::AudioStream> stream;
    const auto open_result = builder.openStream(stream);
    if (open_result != oboe::Result::OK) return "error=could not open microphone: " + std::string(oboe::convertToText(open_result));
    const auto start_result = stream->requestStart();
    if (start_result != oboe::Result::OK) return "error=could not start microphone: " + std::string(oboe::convertToText(start_result));
    network.sender_callback = std::move(callback);
    network.input_stream = std::move(stream);
    return "Android microphone is sending RTP/Opus to " + host + ":" + std::to_string(port);
}

std::string start_receiver(int port) {
    std::lock_guard lock(network.mutex);
    network.stop();
    auto callback = std::make_shared<OutputCallback>(network.received_samples);
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Shared);
    builder.setSampleRate(kSampleRate);
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(kChannels);
    builder.setDataCallback(callback);
    std::shared_ptr<oboe::AudioStream> stream;
    const auto open_result = builder.openStream(stream);
    if (open_result != oboe::Result::OK) return "error=could not open speaker: " + std::string(oboe::convertToText(open_result));
    const auto start_result = stream->requestStart();
    if (start_result != oboe::Result::OK) return "error=could not start speaker: " + std::string(oboe::convertToText(start_result));

    const int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket < 0) return "error=could not open UDP receiver";
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(udp_socket, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)) != 0) { close(udp_socket); return "error=could not bind UDP port"; }

    network.output_callback = std::move(callback);
    network.output_stream = std::move(stream);
    network.receiver_socket = udp_socket;
    network.receiver_running = true;
    network.receiver_thread = std::thread([udp_socket] {
        int opus_error = OPUS_OK;
        OpusDecoder* decoder = opus_decoder_create(kSampleRate, kChannels, &opus_error);
        unsigned char packet[1600];
        float decoded[kPacketFrames * 6];
        while (network.receiver_running) {
            const int bytes = recvfrom(udp_socket, packet, sizeof(packet), 0, nullptr, nullptr);
            if (bytes < 13 || (packet[0] >> 6) != 2 || (packet[1] & 0x7f) != 96) continue;
            const int csrc_count = packet[0] & 0x0f;
            const int header_size = 12 + csrc_count * 4;
            if (bytes <= header_size || decoder == nullptr) continue;
            const int frames = opus_decode_float(decoder, packet + header_size, bytes - header_size, decoded, kPacketFrames * 6, 0);
            if (frames > 0) network.received_samples.push(decoded, frames);
        }
        if (decoder != nullptr) opus_decoder_destroy(decoder);
    });
    return "Android speaker is receiving RTP/Opus on UDP " + std::to_string(port);
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_gem_oamr_audio_OboeAudioEngine_nativeStartInput(JNIEnv* environment, jobject) {
    if (monitor_input_stream && monitor_input_stream->getState() == oboe::StreamState::Started)
        return result(environment, "Oboe microphone is already running");
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Shared);
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(oboe::ChannelCount::Mono);
    std::shared_ptr<oboe::AudioStream> stream;
    const auto open_result = builder.openStream(stream);
    if (open_result != oboe::Result::OK) return result(environment, "Could not open Oboe input: " + std::string(oboe::convertToText(open_result)));
    const auto start_result = stream->requestStart();
    if (start_result != oboe::Result::OK) return result(environment, "Could not start Oboe input: " + std::string(oboe::convertToText(start_result)));
    monitor_input_stream = std::move(stream);
    return result(environment, "Oboe microphone started (" + std::to_string(monitor_input_stream->getSampleRate()) + " Hz)");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gem_oamr_audio_OboeAudioEngine_nativeStartRtpSender(JNIEnv* environment, jobject, jstring host, jint port) {
    const char* raw_host = environment->GetStringUTFChars(host, nullptr);
    const std::string message = start_sender(raw_host ? raw_host : "", port);
    if (raw_host) environment->ReleaseStringUTFChars(host, raw_host);
    return result(environment, message);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gem_oamr_audio_OboeAudioEngine_nativeStartRtpReceiver(JNIEnv* environment, jobject, jint port) {
    return result(environment, start_receiver(port));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gem_oamr_audio_OboeAudioEngine_nativeStopNetworkRoutes(JNIEnv* environment, jobject) {
    std::lock_guard lock(network.mutex);
    network.stop();
    return result(environment, "Android network routes stopped");
}
