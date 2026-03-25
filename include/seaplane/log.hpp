#pragma once

#include <seaplane/time.hpp>

#include <chrono>
#include <format>
#include <print>
#include <source_location>

namespace seaplane {

template<typename... Args>
void log(std::source_location location,
         std::format_string<Args...> format,
         Args&&... args)
{
    using namespace std::chrono;
    auto timepoint = sys_time<nanoseconds>(nanoseconds(tsc::nanos_since_epoch()));
    auto day = floor<days>(timepoint);
    year_month_day ymd{day};
    hh_mm_ss hms{timepoint - day};

    auto message = std::format(format, std::forward<Args>(args)...);

    std::println("{}-{:02}-{:02} {:02}:{:02}:{:02}.{:09} [{}:{}] {}",
                 static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                 hms.hours().count(), hms.minutes().count(), hms.seconds().count(), hms.subseconds().count(),
                 location.function_name(), location.line(),
                 message);
}

}

#define sea_log(fmt, ...) seaplane::log(std::source_location::current(), fmt, ##__VA_ARGS__)
