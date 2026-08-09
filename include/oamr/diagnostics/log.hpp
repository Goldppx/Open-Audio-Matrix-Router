#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace oamr::diagnostics {

enum class Level { Verbose, Info, Warning, Error };

struct Entry {
    std::uint64_t sequence{};
    std::chrono::system_clock::time_point timestamp;
    Level level{Level::Info};
    std::string message;
};

/** Adds one meaningful diagnostic event to the process-wide bounded log. */
void write(Level level, std::string message);

/** Returns retained entries newer than sequence, ordered oldest first. */
[[nodiscard]] std::vector<Entry> entries_after(std::uint64_t sequence);

[[nodiscard]] const char* level_name(Level level) noexcept;

} // namespace oamr::diagnostics
