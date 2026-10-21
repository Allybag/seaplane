#pragma once

#include <atomic>
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

    struct Calibration {
        bool applied;
        bool stepped;
        std::int64_t offset_nanos;
        std::int64_t realtime_window_nanos;
        std::uint64_t counter_window_nanos;
        std::uint64_t frequency;
        std::uint64_t previous_frequency;
        double rate_ppm;
        std::uint64_t sample_gap_nanos;
    };

    namespace detail {

        static constexpr int kShift = 32;
        static constexpr std::uint64_t kMinWindowNanos = 1'000'000'000;
        static constexpr double kMaxSlewPpm = 200.0;

        struct Anchor {
            std::uint64_t tsc;
            std::uint64_t epoch_nanos;   // this clock's reading at tsc
            std::uint64_t realtime_nanos;// CLOCK_REALTIME at tsc
            std::uint64_t frequency;     // counter ticks per second
            std::uint64_t multiplier;    // (1e9 << kShift) / frequency, with any slew folded in
        };

        struct Sample {
            std::uint64_t tsc;
            std::uint64_t realtime_nanos;
            std::uint64_t gap_ticks;
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

        // Sample multiple times, pick the tightest bracket around clock_gettime
        inline Sample sample() {
            Sample best{};
            best.gap_ticks = UINT64_MAX;
            for (int i = 0; i < 16; ++i) {
                std::uint64_t tsc_before = counter();
                timespec timepoint;
                clock_gettime(CLOCK_REALTIME, &timepoint);
                std::uint64_t tsc_after = counter();

                std::uint64_t gap = tsc_after - tsc_before;
                if (gap < best.gap_ticks) {
                    best.gap_ticks = gap;
                    best.tsc = tsc_before + gap / 2;
                    best.realtime_nanos = static_cast<std::uint64_t>(timepoint.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(timepoint.tv_nsec);
                }
            }
            return best;
        }

        inline std::uint64_t multiplier_for(std::uint64_t frequency, double rate) {
            const long double nanos_per_tick = static_cast<long double>(1'000'000'000ULL << kShift) / static_cast<long double>(frequency);
            return static_cast<std::uint64_t>(nanos_per_tick * static_cast<long double>(rate));
        }

        inline std::uint64_t to_nanos(const Anchor &anchor, std::uint64_t ticks) {
            return static_cast<std::uint64_t>((static_cast<__uint128_t>(ticks) * anchor.multiplier) >> kShift);
        }

        inline std::uint64_t reading_at(const Anchor &anchor, std::uint64_t tsc) {
            const auto ticks = static_cast<std::int64_t>(tsc - anchor.tsc);
            const auto nanos = static_cast<std::int64_t>((static_cast<__int128_t>(ticks) * static_cast<__int128_t>(anchor.multiplier)) >> kShift);
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(anchor.epoch_nanos) + nanos);
        }

        inline Anchor initial_anchor() {
            const std::uint64_t frequency = read_counter_freq();
            const Sample now = sample();
            return {now.tsc, now.realtime_nanos, now.realtime_nanos, frequency, multiplier_for(frequency, 1.0)};
        }

        inline Anchor recalibrate(const Anchor &anchor, const Sample &now, Calibration &result) {
            const std::uint64_t ticks = now.tsc - anchor.tsc;
            const std::uint64_t counter_window = to_nanos(anchor, ticks);
            const std::int64_t realtime_window = static_cast<std::int64_t>(now.realtime_nanos) - static_cast<std::int64_t>(anchor.realtime_nanos);
            const std::uint64_t reading = reading_at(anchor, now.tsc);
            result = {};
            result.offset_nanos = static_cast<std::int64_t>(now.realtime_nanos) - static_cast<std::int64_t>(reading);
            result.realtime_window_nanos = realtime_window;
            result.counter_window_nanos = counter_window;
            result.frequency = anchor.frequency;
            result.previous_frequency = anchor.frequency;
            result.sample_gap_nanos = to_nanos(anchor, now.gap_ticks);
            if (counter_window < kMinWindowNanos) return anchor;

            result.applied = true;
            Anchor next{now.tsc, reading, now.realtime_nanos, anchor.frequency, anchor.multiplier};
            const double rate_ppm = static_cast<double>(result.offset_nanos) / static_cast<double>(counter_window) * 1e6;
            if (rate_ppm > kMaxSlewPpm || rate_ppm < -kMaxSlewPpm) {
                // A realtime step: a window holding one cannot measure the frequency, so it is kept
                result.stepped = true;
                next.epoch_nanos = now.realtime_nanos;
                next.multiplier = multiplier_for(anchor.frequency, 1.0);
                return next;
            }

            result.frequency = static_cast<std::uint64_t>((static_cast<__uint128_t>(ticks) * 1'000'000'000ULL) / static_cast<std::uint64_t>(realtime_window));
            result.rate_ppm = rate_ppm;
            next.frequency = result.frequency;
            next.multiplier = multiplier_for(result.frequency, 1.0 + rate_ppm * 1e-6);
            return next;
        }

        inline std::atomic<const Anchor *> &anchor_pointer() {
            static const Anchor initial = initial_anchor();
            static std::atomic<const Anchor *> pointer{&initial};
            return pointer;
        }

        inline const Anchor &get_anchor() {
            return *anchor_pointer().load(std::memory_order_acquire);
        }

    }// namespace detail

    inline std::uint64_t ticks_to_nanos(std::uint64_t ticks) {
        return detail::to_nanos(detail::get_anchor(), ticks);
    }

    inline std::uint64_t nanos_since_epoch() {
        const detail::Anchor &anchor = detail::get_anchor();
        return detail::reading_at(anchor, counter());
    }

    inline std::uint64_t frequency() {
        return detail::get_anchor().frequency;
    }

    // Single writer; readers are lock-free
    inline Calibration calibrate() {
        static detail::Anchor slots[2];
        static int slot = 0;
        auto &pointer = detail::anchor_pointer();
        Calibration result{};
        const detail::Anchor next = detail::recalibrate(*pointer.load(std::memory_order_acquire), detail::sample(), result);
        if (!result.applied) return result;
        slots[slot] = next;
        pointer.store(&slots[slot], std::memory_order_release);
        slot ^= 1;
        return result;
    }

}// namespace seaplane::tsc
