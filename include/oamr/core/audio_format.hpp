#pragma once

#include <cstdint>

namespace oamr {

/** A PCM format advertised by an audio port. GStreamer converts when needed. */
struct AudioFormat {
    std::uint32_t sample_rate{48000};
    std::uint16_t channels{2};
    std::uint16_t bits_per_sample{16};

    [[nodiscard]] bool is_valid() const noexcept {
        return sample_rate > 0 && channels > 0 && bits_per_sample > 0;
    }
};

} // namespace oamr
