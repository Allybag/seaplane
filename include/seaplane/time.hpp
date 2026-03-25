#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace seaplane::tsc {

    inline std::uint64_t counter() noexcept {
#if defined(__x86_64__)
        return __rdtsc();
#elif defined(__aarch64__) && defined(__APPLE__)
        std::uint64_t val;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
        return val;
#elif defined(__aarch64__)
        std::uint64_t val;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
        return val;
#else
#error "Unsupported architecture: expected x86_64 or aarch64"
#endif
    }

    namespace detail {

        static constexpr int kShift = 32;

        struct Anchor {
            std::uint64_t tsc;
            std::uint64_t epoch_nanos;
            std::uint64_t multiplier;// (1e9 << kShift) / freq
        };

        inline std::uint64_t read_counter_freq() {
#if defined(__x86_64__)
            if (auto *file = std::fopen("/sys/devices/system/cpu/cpu0/tsc_freq_khz", "r")) {
                unsigned long khz = 0;
                bool ok = std::fscanf(file, "%lu", &khz) == 1 && khz > 0;
                std::fclose(file);
                if (ok) return khz * 1000ULL;
            }

            // Fallback: spin-calibrate against CLOCK_MONOTONIC for ~10ms
            auto to_ns = [](const timespec &time) -> std::uint64_t {
                return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(time.tv_nsec);
            };
            timespec start_time, end_time;
            clock_gettime(CLOCK_MONOTONIC, &start_time);
            std::uint64_t tsc_start = counter();
            do {
                clock_gettime(CLOCK_MONOTONIC, &end_time);
            } while (to_ns(end_time) - to_ns(start_time) < 10'000'000);
            std::uint64_t tsc_end = counter();
            return (tsc_end - tsc_start) * 1'000'000'000ULL / (to_ns(end_time) - to_ns(start_time));
#elif defined(__aarch64__)
            std::uint64_t frequency;
            __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
            return frequency;
#endif
        }

        inline Anchor calibrate() {
            const std::uint64_t frequency = read_counter_freq();

            // Sample multiple times, pick the tightest bracket around clock_gettime
            std::uint64_t best_tsc = 0;
            std::uint64_t best_nanos = 0;
            std::uint64_t best_gap = UINT64_MAX;

            for (int i = 0; i < 16; ++i) {
                std::uint64_t tsc_before = counter();
                timespec timepoint;
                clock_gettime(CLOCK_REALTIME, &timepoint);
                std::uint64_t tsc_after = counter();

                std::uint64_t gap = tsc_after - tsc_before;
                if (gap < best_gap) {
                    best_gap = gap;
                    best_tsc = tsc_before + gap / 2;
                    best_nanos = static_cast<std::uint64_t>(timepoint.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(timepoint.tv_nsec);
                }
            }

            const std::uint64_t multiplier = (1'000'000'000ULL << kShift) / frequency;
            return {best_tsc, best_nanos, multiplier};
        }

        inline const Anchor &get_anchor() {
            static const Anchor anchor = calibrate();
            return anchor;
        }

    }// namespace detail

    inline std::uint64_t ticks_to_nanos(std::uint64_t ticks) {
        const auto &anchor = detail::get_anchor();
        return static_cast<std::uint64_t>(
                (static_cast<__uint128_t>(ticks) * anchor.multiplier) >> detail::kShift);
    }

    inline std::uint64_t nanos_since_epoch() {
        const auto &anchor = detail::get_anchor();
        return anchor.epoch_nanos + ticks_to_nanos(counter() - anchor.tsc);
    }

}// namespace seaplane::tsc
