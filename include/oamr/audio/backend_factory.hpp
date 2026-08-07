#pragma once

#include "oamr/audio/audio_backend.hpp"

#include <memory>

namespace oamr::audio {

/**
 * Creates the audio backend compiled into this build.
 *
 * The concrete implementation lives in the platform backend library
 * (currently oamr_gstreamer). Returns nullptr when no backend is available;
 * the caller should surface that as a user-facing error.
 */
[[nodiscard]] std::unique_ptr<AudioBackend> create_audio_backend();

} // namespace oamr::audio