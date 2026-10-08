#include <seaplane/ring.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <initializer_list>
#include <print>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

    using namespace seaplane;
    using namespace std::chrono_literals;

    std::atomic<int> failures = 0;
    std::string skipped;

    void check(bool ok, std::string_view what, std::source_location location = std::source_location::current()) {
        if (!ok) {
            ++failures;
            std::println("    failed at line {}: {}", location.line(), what);
        }
    }

    void skip(std::string reason) {
        skipped = std::move(reason);
    }

    template<typename F>
    void run(std::string_view name, F &&test) {
        const int before = failures;
        skipped.clear();
        try {
            test();
        } catch (const std::exception &error) {
            ++failures;
            std::println("    threw: {}", error.what());
        }
        if (failures == before && !skipped.empty()) {
            std::println("skip {} ({})", name, skipped);
        } else {
            std::println("{} {}", failures == before ? "ok  " : "FAIL", name);
        }
        std::fflush(stdout);
    }

    struct ScopedName {
        std::string name;
        ScopedName() : name(std::format("/sprt{}_{}", ::getpid(), count++)) {}
        ~ScopedName() { ring::remove(name); }
        static inline int count = 0;
    };

    std::uint64_t mix(std::uint64_t value) {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31);
    }

    struct Random {
        std::uint64_t state;
        std::uint64_t next() { return state = mix(state); }
        std::uint64_t below(std::uint64_t limit) { return next() % limit; }
        std::uint64_t size(std::uint64_t large) { return below(4) == 0 ? below(large + 1) : below(513); }
    };

    void fill(std::span<std::byte> payload, std::uint64_t seed) {
        auto state = seed;
        for (std::size_t offset = 0; offset < payload.size(); offset += 8) {
            state = mix(state);
            std::memcpy(payload.data() + offset, &state, std::min<std::size_t>(8, payload.size() - offset));
        }
    }

    std::uint64_t checksum(std::span<const std::byte> payload, std::uint64_t seed) {
        auto hash = mix(seed ^ payload.size());
        std::size_t offset = 0;
        for (; offset + 8 <= payload.size(); offset += 8) {
            std::uint64_t word;
            std::memcpy(&word, payload.data() + offset, 8);
            hash = mix(hash ^ word);
        }
        for (; offset < payload.size(); ++offset) hash = mix(hash ^ std::to_integer<std::uint64_t>(payload[offset]));
        return hash;
    }

    void publish_checked(ring::Writer &writer, std::uint64_t length, std::uint32_t type = 0) {
        const auto sequence = writer.next().sequence;
        auto payload = writer.claim(length);
        fill(payload, sequence);
        writer.commit(type, checksum(payload, sequence), length);
    }

    bool intact(const ring::Frame &frame) {
        return frame.user == checksum(frame.payload, frame.sequence);
    }

    ring::WriterOptions options(const std::string &name, std::uint64_t capacity, std::uint64_t max_message) {
        return {.name = name, .capacity = capacity, .max_message = max_message};
    }

    template<typename F>
    int run_child(F &&body) {
        const auto pid = ::fork();
        if (pid == 0) {
            try {
                body();
            } catch (...) {
                ::_exit(100);
            }
            ::_exit(0);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        return status;
    }

    bool faulted(int status) {
        return WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
    }

    void round_trip() {
        ScopedName scoped;
        constexpr std::uint64_t max = 65536;
        ring::Writer writer(options(scoped.name, 1 << 20, max));
        ring::Reader reader(scoped.name);
        ring::Frame frame{};
        check(reader.poll(frame) == ring::Poll::empty, "fresh ring polls empty");

        std::uint64_t sequence = 1;
        std::uint64_t position = 0;
        for (std::uint64_t length : std::initializer_list<std::uint64_t>{0, 1, 7, 8, 9, 4096, max}) {
            std::vector<std::byte> payload(length);
            fill(payload, length);
            const auto before = tsc::counter();
            writer.publish(static_cast<std::uint32_t>(length), length * 3, payload);
            check(reader.lag_bytes() == ring::detail::frame_bytes(length), "lag is one frame");
            check(reader.poll(frame) == ring::Poll::message, std::format("message of {} bytes", length));
            check(frame.type == length && frame.user == length * 3, "type and user round trip");
            check(frame.sequence == sequence && frame.position == position, "sequence and position");
            check(frame.publish_ticks >= before && frame.publish_ticks <= tsc::counter(), "publish ticks");
            check(std::ranges::equal(frame.payload, payload), std::format("payload of {} bytes", length));
            position += ring::detail::frame_bytes(length);
            ++sequence;
            check(reader.cursor().position == position && reader.cursor().sequence == sequence, "cursor advanced");
        }
        check(reader.poll(frame) == ring::Poll::empty, "drained ring polls empty");
        check(reader.lag_bytes() == 0, "no lag when drained");
        check(position % 8 == 0 && ring::detail::frame_bytes(9) == 48 && ring::detail::frame_bytes(0) == 32, "padding");

        const std::vector<std::byte> ones(100, std::byte{0xff});
        for (std::size_t length : {std::size_t{100}, std::size_t{10}, std::size_t{0}}) {
            writer.publish(0, 0, std::span{ones}.first(length));
            check(reader.poll(frame) == ring::Poll::message && frame.payload.size() == length, "message before terminator check");
            check(frame.payload.data()[length] == std::byte{0}, std::format("copied payload of {} bytes is NUL-terminated", length));
        }
    }

    void wrap() {
        ScopedName scoped;
        constexpr std::uint64_t capacity = 65536;
        auto prefetching = options(scoped.name, capacity, 4096);
        prefetching.prefetch_ahead_bytes = 8192;
        ring::Writer writer(prefetching);
        ring::Reader reader(scoped.name);
        Random random{7};
        ring::Frame frame{};
        std::uint64_t frames = 0;
        std::uint64_t bad = 0;
        while (writer.next().position < 20 * capacity) {
            publish_checked(writer, random.size(4096));
            if (reader.poll(frame) != ring::Poll::message || !intact(frame) || frame.sequence != ++frames) ++bad;
        }
        check(bad == 0, std::format("{} bad frames across wraps", bad));
    }

    void continuity() {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 1 << 20, 4096));
        ring::Reader reader(scoped.name);
        Random random{11};
        ring::Frame frame{};
        std::uint64_t expected = 1;
        std::uint64_t bad = 0;
        std::uint64_t laps = 0;
        while (expected <= 2'000'000) {
            const auto batch = 1 + random.below(64);
            for (std::uint64_t i = 0; i < batch; ++i) publish_checked(writer, random.size(4096));
            for (;;) {
                const auto result = reader.poll(frame);
                if (result == ring::Poll::empty) break;
                if (result == ring::Poll::lapped) {
                    ++laps;
                    break;
                }
                if (frame.sequence != expected++ || !intact(frame)) ++bad;
            }
            if (laps) break;
        }
        check(laps == 0 && bad == 0, std::format("{} laps, {} bad frames", laps, bad));
        check(writer.next().sequence == expected, "every published frame was read once");
    }

    void peek_validate() {
        ScopedName scoped;
        constexpr std::uint64_t capacity = 65536;
        ring::Writer writer(options(scoped.name, capacity, 4096));
        ring::Reader reader(scoped.name);
        ring::Frame frame{};

        publish_checked(writer, 100);
        check(reader.peek(frame) == ring::Poll::message && intact(frame), "peek");
        check(reader.validate(frame), "fresh peek validates");
        reader.advance(frame);
        check(reader.poll(frame) == ring::Poll::empty, "advance moves past the frame");

        publish_checked(writer, 100);
        check(reader.peek(frame) == ring::Poll::message, "peek second frame");
        while (writer.next().position + ring::detail::frame_bytes(1000) - frame.position <= capacity) publish_checked(writer, 1000);
        check(reader.validate(frame), "still valid one frame before the lap");
        publish_checked(writer, 1000);
        check(!reader.validate(frame), "overwritten frame fails validation");
        ring::Frame lapped{};
        check(reader.poll(lapped) == ring::Poll::lapped, "poll reports lapped");
        check(reader.poll(lapped) == ring::Poll::lapped, "lapped is sticky until reattach");
        reader.attach_at_tail();
        check(reader.poll(lapped) == ring::Poll::empty, "reattached at tail");
    }

    void claim_commit() {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 1 << 20, 4096));
        ring::Reader reader(scoped.name);
        ring::Frame frame{};

        auto span = writer.claim(1000);
        check(span.size() == 1000, "claim returns the requested span");
        fill(span.first(10), 1);
        writer.commit(5, checksum(span.first(10), 1), 10);
        check(writer.next().position == ring::detail::frame_bytes(10), "shrunk commit advances by the committed size");
        check(reader.poll(frame) == ring::Poll::message && frame.payload.size() == 10 && frame.type == 5 && intact(frame), "shrunk frame");

        bool threw = false;
        try {
            writer.commit(0, 0, 0);
        } catch (const FlushingError &) {
            threw = true;
        }
        check(threw, "commit without claim throws");

        threw = false;
        writer.claim(100);
        try {
            writer.commit(0, 0, 101);
        } catch (const FlushingError &) {
            threw = true;
        }
        check(threw, "commit larger than claim throws");

        threw = false;
        try {
            std::vector<std::byte> big(4097);
            writer.publish(0, 0, big);
        } catch (const FlushingError &) {
            threw = true;
        }
        check(threw, "publish over max_message throws");

        for (int i = 0; i < 1000; ++i) publish_checked(writer, 200);
        std::uint64_t good = 0;
        while (reader.poll(frame) == ring::Poll::message) good += intact(frame);
        check(good == 1000, "frames after a shrunk commit read back");
    }

    void attach_snapshot() {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 1 << 20, 4096));
        for (int i = 0; i < 10; ++i) publish_checked(writer, 300);
        const auto snapshot = writer.next();
        for (int i = 0; i < 5; ++i) publish_checked(writer, 300);

        ring::Reader reader(scoped.name);
        check(reader.cursor().sequence == 16, "constructor attaches at tail");
        check(reader.attach_at(snapshot), "attach at snapshot cursor");
        ring::Frame frame{};
        check(reader.poll(frame) == ring::Poll::message, "message after snapshot");
        check(frame.sequence == snapshot.sequence && frame.position == snapshot.position && intact(frame), "first frame is the snapshot's next");
    }

    void attach_stale() {
        ScopedName scoped;
        constexpr std::uint64_t capacity = 65536;
        ring::Writer writer(options(scoped.name, capacity, 4096));
        const auto old = writer.next();
        while (writer.next().position - old.position <= capacity) publish_checked(writer, 1000);
        ring::Reader reader(scoped.name);
        check(!reader.attach_at(old), "stale cursor is lapped at once");
        check(reader.attach_at(writer.next()), "current cursor attaches");
    }

    void attach_rejects() {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 1 << 20, 4096));
        for (int i = 0; i < 10; ++i) publish_checked(writer, 300);
        ring::Reader reader(scoped.name);
        const auto tail = writer.next();
        check(reader.epoch() == tail.epoch && reader.cursor().epoch == tail.epoch, "cursors carry the epoch");

        check(!reader.attach_at({0, 1, tail.epoch + 1}), "cursor with another epoch is rejected");
        check(!reader.attach_at({tail.position + ring::detail::frame_bytes(300), tail.sequence + 1, tail.epoch}), "cursor past the tail is rejected");
        check(reader.cursor().position == tail.position && reader.cursor().sequence == tail.sequence, "rejected attach leaves the cursor alone");
        ring::Frame frame{};
        check(reader.poll(frame) == ring::Poll::empty, "nothing is read after a rejected attach");

        writer.claim(300);
        check(!reader.attach_at({tail.position + ring::detail::frame_bytes(300), tail.sequence + 1, tail.epoch}), "cursor inside an open claim is rejected");
        writer.commit(0, checksum({}, tail.sequence), 0);

        check(reader.attach_at({0, 1, tail.epoch}), "position zero of this epoch attaches");
        std::uint64_t good = 0;
        while (reader.poll(frame) == ring::Poll::message) good += intact(frame);
        check(good == 11, "every frame replays from position zero");
    }

    void append_only() {
        ScopedName scoped;
        constexpr std::uint64_t capacity = 65536;
        ring::Writer writer({.name = scoped.name, .capacity = capacity, .max_message = 4096, .prefetch_ahead_bytes = 4096, .allow_wrap = false});
        check(writer.remaining_bytes() == capacity, "empty ring has the whole capacity remaining");

        Random random{43};
        std::uint64_t published = 0;
        std::uint64_t miscounted = 0;
        bool refused = false;
        while (!refused && writer.next().position <= capacity) {
            const auto before = writer.remaining_bytes();
            const auto length = random.below(4097);
            try {
                publish_checked(writer, length);
            } catch (const FlushingError &) {
                check(ring::detail::frame_bytes(length) > before, "throws only when the frame does not fit");
                check(writer.remaining_bytes() == before && writer.next().sequence == published + 1, "a refused claim changes nothing");
                refused = true;
                break;
            }
            ++published;
            miscounted += writer.remaining_bytes() != before - ring::detail::frame_bytes(length);
        }
        check(refused, "a frame that does not fit is refused");
        check(miscounted == 0, "remaining_bytes counts down by each frame");
        if (!refused) return;

        while (writer.remaining_bytes() >= ring::detail::frame_bytes(0)) {
            publish_checked(writer, std::min<std::uint64_t>(writer.remaining_bytes() - ring::detail::frame_bytes(0), 4096));
            ++published;
        }
        check(writer.remaining_bytes() == 0 && writer.next().position == capacity, "ring filled exactly to capacity");
        bool threw = false;
        try {
            writer.publish(0, 0, {});
        } catch (const FlushingError &) {
            threw = true;
        }
        check(threw, "an empty frame does not fit in a full ring");

        ring::Reader reader(scoped.name);
        check(reader.attach_at({0, 1, reader.epoch()}), "join at position zero of a full ring");
        ring::Frame frame{};
        std::uint64_t good = 0;
        ring::Poll result;
        while ((result = reader.poll(frame)) == ring::Poll::message) good += intact(frame) && frame.sequence == good + 1;
        check(result == ring::Poll::empty && good == published, std::format("{} of {} frames replayed, then empty", good, published));
    }

    std::uint64_t page_faults() {
        rusage usage{};
        ::getrusage(RUSAGE_SELF, &usage);
        return static_cast<std::uint64_t>(usage.ru_minflt + usage.ru_majflt);
    }

    void no_page_faults() {
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
        return skip("sanitizer shadow memory faults on first access");
#endif
        ScopedName scoped;
        constexpr std::uint64_t capacity = 1 << 22;
        auto prefetching = options(scoped.name, capacity, 4096);
        prefetching.prefetch_ahead_bytes = 16384;
        ring::Writer writer(prefetching);
        ring::Reader reader(scoped.name);
        Random random{31};
        ring::Frame frame{};
        std::uint64_t good = 0;
        std::uint64_t frames = 0;
        const auto before = page_faults();
        while (writer.next().position < 3 * capacity) {
            publish_checked(writer, random.size(4096));
            good += reader.poll(frame) == ring::Poll::message && intact(frame);
            ++frames;
        }
        const auto faults = page_faults() - before;
        check(good == frames, "frames read back");
        check(faults == 0, std::format("{} page faults publishing and polling three times round a prefaulted ring", faults));
    }

    std::uint64_t meminfo(std::string_view key) {
        std::ifstream file("/proc/meminfo");
        std::string line;
        while (std::getline(file, line)) {
            if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == ':') return std::stoull(line.substr(key.size() + 1));
        }
        throw FlushingError{std::format("{} not in /proc/meminfo", key)};
    }

    void hugetlbfs() {
#if defined(__linux__)
        const char *configured = std::getenv("SEAPLANE_HUGETLBFS");
        const std::string directory = configured ? configured : "/dev/hugepages";
        if (::access(directory.c_str(), W_OK) != 0) return skip(std::format("{} is not writable; set SEAPLANE_HUGETLBFS", directory));
        if (meminfo("Hugepagesize") != 2048) return skip("default huge page size is not 2MB");

        constexpr std::uint64_t capacity = 8 << 20;
        constexpr std::uint64_t pages = (ring::header_bytes + capacity) >> 21;
        if (meminfo("HugePages_Free") - meminfo("HugePages_Rsvd") < pages) return skip(std::format("fewer than {} free huge pages", pages));
        const auto path = std::format("{}/sprt_{}", directory, ::getpid());
        const auto options = [&](bool prefault) {
            return ring::WriterOptions{.name = path, .backing = ring::Backing::file, .capacity = capacity, .max_message = 1 << 20, .prefault = prefault};
        };

        const auto committed = [] { return meminfo("HugePages_Total") - meminfo("HugePages_Free") + meminfo("HugePages_Rsvd"); };
        const auto baseline = committed();
        const auto reserved = meminfo("HugePages_Rsvd");
        {
            ring::Writer writer(options(false));
            check(committed() - baseline == pages, std::format("lazy writer commits {} pages, not more for the mirror", pages));
            ring::Reader reader(path, ring::Backing::file, false);
            check(committed() - baseline == pages, "a reader commits nothing more");
        }
        ring::remove(path, ring::Backing::file);
        check(committed() == baseline, "removing the ring releases its pages");

        ring::Writer writer(options(true));
        ring::Reader reader(path, ring::Backing::file);
        check(committed() - baseline == pages, std::format("prefaulted writer and reader commit {} pages", pages));
        check(meminfo("HugePages_Rsvd") == reserved, "prefaulted ring leaves nothing reserved and unfaulted");

        Random random{47};
        ring::Frame frame{};
        std::uint64_t good = 0;
        std::uint64_t frames = 0;
        while (writer.next().position < 3 * capacity) {
            publish_checked(writer, random.size(1 << 20));
            good += reader.poll(frame) == ring::Poll::message && intact(frame);
            ++frames;
        }
        check(good == frames, std::format("{} of {} frames read back across wraps", good, frames));
        ring::remove(path, ring::Backing::file);

        bool threw = false;
        try {
            ring::Writer small({.name = path, .backing = ring::Backing::file, .capacity = 1 << 20, .max_message = 1024});
        } catch (const FlushingError &) {
            threw = true;
        }
        check(threw, "capacity below the huge page size is rejected");
        ring::remove(path, ring::Backing::file);
#else
        skip("Linux only");
#endif
    }

    void bad_options() {
        ScopedName scoped;
        int thrown = 0;
        for (auto [capacity, max] : {std::pair{100'000ULL, 16ULL}, std::pair{32768ULL, 16ULL}, std::pair{65536ULL, 65536ULL}}) {
            try {
                ring::Writer writer(options(scoped.name, capacity, max));
            } catch (const FlushingError &) {
                ++thrown;
            }
        }
        try {
            ring::Writer writer({.name = scoped.name, .capacity = 65536, .max_message = 16, .prefetch_ahead_bytes = 65536});
        } catch (const FlushingError &) {
            ++thrown;
        }
        check(thrown == 4, "invalid capacity, max_message or prefetch_ahead_bytes rejected");
    }

    void second_writer() {
        ScopedName scoped;
        ring::Writer first(options(scoped.name, 65536, 1024));
        publish_checked(first, 10);
        ring::Reader before(scoped.name);

        bool threw = false;
        try {
            ring::Writer second(options(scoped.name, 65536, 1024));
        } catch (const FlushingError &) {
            threw = true;
        }
        check(threw, "second writer on the same name fails");

        ring::Reader after(scoped.name);
        check(after.epoch() == before.epoch(), "first writer's object is untouched");
        publish_checked(first, 10);
        ring::Frame frame{};
        check(after.poll(frame) == ring::Poll::message && frame.sequence == 2 && intact(frame), "first writer still publishes");
    }

    void file_backing() {
        const auto path = std::format("/tmp/sprt_file_{}", ::getpid());
        ring::Writer writer({.name = path, .backing = ring::Backing::file, .capacity = 65536, .max_message = 1024});
        ring::Reader reader(path, ring::Backing::file);
        std::uint64_t good = 0;
        ring::Frame frame{};
        for (int i = 0; i < 1000; ++i) {
            publish_checked(writer, 500);
            good += reader.poll(frame) == ring::Poll::message && intact(frame);
        }
        check(good == 1000, "file-backed ring round trips");
        ring::remove(path, ring::Backing::file);
    }

    void cross_process() {
        ScopedName scoped;
        constexpr std::uint64_t total = 500'000;
        ring::Writer writer(options(scoped.name, 1 << 22, 16384));
        int ready[2];
        check(::pipe(ready) == 0, "pipe");

        const auto pid = ::fork();
        if (pid == 0) {
            ::alarm(60);
            try {
                ring::Reader reader(scoped.name);
                char byte = 1;
                if (::write(ready[1], &byte, 1) != 1) ::_exit(3);
                ring::Frame frame{};
                std::uint64_t bad = 0;
                std::uint64_t received = 0;
                std::uint64_t laps = 0;
                while (reader.cursor().sequence <= total) {
                    const auto result = reader.poll(frame);
                    if (result == ring::Poll::message) {
                        bad += !intact(frame);
                        ++received;
                    } else if (result == ring::Poll::lapped) {
                        ++laps;
                        reader.attach_at_tail();
                    }
                }
                std::println("    child read {} frames, {} laps, {} bad", received, laps, bad);
                std::fflush(stdout);
                ::_exit(bad == 0 && received > 0 ? 0 : 1);
            } catch (...) {
                ::_exit(2);
            }
        }

        char byte = 0;
        check(::read(ready[0], &byte, 1) == 1, "child ready");
        Random random{13};
        for (std::uint64_t i = 0; i < total; ++i) publish_checked(writer, random.size(16384));
        int status = 0;
        ::waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, std::format("child reader status {}", status));
        ::close(ready[0]);
        ::close(ready[1]);
    }

    void read_only() {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 65536, 1024));
        publish_checked(writer, 100);

        const auto header_status = run_child([&] {
            ring::Reader reader(scoped.name);
            auto *byte = const_cast<volatile std::byte *>(reader.user_header().data());
            *byte = std::byte{1};
        });
        check(faulted(header_status), std::format("write through reader header faults (status {})", header_status));

        const auto data_status = run_child([&] {
            ring::Reader reader(scoped.name);
            reader.attach_at({0, 1, reader.epoch()});
            ring::Frame frame{};
            if (reader.peek(frame) != ring::Poll::message) ::_exit(4);
            auto *byte = const_cast<volatile std::byte *>(frame.payload.data());
            *byte = std::byte{1};
        });
        check(faulted(data_status), std::format("write through reader data faults (status {})", data_status));

        ring::Reader reader(scoped.name);
        reader.attach_at({0, 1, reader.epoch()});
        ring::Frame frame{};
        check(reader.poll(frame) == ring::Poll::message && intact(frame), "ring unchanged by faulting readers");
    }

    void writer_restart() {
        ScopedName scoped;
        int ready[2];
        check(::pipe(ready) == 0, "pipe");
        const auto pid = ::fork();
        if (pid == 0) {
            try {
                ring::Writer writer(options(scoped.name, 65536, 1024));
                publish_checked(writer, 100);
                char byte = 1;
                if (::write(ready[1], &byte, 1) != 1) ::_exit(3);
                for (;;) {
                    writer.heartbeat();
                    ::usleep(1000);
                }
            } catch (...) {
                ::_exit(2);
            }
        }

        char byte = 0;
        check(::read(ready[0], &byte, 1) == 1, "child writer ready");
        ring::Reader old(scoped.name);
        check(old.writer_age_nanos() < 50'000'000, std::format("live writer age {}ns", old.writer_age_nanos()));

        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        std::this_thread::sleep_for(100ms);
        check(old.writer_age_nanos() >= 90'000'000, std::format("dead writer age {}ns", old.writer_age_nanos()));

        ring::Writer replacement(options(scoped.name, 65536, 1024));
        ring::Reader fresh(scoped.name);
        check(fresh.epoch() != old.epoch(), "new writer has a new epoch");
        check(fresh.writer_age_nanos() < 50'000'000, "new writer is live");

        const auto stale = old.cursor();
        publish_checked(replacement, 1000);
        check(stale.position < replacement.next().position, "stale position lies inside the new incarnation's first frame");
        check(!fresh.attach_at(stale), "cursor from the previous incarnation is rejected");
        check(fresh.attach_at({0, 1, fresh.epoch()}), "cursor from this incarnation attaches");

        check(old.attach_at({0, 1, old.epoch()}), "old mapping accepts its own epoch");
        ring::Frame frame{};
        check(old.poll(frame) == ring::Poll::message && intact(frame), "old mapping still readable");
        ::close(ready[0]);
        ::close(ready[1]);
    }

    ring::Reader open_reader(const ring::Writer &writer, const std::string &name, bool shared) {
        return shared ? ring::Reader(writer) : ring::Reader(name);
    }

    struct ConcurrentResult {
        std::uint64_t received = 0;
        std::uint64_t laps = 0;
        std::uint64_t bad = 0;
    };

    void concurrent_checksums(bool shared) {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 1 << 20, 65536));
        auto reader = open_reader(writer, scoped.name, shared);
        std::atomic<bool> done = false;

        std::thread producer([&] {
            Random random{17};
            for (int i = 0; i < 1'000'000; ++i) publish_checked(writer, random.size(65536));
            done.store(true, std::memory_order_release);
        });

        ConcurrentResult result;
        ring::Frame frame{};
        std::uint64_t expected = reader.cursor().sequence;
        try {
            for (;;) {
                const bool finished = done.load(std::memory_order_acquire);
                const auto poll = reader.poll(frame);
                if (poll == ring::Poll::message) {
                    ++result.received;
                    result.bad += !intact(frame) || frame.sequence != expected;
                    expected = frame.sequence + 1;
                } else if (poll == ring::Poll::lapped) {
                    ++result.laps;
                    reader.attach_at_tail();
                    expected = reader.cursor().sequence;
                } else if (finished) {
                    break;
                }
            }
        } catch (const std::exception &error) {
            check(false, error.what());
        }
        producer.join();
        std::println("    read {} frames, {} laps", result.received, result.laps);
        check(result.bad == 0, std::format("{} bad frames", result.bad));
        check(result.received > 0, "reader received frames");
    }

    void slow_reader(bool shared) {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 65536, 4096));
        auto reader = open_reader(writer, scoped.name, shared);
        std::atomic<bool> stop = false;

        std::thread producer([&] {
            Random random{19};
            while (!stop.load(std::memory_order_relaxed)) publish_checked(writer, random.size(4096));
        });

        ConcurrentResult result;
        ring::Frame frame{};
        try {
            for (int i = 0; i < 200; ++i) {
                for (int burst = 0; burst < 16;) {
                    const auto poll = reader.poll(frame);
                    if (poll == ring::Poll::message) {
                        ++result.received;
                        result.bad += !intact(frame);
                        ++burst;
                    } else if (poll == ring::Poll::lapped) {
                        ++result.laps;
                        reader.attach_at_tail();
                    }
                }
                std::this_thread::sleep_for(1ms);
            }
        } catch (const std::exception &error) {
            check(false, error.what());
        }
        stop = true;
        producer.join();
        std::println("    read {} frames, {} laps", result.received, result.laps);
        check(result.laps > 0, "slow reader is lapped");
        check(result.received > 0, "slow reader reads between laps");
        check(result.bad == 0, std::format("{} bad frames", result.bad));
    }

    void fill_pattern(std::span<std::byte> payload, std::uint64_t word) {
        std::size_t offset = 0;
        for (; offset + 8 <= payload.size(); offset += 8) std::memcpy(payload.data() + offset, &word, 8);
        std::memcpy(payload.data() + offset, &word, payload.size() - offset);
    }

    bool pattern_intact(const ring::Frame &frame) {
        const auto word = mix(frame.sequence);
        if (frame.user != word) return false;
        std::size_t offset = 0;
        for (; offset + 8 <= frame.payload.size(); offset += 8) {
            std::uint64_t value;
            std::memcpy(&value, frame.payload.data() + offset, 8);
            if (value != word) return false;
        }
        std::uint64_t value = word;
        std::memcpy(&value, frame.payload.data() + offset, frame.payload.size() - offset);
        return value == word;
    }

    void torn_reads(bool shared) {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 65536, 16384));
        auto reader = open_reader(writer, scoped.name, shared);
        std::atomic<bool> stop = false;

        std::thread producer([&] {
            Random random{29};
            while (!stop.load(std::memory_order_relaxed)) {
                const auto length = 4096 + random.below(12289);
                const auto word = mix(writer.next().sequence);
                fill_pattern(writer.claim(length), word);
                writer.commit(0, word, length);
            }
        });

        ConcurrentResult result;
        ring::Frame frame{};
        try {
            while (result.received + result.laps < 1'000'000) {
                const auto poll = reader.poll(frame);
                if (poll == ring::Poll::message) {
                    ++result.received;
                    result.bad += !pattern_intact(frame);
                } else if (poll == ring::Poll::lapped) {
                    ++result.laps;
                    reader.attach_at_tail();
                }
            }
        } catch (const std::exception &error) {
            check(false, error.what());
        }
        stop = true;
        producer.join();
        std::println("    read {} frames, {} laps", result.received, result.laps);
        check(result.bad == 0, std::format("{} bad frames", result.bad));
        check(result.laps > 0 && result.received > 0, "reader both reads and is lapped");
    }

    void attach_race(bool shared) {
        ScopedName scoped;
        ring::Writer writer(options(scoped.name, 1 << 22, 4096));
        auto reader = open_reader(writer, scoped.name, shared);
        std::atomic<bool> stop = false;

        std::thread producer([&] {
            Random random{23};
            while (!stop.load(std::memory_order_relaxed)) publish_checked(writer, random.below(257));
        });

        std::uint64_t attaches = 0;
        std::uint64_t bad = 0;
        ring::Frame frame{};
        try {
            for (int i = 0; i < 100'000; ++i) {
                reader.attach_at_tail();
                const auto cursor = reader.cursor();
                ring::Poll poll;
                while ((poll = reader.poll(frame)) == ring::Poll::empty) {}
                if (poll == ring::Poll::message) {
                    ++attaches;
                    bad += frame.sequence != cursor.sequence || frame.position != cursor.position || !intact(frame);
                }
            }
        } catch (const std::exception &error) {
            check(false, error.what());
        }
        stop = true;
        producer.join();
        check(attaches > 0 && bad == 0, std::format("{} attaches, {} bad", attaches, bad));
    }

    void replay_from_zero(bool shared) {
        ScopedName scoped;
        constexpr std::uint64_t before_join = 1'000'000;
        constexpr std::uint64_t total = 2'000'000;
        ring::Writer writer({.name = scoped.name, .capacity = 1 << 28, .max_message = 128, .prefetch_ahead_bytes = 4096, .allow_wrap = false});
        Random random{37};
        for (std::uint64_t i = 0; i < before_join; ++i) publish_checked(writer, random.below(129));

        auto reader = open_reader(writer, scoped.name, shared);
        check(reader.attach_at({0, 1, reader.epoch()}), "join at position zero");
        std::thread producer([&] {
            for (std::uint64_t i = before_join; i < total; ++i) publish_checked(writer, random.below(129));
        });

        ConcurrentResult result;
        ring::Frame frame{};
        try {
            while (result.received < total) {
                const auto poll = reader.poll(frame);
                if (poll == ring::Poll::message) {
                    result.bad += !intact(frame) || frame.sequence != ++result.received;
                } else if (poll == ring::Poll::lapped) {
                    ++result.laps;
                    break;
                }
            }
        } catch (const std::exception &error) {
            check(false, error.what());
        }
        producer.join();
        check(result.laps == 0 && result.bad == 0 && result.received == total, std::format("{} frames, {} laps, {} bad", result.received, result.laps, result.bad));
        check(reader.poll(frame) == ring::Poll::empty, "caught up to the tail");
    }

}// namespace

int main() {
    run("round trip", round_trip);
    run("wrap", wrap);
    run("sequence continuity", continuity);
    run("peek and validate", peek_validate);
    run("claim and commit", claim_commit);
    run("attach at snapshot cursor", attach_snapshot);
    run("attach at stale cursor", attach_stale);
    run("attach rejects other epochs and positions past the tail", attach_rejects);
    run("append-only ring refuses to wrap", append_only);
    run("no page faults after attach", no_page_faults);
    run("hugetlbfs backing", hugetlbfs);
    run("invalid options", bad_options);
    run("second writer", second_writer);
    run("file backing", file_backing);
    run("cross process", cross_process);
    run("read-only mapping", read_only);
    run("writer restart", writer_restart);
    for (bool shared : {false, true}) {
        const auto mapping = shared ? " (shared mapping)" : " (own mapping)";
        run(std::format("concurrent checksums{}", mapping), [&] { concurrent_checksums(shared); });
        run(std::format("slow reader{}", mapping), [&] { slow_reader(shared); });
        run(std::format("torn reads under lapping{}", mapping), [&] { torn_reads(shared); });
        run(std::format("attach at tail while publishing{}", mapping), [&] { attach_race(shared); });
        run(std::format("replay from zero while publishing{}", mapping), [&] { replay_from_zero(shared); });
    }

    std::println("{}", failures == 0 ? "all passed" : std::format("{} failures", failures.load()));
    return failures == 0 ? 0 : 1;
}
