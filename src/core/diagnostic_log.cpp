#include "oamr/diagnostics/log.hpp"

#include <deque>
#include <iostream>
#include <mutex>
#include <utility>

namespace oamr::diagnostics {
namespace {
constexpr std::size_t kMaximumEntries = 500;
std::mutex log_mutex;
std::deque<Entry> log_entries;
std::uint64_t next_sequence{1};
}

void write(Level level, std::string message) {
    if (message.empty()) return;
    const std::string console_message = message;
    const auto timestamp = std::chrono::system_clock::now();
    {
        std::lock_guard lock(log_mutex);
        log_entries.push_back({next_sequence++, timestamp, level, std::move(message)});
        while (log_entries.size() > kMaximumEntries) log_entries.pop_front();
    }
    // Keep warnings and errors visible when OAMR runs without the Web UI.
    if (level == Level::Warning || level == Level::Error)
        std::cerr << '[' << level_name(level) << "] " << console_message << '\n';
}

std::vector<Entry> entries_after(std::uint64_t sequence) {
    std::lock_guard lock(log_mutex);
    std::vector<Entry> result;
    for (const auto& entry : log_entries)
        if (entry.sequence > sequence) result.push_back(entry);
    return result;
}

const char* level_name(Level level) noexcept {
    switch (level) {
        case Level::Verbose: return "VERBOSE";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARNING";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

} // namespace oamr::diagnostics
