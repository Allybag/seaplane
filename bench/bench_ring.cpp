#include <seaplane/args.hpp>
#include <seaplane/bench.hpp>
#include <seaplane/error.hpp>
#include <seaplane/ring.hpp>
#include <seaplane/time.hpp>

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sched.h>
#endif

namespace {

    using namespace seaplane;

    constexpr std::uint32_t kStop = 1;

    enum class Mode { copying,
                      zero_copy };

    struct Summary {
        std::uint64_t count;
        std::uint64_t laps;
        std::uint64_t p50;
        std::uint64_t p90;
        std::uint64_t p99;
        std::uint64_t p999;
        std::uint64_t max;
    };

    void pin(std::int64_t core) {
#if defined(__linux__)
        if (core < 0) return;
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<int>(core), &set);
        if (::sched_setaffinity(0, sizeof set, &set) != 0) {
            throw FlushingError{std::format("sched_setaffinity to core {} failed: {}", core, std::strerror(errno))};
        }
#else
        static_cast<void>(core);
#endif
    }

    Summary summarise(std::vector<std::uint64_t> &nanos, std::uint64_t laps) {
        std::ranges::sort(nanos);
        auto at = [&](double quantile) -> std::uint64_t {
            if (nanos.empty()) return 0;
            const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(nanos.size())));
            return nanos[std::clamp<std::size_t>(rank, 1, nanos.size()) - 1];
        };
        return {nanos.size(), laps, at(0.5), at(0.9), at(0.99), at(0.999), nanos.empty() ? 0 : nanos.back()};
    }

    void print_row(std::string_view label, std::uint64_t size, const Summary &summary, std::string_view extra) {
        std::println("{:<10} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>10}",
                     label, size, summary.count, extra, summary.p50, summary.p90, summary.p99, summary.p999, summary.max);
    }

    std::uint64_t ticks_per_micro() {
        return std::max<std::uint64_t>(1, 1'000'000'000ULL / std::max<std::uint64_t>(1, tsc::ticks_to_nanos(1'000'000'000ULL) / 1000));
    }

    struct Random {
        std::uint64_t state;
        std::uint64_t next() {
            state += 0x9E3779B97F4A7C15ULL;
            auto value = state;
            value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
            value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
            return value ^ (value >> 31);
        }
    };

    std::uint64_t prefetch_bytes(const OptionMap &options) {
        return static_cast<std::uint64_t>(options.at("prefetchBytes").as<std::int64_t>());
    }

    ring::Backing backing_option(const OptionMap &options) {
        const auto backing = options.at("backing").as<std::string>();
        if (backing == "shm") return ring::Backing::posix_shm;
        if (backing == "file") return ring::Backing::file;
        throw FlushingError{std::format("-backing must be shm or file, not {}", backing)};
    }

    std::vector<std::int64_t> number_list(const OptionMap &options, const std::string &key) {
        const auto list = options.at(key).as<std::string>();
        std::vector<std::int64_t> numbers;
        for (std::size_t begin = 0; begin <= list.size();) {
            const auto comma = std::min(list.find(',', begin), list.size());
            std::int64_t number = 0;
            const auto [end, error] = std::from_chars(list.data() + begin, list.data() + comma, number);
            if (error != std::errc{} || end != list.data() + comma) throw FlushingError{std::format("-{} must be a comma-separated list of numbers, not {}", key, list)};
            numbers.push_back(number);
            begin = comma + 1;
        }
        return numbers;
    }

    std::vector<std::int64_t> reader_cores(const OptionMap &options) {
        return number_list(options, "readerCores");
    }

    void write_all(int fd, const void *data, std::size_t size) {
        const auto *bytes = static_cast<const char *>(data);
        while (size > 0) {
            const auto written = ::write(fd, bytes, size);
            if (written <= 0) ::_exit(4);
            bytes += written;
            size -= static_cast<std::size_t>(written);
        }
    }

    bool read_all(int fd, void *data, std::size_t size) {
        auto *bytes = static_cast<char *>(data);
        while (size > 0) {
            const auto got = ::read(fd, bytes, size);
            if (got <= 0) return false;
            bytes += got;
            size -= static_cast<std::size_t>(got);
        }
        return true;
    }

    [[noreturn]] void read_frames(const std::string &name, ring::Backing backing, Mode mode, std::int64_t core, std::uint64_t expected, int ready, int results) {
        try {
            pin(core);
            ring::Reader reader(name, backing);
            std::vector<std::uint64_t> nanos;
            nanos.reserve(expected);
            std::uint64_t laps = 0;
            ring::Frame frame{};
            const char byte = 1;
            write_all(ready, &byte, 1);

            for (;;) {
                const auto result = mode == Mode::copying ? reader.poll(frame) : reader.peek(frame);
                if (result == ring::Poll::empty) continue;
                if (result == ring::Poll::lapped || (mode == Mode::zero_copy && !reader.validate(frame))) {
                    ++laps;
                    reader.attach_at_tail();
                    continue;
                }
                const auto arrived = tsc::counter();
                if (mode == Mode::zero_copy) reader.advance(frame);
                if (frame.type == kStop) break;
                nanos.push_back(arrived > frame.publish_ticks ? tsc::ticks_to_nanos(arrived - frame.publish_ticks) : 0);
            }

            const std::uint64_t counts[2]{laps, nanos.size()};
            write_all(results, counts, sizeof counts);
            write_all(results, nanos.data(), nanos.size() * sizeof(std::uint64_t));
            ::_exit(0);
        } catch (...) {
            ::_exit(2);
        }
    }

    Summary measure_hop(ring::Writer &writer, const std::string &name, Mode mode, std::uint64_t size, const OptionMap &options) {
        const auto frames = static_cast<std::uint64_t>(options.at("frames").as<std::int64_t>());
        const auto cores = reader_cores(options);
        int ready[2];
        if (::pipe(ready) != 0) throw FlushingError{"pipe failed"};

        struct Child {
            pid_t pid;
            int results;
        };
        std::vector<Child> children;
        for (const auto core : cores) {
            int results[2];
            if (::pipe(results) != 0) throw FlushingError{"pipe failed"};
            const auto pid = ::fork();
            if (pid < 0) throw FlushingError{"fork failed"};
            if (pid == 0) read_frames(name, backing_option(options), mode, core, frames, ready[1], results[1]);
            ::close(results[1]);
            children.push_back({pid, results[0]});
        }
        for (std::size_t started = 0; started < children.size(); ++started) {
            char byte = 0;
            if (!read_all(ready[0], &byte, 1)) throw FlushingError{"reader did not start"};
        }

        const std::vector<std::byte> payload(size, std::byte{0x5a});
        const auto per_micro = ticks_per_micro();
        const auto min_gap = static_cast<std::uint64_t>(options.at("minGapMicros").as<std::int64_t>()) * per_micro;
        const auto max_gap = static_cast<std::uint64_t>(options.at("maxGapMicros").as<std::int64_t>()) * per_micro;
        Random random{size};
        for (std::uint64_t i = 0; i < frames; ++i) {
            const auto deadline = tsc::counter() + min_gap + random.next() % (max_gap - min_gap + 1);
            while (tsc::counter() < deadline) {}
            writer.publish(0, i, payload);
        }
        writer.publish(kStop, 0, {});

        std::vector<std::uint64_t> nanos;
        std::uint64_t laps = 0;
        bool failed = false;
        for (const auto &child : children) {
            std::uint64_t counts[2]{};
            if (read_all(child.results, counts, sizeof counts)) {
                const auto offset = nanos.size();
                nanos.resize(offset + counts[1]);
                failed |= !read_all(child.results, nanos.data() + offset, counts[1] * sizeof(std::uint64_t));
                laps += counts[0];
            } else {
                failed = true;
            }
            int status = 0;
            ::waitpid(child.pid, &status, 0);
            ::close(child.results);
            failed |= !WIFEXITED(status) || WEXITSTATUS(status) != 0;
        }
        ::close(ready[0]);
        ::close(ready[1]);
        if (failed) throw FlushingError{"a reader failed"};
        return summarise(nanos, laps);
    }

    Summary measure_publish(ring::Writer &writer, std::uint64_t size) {
        const std::vector<std::byte> payload(size, std::byte{0x5a});
        const auto iterations = std::clamp<std::uint64_t>((512ULL << 20) / std::max<std::uint64_t>(size, 1), 1000, 100'000);
        std::vector<std::uint64_t> nanos;
        nanos.reserve(iterations);
        for (std::uint64_t i = 0; i < iterations; ++i) {
            const auto start = tsc::counter();
            writer.publish(0, i, payload);
            nanos.push_back(tsc::ticks_to_nanos(tsc::counter() - start));
        }
        return summarise(nanos, 0);
    }

    double batch_nanos(std::uint64_t iterations, auto &&body) {
        const auto start = tsc::counter();
        for (std::uint64_t i = 0; i < iterations; ++i) body(i);
        return static_cast<double>(tsc::ticks_to_nanos(tsc::counter() - start)) / static_cast<double>(iterations);
    }

    void measure_replay(const std::string &name, const OptionMap &options) {
        constexpr std::uint64_t size = 505;
        const auto backing = backing_option(options);
        const auto capacity = static_cast<std::uint64_t>(options.at("replayMb").as<std::int64_t>()) << 20;
        const std::vector<std::byte> payload(size, std::byte{0x5a});

        auto start = tsc::counter();
        ring::Writer writer({.name = name, .backing = backing, .capacity = capacity, .max_message = size, .prefetch_ahead_bytes = prefetch_bytes(options), .allow_wrap = false});
        const auto create = tsc::ticks_to_nanos(tsc::counter() - start);
        std::uint64_t frames = 0;
        start = tsc::counter();
        while (writer.remaining_bytes() >= ring::detail::frame_bytes(size)) writer.publish(0, frames++, payload);
        const auto fill = tsc::ticks_to_nanos(tsc::counter() - start);

        pin(reader_cores(options).front());
        start = tsc::counter();
        ring::Reader reader(name, backing);
        const auto attach = tsc::ticks_to_nanos(tsc::counter() - start);
        if (!reader.attach_at({0, 1, reader.epoch()})) throw FlushingError{"replay reader could not attach at position zero"};

        ring::Frame frame{};
        std::uint64_t read = 0;
        std::uint64_t checksum = 0;
        start = tsc::counter();
        while (reader.poll(frame) == ring::Poll::message) {
            checksum += frame.user;
            ++read;
        }
        const auto replay = tsc::ticks_to_nanos(tsc::counter() - start);
        bench::keep(checksum);
        if (read != frames) throw FlushingError{std::format("replay read {} of {} frames", read, frames)};

        const auto seconds = static_cast<double>(replay) / 1e9;
        std::println("\nreplay from position zero, {} MB append-only ring, {} byte payloads, copying reader", capacity >> 20, size);
        std::println("writer create {:.1f} ms, fill {:.1f} ns per frame, reader attach {:.1f} ms",
                     static_cast<double>(create) / 1e6, static_cast<double>(fill) / static_cast<double>(frames), static_cast<double>(attach) / 1e6);
        std::println("{} frames in {:.3f} s: {:.1f} ns per frame, {:.2f} M frames/s, {:.2f} GB/s of ring",
                     frames, seconds, static_cast<double>(replay) / static_cast<double>(frames), static_cast<double>(frames) / seconds / 1e6,
                     static_cast<double>(writer.next().position) / seconds / 1e9);
    }

}// namespace

int main(int argc, const char **argv) {
    OptionMap defaults;
    defaults["name"] = "/seaplane_bench_ring";
    defaults["frames"] = 5000;
    defaults["writerCore"] = 2;
    defaults["readerCores"] = "3";
    defaults["prefetchBytes"] = 0;
    defaults["capacityMb"] = 256;
    defaults["minGapMicros"] = 50;
    defaults["maxGapMicros"] = 2000;
    defaults["backing"] = "shm";
    defaults["replayMb"] = 1024;
    defaults["sizes"] = "64,322,1024,1696,16384,2097152";
    defaults["hop"] = true;
    defaults["publishAlone"] = true;
    defaults["zeroCopy"] = true;
    defaults["replay"] = true;
    const auto options = parse_args(defaults, argc, argv);

    const auto name = options.at("name").as<std::string>();
    const auto capacity = static_cast<std::uint64_t>(options.at("capacityMb").as<std::int64_t>()) << 20;
    std::vector<std::uint64_t> sizes;
    for (const auto size : number_list(options, "sizes")) sizes.push_back(static_cast<std::uint64_t>(size));

    pin(options.at("writerCore").as<std::int64_t>());
    std::println("counter: {:.3f} ns per tick", static_cast<double>(tsc::ticks_to_nanos(1'000'000'000ULL)) / 1e9);
    std::println("ring: {} MB, prefetch ahead {} bytes, backing {}, writer core {}, reader cores {}",
                 capacity >> 20, prefetch_bytes(options), options.at("backing").as<std::string>(),
                 options.at("writerCore").as<std::int64_t>(), options.at("readerCores").as<std::string>());

    const auto backing = backing_option(options);
    if (options.at("hop").as<bool>()) {
        ring::Writer writer({.name = name, .backing = backing, .capacity = capacity, .max_message = std::ranges::max(sizes), .prefetch_ahead_bytes = prefetch_bytes(options)});
        ring::Reader idle(name, backing);
        ring::Frame frame{};
        const auto empty_poll = batch_nanos(10'000'000, [&](std::uint64_t) { bench::keep(idle.poll(frame)); });
        const auto lag = batch_nanos(10'000'000, [&](std::uint64_t) { bench::keep(idle.lag_bytes()); });
        std::println("empty poll: {:.1f} ns, lag_bytes: {:.1f} ns (mean over a tight loop of 10M calls)", empty_poll, lag);

        if (options.at("publishAlone").as<bool>()) {
            std::println("\npublish cost in the writer alone, no reader (ns); mean from a batch, percentiles per call");
            std::println("{:<10} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>10}", "", "bytes", "n", "mean", "p50", "p90", "p99", "p99.9", "max");
            for (auto size : sizes) {
                const std::vector<std::byte> payload(size, std::byte{0x5a});
                const auto iterations = std::clamp<std::uint64_t>((512ULL << 20) / std::max<std::uint64_t>(size, 1), 1000, 1'000'000);
                const auto mean = batch_nanos(iterations, [&](std::uint64_t i) { writer.publish(0, i, payload); });
                print_row("publish", size, measure_publish(writer, size), std::format("{:.1f}", mean));
            }
        }

        std::println("\nhop latency, reader tick at poll return minus frame.publish_ticks (ns), all readers merged, writer gaps {}..{} us",
                     options.at("minGapMicros").as<std::int64_t>(), options.at("maxGapMicros").as<std::int64_t>());
        std::println("{:<10} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>10}", "reader", "bytes", "n", "laps", "p50", "p90", "p99", "p99.9", "max");
        for (auto mode : {Mode::copying, Mode::zero_copy}) {
            if (mode == Mode::zero_copy && !options.at("zeroCopy").as<bool>()) continue;
            for (auto size : sizes) {
                const auto summary = measure_hop(writer, name, mode, size, options);
                print_row(mode == Mode::copying ? "copying" : "zero-copy", size, summary, std::format("{}", summary.laps));
                std::fflush(stdout);
            }
        }

        ring::remove(name, backing);
    }

    if (options.at("replay").as<bool>()) {
        const auto replay_name = name + "_replay";
        measure_replay(replay_name, options);
        ring::remove(replay_name, backing);
    }
    return 0;
}
