#include <seaplane/time.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <format>
#include <print>
#include <source_location>
#include <string_view>
#include <thread>

namespace {

    using namespace seaplane;
    using namespace std::chrono_literals;

    int failures = 0;

    void check(bool ok, std::string_view what, std::source_location location = std::source_location::current()) {
        if (!ok) {
            ++failures;
            std::println("    failed at line {}: {}", location.line(), what);
        }
    }

    template<typename F>
    void run(std::string_view name, F &&test) {
        const int before = failures;
        try {
            test();
        } catch (const std::exception &error) {
            ++failures;
            std::println("    threw: {}", error.what());
        }
        std::println("{} {}", failures == before ? "ok  " : "FAIL", name);
        std::fflush(stdout);
    }

    std::int64_t difference(std::uint64_t a, std::uint64_t b) {
        return static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b);
    }

    struct World {
        std::uint64_t true_frequency;
        std::uint64_t realtime_at_zero = 1'700'000'000'000'000'000ULL;

        tsc::detail::Sample at(double seconds) const {
            const auto nanos = static_cast<std::uint64_t>(seconds * 1e9);
            const auto ticks = static_cast<std::uint64_t>(static_cast<long double>(nanos) * true_frequency / 1e9L);
            return {ticks, realtime_at_zero + nanos};
        }

        tsc::detail::Anchor startup(std::uint64_t assumed_frequency) const {
            const auto start = at(0.0);
            return {start.tsc, start.realtime_nanos, start.realtime_nanos, assumed_frequency,
                    tsc::detail::multiplier_for(assumed_frequency, 1.0)};
        }
    };

    std::uint64_t realtime_now() {
        timespec timepoint;
        clock_gettime(CLOCK_REALTIME, &timepoint);
        return static_cast<std::uint64_t>(timepoint.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(timepoint.tv_nsec);
    }

}// namespace

int main() {
    using namespace tsc::detail;
    using tsc::Calibration;

    run("slow counter is measured and the offset slewed out over the next window", [] {
        const World world{999'993'000};
        const Anchor start = world.startup(1'000'000'000);
        Calibration first{};
        const Anchor after_first = recalibrate(start, world.at(360.0), first);

        check(difference(world.at(360.0).realtime_nanos, reading_at(start, world.at(360.0).tsc)) == first.offset_nanos, "offset is realtime minus the reading");
        check(first.offset_nanos > 2'500'000 && first.offset_nanos < 2'540'000, std::format("offset {} ns", first.offset_nanos));
        check(first.realtime_window_nanos == 360'000'000'000LL, "window is the realtime elapsed");
        check(first.frequency >= 999'992'999 && first.frequency <= 999'993'001, std::format("frequency {}", first.frequency));
        check(!first.stepped, "a few ms is drift, not a step");
        check(first.rate_ppm > 6.9 && first.rate_ppm < 7.1, std::format("rate {} ppm", first.rate_ppm));
        check(reading_at(after_first, world.at(360.0).tsc) == reading_at(start, world.at(360.0).tsc), "the reading is continuous across the anchor");

        const auto later = world.at(720.0);
        const auto error = difference(later.realtime_nanos, reading_at(after_first, later.tsc));
        check(std::abs(error) < 2'000, std::format("error after slewing {} ns", error));
        Calibration second{};
        const Anchor after_second = recalibrate(after_first, later, second);
        check(std::abs(second.rate_ppm) < 0.01, std::format("second rate {} ppm", second.rate_ppm));
        check(second.frequency >= 999'992'999 && second.frequency <= 999'993'001, std::format("second frequency {}", second.frequency));
        const auto final_error = difference(world.at(1080.0).realtime_nanos, reading_at(after_second, world.at(1080.0).tsc));
        check(std::abs(final_error) < 100, std::format("error two windows on {} ns", final_error));
    });

    run("fast counter slews backwards without the reading going backwards", [] {
        const World world{1'000'020'000};
        const Anchor start = world.startup(1'000'000'000);
        Calibration first{};
        const Anchor after = recalibrate(start, world.at(100.0), first);
        check(first.offset_nanos < -1'990'000 && first.offset_nanos > -2'010'000, std::format("offset {} ns", first.offset_nanos));
        check(first.rate_ppm < -19.9 && first.rate_ppm > -20.1, std::format("rate {} ppm", first.rate_ppm));
        std::uint64_t previous = reading_at(after, world.at(100.0).tsc);
        for (double seconds = 100.001; seconds <= 200.0; seconds += 1.0) {
            const auto reading = reading_at(after, world.at(seconds).tsc);
            check(reading > previous, "monotonic while slewing back");
            previous = reading;
        }
        const auto error = difference(world.at(200.0).realtime_nanos, reading_at(after, world.at(200.0).tsc));
        check(std::abs(error) < 2'000, std::format("error after slewing {} ns", error));
    });

    run("an offset beyond the slew cap is a realtime step: followed at once, frequency kept", [] {
        const World world{1'000'000'000};
        const Anchor start = world.startup(1'000'000'000);
        auto now = world.at(60.0);
        now.realtime_nanos += 50'000'000;
        Calibration result{};
        const Anchor after = recalibrate(start, now, result);
        check(result.stepped, "stepped");
        check(result.offset_nanos == 50'000'000, std::format("offset {}", result.offset_nanos));
        check(result.applied, "applied");
        check(result.frequency == 1'000'000'000, "frequency kept");
        check(result.rate_ppm == 0.0, "no slew");
        check(reading_at(after, now.tsc) == now.realtime_nanos, "reads realtime after the step");
    });

    run("a short window changes nothing", [] {
        const World world{999'000'000};
        const Anchor start = world.startup(1'000'000'000);
        Calibration result{};
        const Anchor after = recalibrate(start, world.at(0.5), result);
        check(!result.applied, "nothing applied");
        check(result.realtime_window_nanos == 500'000'000, "realtime window reported");
        check(result.counter_window_nanos > 499'000'000 && result.counter_window_nanos < 500'000'000, "counter window is by the assumed frequency");
        check(after.multiplier == start.multiplier && after.tsc == start.tsc && after.epoch_nanos == start.epoch_nanos, "anchor unchanged");
    });

    run("the slew cap is the step threshold: at the cap slews, above it steps", [] {
        const World world{1'000'000'000};
        const Anchor start = world.startup(1'000'000'000);
        auto at_cap = world.at(10.0);
        at_cap.realtime_nanos += 2'000'000;
        Calibration result{};
        const Anchor after = recalibrate(start, at_cap, result);
        check(!result.stepped, "200 ppm over 10 s slews");
        check(std::abs(result.rate_ppm - kMaxSlewPpm) < 1e-9, std::format("rate {} ppm", result.rate_ppm));
        check(after.multiplier > start.multiplier, "running faster");

        auto above = world.at(10.0);
        above.realtime_nanos += 2'000'001;
        const Anchor stepped = recalibrate(start, above, result);
        check(result.stepped, "just above the cap steps");
        check(reading_at(stepped, above.tsc) == above.realtime_nanos, "reads realtime after the step");
    });

    run("30 ppm on a 6 minute cadence is measured and slewed, not stepped", [] {
        const World world{999'970'000};
        const Anchor start = world.startup(1'000'000'000);
        Calibration first{};
        const Anchor after_first = recalibrate(start, world.at(360.0), first);
        check(!first.stepped, "10.8 ms over 360 s is drift");
        check(first.offset_nanos > 10'700'000 && first.offset_nanos < 10'900'000, std::format("offset {} ns", first.offset_nanos));
        check(first.frequency >= 999'969'999 && first.frequency <= 999'970'001, std::format("frequency {}", first.frequency));
        check(first.rate_ppm > 29.9 && first.rate_ppm < 30.1, std::format("rate {} ppm", first.rate_ppm));

        Calibration second{};
        const Anchor after_second = recalibrate(after_first, world.at(720.0), second);
        check(!second.stepped, "second round is not a step either");
        check(std::abs(second.offset_nanos) < 5'000, std::format("second offset {} ns", second.offset_nanos));
        check(std::abs(second.rate_ppm) < 0.02, std::format("second rate {} ppm", second.rate_ppm));
        const auto error = difference(world.at(1080.0).realtime_nanos, reading_at(after_second, world.at(1080.0).tsc));
        check(std::abs(error) < 100, std::format("error two windows on {} ns", error));
    });

    run("a backward realtime step is followed at once", [] {
        const World world{1'000'000'000};
        const Anchor start = world.startup(1'000'000'000);
        auto now = world.at(60.0);
        now.realtime_nanos -= 3'600'000'000'000ULL;
        Calibration result{};
        const Anchor after = recalibrate(start, now, result);
        check(result.applied, "applied");
        check(result.stepped, "stepped");
        check(result.realtime_window_nanos < 0, std::format("realtime window {} ns", result.realtime_window_nanos));
        check(result.counter_window_nanos == 60'000'000'000ULL, "counter window is the elapsed time");
        check(result.frequency == 1'000'000'000, "frequency kept");
        check(reading_at(after, now.tsc) == now.realtime_nanos, "reads realtime after the step");
        check(reading_at(after, world.at(61.0).tsc) == now.realtime_nanos + 1'000'000'000, "and keeps running");
    });

    run("a reader whose counter value predates the anchor reads just before its epoch", [] {
        const World world{24'000'000};
        const Anchor anchor{1'000'000, world.realtime_at_zero, world.realtime_at_zero, 24'000'000, multiplier_for(24'000'000, 1.0)};
        const auto at_anchor = reading_at(anchor, anchor.tsc);
        const auto one_before = reading_at(anchor, anchor.tsc - 1);
        const auto two_before = reading_at(anchor, anchor.tsc - 2);
        check(at_anchor == anchor.epoch_nanos, "epoch at the anchor");
        check(one_before < at_anchor && at_anchor - one_before <= 42, std::format("one tick before reads {} ns earlier", at_anchor - one_before));
        check(two_before < one_before, "monotonic across the anchor");
    });

    run("the live clock calibrates and stays monotonic", [] {
        const auto before = tsc::nanos_since_epoch();
        const Calibration first = tsc::calibrate();
        check(!first.applied, "first call right after the anchor is a no-op");
        std::this_thread::sleep_for(1100ms);
        const auto mid = tsc::nanos_since_epoch();
        const Calibration second = tsc::calibrate();
        const auto after = tsc::nanos_since_epoch();
        check(second.applied, "a second on is long enough to measure");
        check(mid > before && after >= mid, "monotonic across calibration");
        const double frequency_change = std::abs(static_cast<double>(second.frequency) - static_cast<double>(tsc::frequency())) / static_cast<double>(second.frequency);
        check(frequency_change < 1e-6, "the measured frequency is now the clock's");
        check(std::abs(second.rate_ppm) <= kMaxSlewPpm, std::format("rate {} ppm", second.rate_ppm));
        std::println("    offset {} ns over {} ms, frequency {} Hz (was {}), rate {:.3f} ppm, bracket {} ns{}", second.offset_nanos,
                     second.realtime_window_nanos / 1'000'000, second.frequency, second.previous_frequency, second.rate_ppm,
                     second.sample_gap_nanos, second.stepped ? ", stepped" : "");
        check(second.previous_frequency == first.frequency, "previous frequency is what the first call saw");
        check(second.sample_gap_nanos < 100'000, std::format("bracket {} ns", second.sample_gap_nanos));
        const auto realtime_gap = difference(realtime_now(), tsc::nanos_since_epoch());
        check(std::abs(realtime_gap) < 50'000'000, std::format("within 50 ms of realtime ({} ns)", realtime_gap));
    });

    if (failures != 0) {
        std::println("{} failures", failures);
        return EXIT_FAILURE;
    }
    std::println("all passed");
    return EXIT_SUCCESS;
}
