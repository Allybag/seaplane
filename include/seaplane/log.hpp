#pragma once

#include <source_location>
#include <chrono>
#include <print>
#include <format>

namespace seaplane {

template<typename... Args>
void log(std::source_location location,
         std::format_string<Args...> format,
         Args&&... args)
{
    using namespace std::chrono;
    auto timestamp = system_clock::now();
    auto millis = duration_cast<milliseconds>(timestamp.time_since_epoch() % 1000);

    auto message = std::format(format, std::forward<Args>(args)...);

    std::println("{:%Y-%m-%d %H:%M:%S} [{}:{}] {}",
                 timestamp,
                 location.function_name(), location.line(),
                 message);
}

}

#define sea_log(fmt, ...) seaplane::log(std::source_location::current(), fmt, ##__VA_ARGS__)
