#include <jni.h>
#include <oboe/Oboe.h>

#include <memory>
#include <string>

namespace {
std::shared_ptr<oboe::AudioStream> input_stream;

jstring result(JNIEnv* environment, const std::string& message) {
    return environment->NewStringUTF(message.c_str());
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_gem_oamr_audio_OboeAudioEngine_nativeStartInput(JNIEnv* environment, jobject) {
    if (input_stream && input_stream->getState() == oboe::StreamState::Started)
        return result(environment, "Oboe 麦克风已在运行");

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Shared);
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(oboe::ChannelCount::Mono);

    std::shared_ptr<oboe::AudioStream> stream;
    const oboe::Result open_result = builder.openStream(stream);
    if (open_result != oboe::Result::OK)
        return result(environment, "无法打开 Oboe 输入：" + std::string(oboe::convertToText(open_result)));

    const oboe::Result start_result = stream->requestStart();
    if (start_result != oboe::Result::OK)
        return result(environment, "无法启动 Oboe 输入：" + std::string(oboe::convertToText(start_result)));

    input_stream = std::move(stream);
    return result(environment, "Oboe 麦克风已启动（" + std::to_string(input_stream->getSampleRate()) + " Hz）");
}
