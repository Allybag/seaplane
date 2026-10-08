#pragma once

#include <seaplane/error.hpp>
#include <seaplane/time.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

namespace seaplane::ring {

    inline constexpr std::uint64_t header_bytes = 2097152;
    inline constexpr std::uint64_t min_capacity = 65536;

    enum class Backing { posix_shm,
                         file };

    enum class Poll { empty,
                      message,
                      lapped };

    struct Cursor {
        std::uint64_t position;
        std::uint64_t sequence;
        std::uint64_t epoch;
    };

    struct Frame {
        std::uint32_t type;
        std::uint64_t sequence;
        std::uint64_t position;
        std::uint64_t publish_ticks;
        std::uint64_t user;
        std::span<const std::byte> payload;
    };

    struct WriterOptions {
        std::string name;
        Backing backing = Backing::posix_shm;
        std::uint64_t capacity;
        std::uint64_t max_message;
        std::uint64_t prefetch_ahead_bytes = 0;
        bool prefault = true;
        bool allow_wrap = true;
    };

    namespace detail {

        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(std::endian::native == std::endian::little);

        inline constexpr std::uint64_t kMagic = 0x474E495250414553ULL;
        inline constexpr std::uint32_t kVersion = 2;
        inline constexpr std::size_t kUserHeaderBytes = 65536;
        inline constexpr std::size_t kSeparation = 128;
        inline constexpr std::uint64_t kLine = 64;

        struct FrameHeader {
            std::uint32_t length;
            std::uint32_t type;
            std::uint64_t sequence;
            std::uint64_t publish_ticks;
            std::uint64_t user;
        };

        static_assert(sizeof(FrameHeader) == 32);

        struct alignas(64) Header {
            std::atomic<std::uint64_t> magic{0};
            std::uint32_t version = 0;
            std::uint32_t header_bytes = 0;
            std::uint64_t capacity = 0;
            std::uint64_t max_message = 0;
            std::uint64_t epoch = 0;
            std::uint64_t writer_pid = 0;
            alignas(64) std::atomic<std::uint64_t> tail_intent{0};
            alignas(64) std::atomic<std::uint64_t> next_sequence{1};
            std::atomic<std::uint64_t> tail{0};
            std::atomic<std::uint64_t> next_sequence_confirm{1};
            alignas(64) std::atomic<std::uint64_t> heartbeat_ticks{0};
            alignas(64) std::byte user_header[kUserHeaderBytes]{};
        };

        static_assert(offsetof(Header, version) == 8);
        static_assert(offsetof(Header, writer_pid) == 40);
        static_assert(offsetof(Header, tail_intent) == 64);
        static_assert(offsetof(Header, next_sequence) == 128);
        static_assert(offsetof(Header, tail) == 136);
        static_assert(offsetof(Header, next_sequence_confirm) == 144);
        static_assert(offsetof(Header, heartbeat_ticks) == 192);
        static_assert(offsetof(Header, user_header) == 256);
        static_assert(sizeof(Header) <= ring::header_bytes);

        inline constexpr std::uint64_t frame_bytes(std::uint64_t length) noexcept {
            return (sizeof(FrameHeader) + length + 7) & ~std::uint64_t{7};
        }

        inline constexpr std::uint64_t line_up(std::uint64_t position) noexcept {
            return (position + kLine - 1) & ~(kLine - 1);
        }

        inline void prefetch_for_write(const std::byte *address) noexcept {
#if defined(__x86_64__)
            __asm__ volatile("prefetchw %0" : : "m"(*address));
#else
            __builtin_prefetch(address, 1, 3);
#endif
        }

        inline std::uint64_t realtime_nanos() noexcept {
            timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(now.tv_nsec);
        }

        [[noreturn]] inline void fail(const char *what, const std::string &name) {
            throw FlushingError{std::format("ring {}: {} failed: {}", name, what, std::strerror(errno))};
        }

        [[noreturn, gnu::cold, gnu::noinline]] inline void corrupt(std::uint64_t position, const char *what) {
            throw FlushingError{std::format("ring: corrupt frame at position {}: {}", position, what)};
        }

        struct Descriptor {
            int fd = -1;

            Descriptor() = default;
            explicit Descriptor(int descriptor) : fd(descriptor) {}
            Descriptor(Descriptor &&other) noexcept : fd(std::exchange(other.fd, -1)) {}
            Descriptor &operator=(Descriptor &&other) noexcept {
                std::swap(fd, other.fd);
                return *this;
            }
            ~Descriptor() {
                if (fd >= 0) ::close(fd);
            }
            explicit operator bool() const noexcept { return fd >= 0; }
        };

        struct Mapping {
            std::byte *base = nullptr;
            std::size_t size = 0;

            Mapping() = default;
            Mapping(Mapping &&other) noexcept : base(std::exchange(other.base, nullptr)), size(std::exchange(other.size, 0)) {}
            Mapping &operator=(Mapping &&other) noexcept {
                std::swap(base, other.base);
                std::swap(size, other.size);
                return *this;
            }
            ~Mapping() {
                if (base) ::munmap(base, size);
            }
        };

        inline std::string object_name(const std::string &name, Backing backing) {
            if (name.empty()) throw FlushingError{"ring: empty name"};
            if (backing == Backing::file) return name;
            auto object = name.front() == '/' ? name : "/" + name;
#if defined(__APPLE__)
            if (object.size() > 31) throw FlushingError{std::format("ring {}: POSIX shm names are limited to 31 characters on macOS", object)};
#endif
            return object;
        }

        inline int open_object(const std::string &name, Backing backing, int flags, mode_t mode) {
            if (backing == Backing::posix_shm) return ::shm_open(name.c_str(), flags, mode);
            return ::open(name.c_str(), flags | O_CLOEXEC, mode);
        }

        inline int unlink_object(const std::string &name, Backing backing) {
            if (backing == Backing::posix_shm) return ::shm_unlink(name.c_str());
            return ::unlink(name.c_str());
        }

        inline void lock_or_throw(int fd, const std::string &name) {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return;
            if (errno == EWOULDBLOCK) throw FlushingError{std::format("ring {}: another writer is running", name)};
            fail("flock", name);
        }

        inline void check_page_size(int fd, std::uint64_t capacity, const std::string &name) {
#if defined(__linux__)
            struct statfs filesystem {};
            if (::fstatfs(fd, &filesystem) != 0) fail("fstatfs", name);
            const auto page = static_cast<std::uint64_t>(filesystem.f_bsize);
            if (filesystem.f_type == HUGETLBFS_MAGIC && (header_bytes % page != 0 || capacity % page != 0)) {
                throw FlushingError{std::format("ring {}: capacity {} and header {} must be multiples of the {} byte huge page", name, capacity, header_bytes, page)};
            }
#else
            static_cast<void>(fd);
            static_cast<void>(capacity);
            static_cast<void>(name);
#endif
        }

        inline void map_fixed(std::byte *address, std::uint64_t length, int prot, int fd, std::uint64_t offset, const std::string &name) {
            void *mapped = ::mmap(address, length, prot, MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(offset));
            if (mapped != address) fail("mmap", name);
        }

#if !defined(__linux__)
        inline void touch_pages(std::byte *begin, std::size_t size, bool write) {
            const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
            volatile std::byte sink{};
            for (std::size_t offset = 0; offset < size; offset += page) {
                auto *byte = static_cast<volatile std::byte *>(begin + offset);
                if (write) {
                    *byte = std::byte{0};
                } else {
                    sink = *byte;
                }
            }
            static_cast<void>(sink);
        }
#endif

        inline Mapping map_ring(int fd, std::uint64_t capacity, bool writable, bool prefault, const std::string &name) {
            Mapping mapping;
            const auto total = header_bytes + 2 * capacity;
            void *reserved = ::mmap(nullptr, total + header_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (reserved == MAP_FAILED) fail("reserve address space", name);
            auto *start = static_cast<std::byte *>(reserved);
            const auto skip = (header_bytes - reinterpret_cast<std::uintptr_t>(start) % header_bytes) % header_bytes;
            if (skip != 0) ::munmap(start, skip);
            ::munmap(start + skip + total, header_bytes - skip);
            mapping.base = start + skip;
            mapping.size = total;

            const int prot = writable ? PROT_READ | PROT_WRITE : PROT_READ;
            map_fixed(mapping.base, header_bytes, prot, fd, 0, name);
            map_fixed(mapping.base + header_bytes, capacity, prot, fd, header_bytes, name);
            map_fixed(mapping.base + header_bytes + capacity, capacity, prot, fd, header_bytes, name);
#if defined(__linux__)
            if (prefault && ::madvise(mapping.base, mapping.size, writable ? MADV_POPULATE_WRITE : MADV_POPULATE_READ) != 0) fail("prefault", name);
#else
            if (prefault) touch_pages(mapping.base, mapping.size, writable);
#endif
            return mapping;
        }

#if defined(__APPLE__)
        inline std::string lock_path(const std::string &object) {
            return std::format("/tmp/seaplane-ring-{}.lock", object.substr(1));
        }
#endif

    }// namespace detail

    inline void remove(const std::string &name, Backing backing = Backing::posix_shm) {
        const auto object = detail::object_name(name, backing);
        if (detail::unlink_object(object, backing) != 0 && errno != ENOENT) detail::fail("unlink", object);
    }

    class Reader;

    class alignas(detail::kSeparation) Writer {
    public:
        explicit Writer(const WriterOptions &options) {
            if (!std::has_single_bit(options.capacity) || options.capacity < min_capacity) {
                throw FlushingError{std::format("ring {}: capacity {} must be a power of two of at least {}", options.name, options.capacity, min_capacity)};
            }
            if (options.max_message > std::numeric_limits<std::uint32_t>::max() || detail::frame_bytes(options.max_message) > options.capacity) {
                throw FlushingError{std::format("ring {}: max_message {} does not fit in capacity {}", options.name, options.max_message, options.capacity)};
            }
            if (options.prefetch_ahead_bytes > options.capacity / 2) {
                throw FlushingError{std::format("ring {}: prefetch_ahead_bytes {} exceeds half the capacity {}", options.name, options.prefetch_ahead_bytes, options.capacity)};
            }

            const auto object = detail::object_name(options.name, options.backing);
#if defined(__APPLE__)
            if (options.backing == Backing::posix_shm) {
                lock = detail::Descriptor{::open(detail::lock_path(object).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)};
                if (!lock) detail::fail("open lock file", object);
                detail::lock_or_throw(lock.fd, object);
            }
#endif
            {
                detail::Descriptor previous{detail::open_object(object, options.backing, O_RDWR, 0)};
                if (previous) {
                    if (!lock) detail::lock_or_throw(previous.fd, object);
                    if (detail::unlink_object(object, options.backing) != 0 && errno != ENOENT) detail::fail("unlink previous", object);
                } else if (errno != ENOENT) {
                    detail::fail("open previous", object);
                }

                file = detail::Descriptor{detail::open_object(object, options.backing, O_RDWR | O_CREAT | O_EXCL, 0600)};
                if (!file && errno == EEXIST) throw FlushingError{std::format("ring {}: another writer created it first", object)};
                if (!file) detail::fail("create", object);
                if (!lock) detail::lock_or_throw(file.fd, object);
            }

            detail::check_page_size(file.fd, options.capacity, object);
            if (::ftruncate(file.fd, static_cast<off_t>(header_bytes + options.capacity)) != 0) detail::fail("ftruncate", object);
            mapping = detail::map_ring(file.fd, options.capacity, true, options.prefault, object);

            header = new (mapping.base) detail::Header{};
            header->version = detail::kVersion;
            header->header_bytes = static_cast<std::uint32_t>(header_bytes);
            header->capacity = options.capacity;
            header->max_message = options.max_message;
            header->epoch = detail::realtime_nanos();
            header->writer_pid = static_cast<std::uint64_t>(::getpid());
            header->heartbeat_ticks.store(tsc::counter(), std::memory_order_relaxed);
            header->magic.store(detail::kMagic, std::memory_order_release);

            data = mapping.base + header_bytes;
            capacity = options.capacity;
            mask = options.capacity - 1;
            max_message = options.max_message;
            prefetch_ahead = options.prefetch_ahead_bytes;
            allow_wrap = options.allow_wrap;
            prefetch();
        }

        std::span<std::byte> claim(std::uint64_t length) {
            if (length > max_message) {
                throw FlushingError{std::format("ring: message of {} bytes exceeds max_message {}", length, max_message)};
            }
            const auto end = tail + detail::frame_bytes(length);
            if (!allow_wrap && end > capacity) [[unlikely]] {
                throw FlushingError{std::format("ring: append-only ring is full, a {} byte frame does not fit in the {} bytes remaining", detail::frame_bytes(length), capacity - tail)};
            }
            claim_ticks = tsc::counter();
            intent = std::max(intent, end);
            header->tail_intent.store(intent, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            claimed = length;
            claiming = true;
            return {data + (tail & mask) + sizeof(detail::FrameHeader), length};
        }

        void commit(std::uint32_t type, std::uint64_t user, std::uint64_t length) {
            if (!claiming || length > claimed) {
                throw FlushingError{std::format("ring: commit of {} bytes without a matching claim", length)};
            }
            const detail::FrameHeader frame{static_cast<std::uint32_t>(length), type, sequence, claim_ticks, user};
            std::memcpy(data + (tail & mask), &frame, sizeof frame);
            tail += detail::frame_bytes(length);
            ++sequence;
            claiming = false;
            header->next_sequence.store(sequence, std::memory_order_relaxed);
            header->tail.store(tail, std::memory_order_release);
            header->next_sequence_confirm.store(sequence, std::memory_order_release);
            header->heartbeat_ticks.store(claim_ticks, std::memory_order_relaxed);
            prefetch();
        }

        void publish(std::uint32_t type, std::uint64_t user, std::span<const std::byte> payload) {
            auto destination = claim(payload.size());
            if (!payload.empty()) std::memcpy(destination.data(), payload.data(), payload.size());
            commit(type, user, payload.size());
        }

        void heartbeat() {
            header->heartbeat_ticks.store(tsc::counter(), std::memory_order_relaxed);
        }

        Cursor next() const { return {tail, sequence, header->epoch}; }

        std::uint64_t remaining_bytes() const { return tail < capacity ? capacity - tail : 0; }

        std::span<std::byte> user_header() { return header->user_header; }

    private:
        friend class Reader;

        void prefetch() noexcept {
            if (prefetch_ahead == 0) return;
            const auto end = detail::line_up(allow_wrap ? tail + prefetch_ahead : std::min(tail + prefetch_ahead, capacity));
            for (auto line = std::max(prefetched, detail::line_up(tail)); line < end; line += detail::kLine) {
                detail::prefetch_for_write(data + (line & mask));
            }
            prefetched = std::max(prefetched, end);
        }

        detail::Descriptor lock;
        detail::Descriptor file;
        detail::Mapping mapping;
        detail::Header *header = nullptr;
        std::byte *data = nullptr;
        std::uint64_t capacity = 0;
        std::uint64_t mask = 0;
        std::uint64_t max_message = 0;
        std::uint64_t prefetch_ahead = 0;
        std::uint64_t prefetched = 0;
        std::uint64_t tail = 0;
        std::uint64_t intent = 0;
        std::uint64_t sequence = 1;
        std::uint64_t claim_ticks = 0;
        std::uint64_t claimed = 0;
        bool allow_wrap = true;
        bool claiming = false;
    };

    class alignas(detail::kSeparation) Reader {
    public:
        explicit Reader(const std::string &name, Backing backing = Backing::posix_shm, bool prefault = true) {
            const auto object = detail::object_name(name, backing);
            detail::Descriptor file{detail::open_object(object, backing, O_RDONLY, 0)};
            if (!file) detail::fail("open", object);

            struct stat info {};
            if (::fstat(file.fd, &info) != 0) detail::fail("fstat", object);
            const auto total = static_cast<std::uint64_t>(info.st_size);
            if (total < header_bytes + min_capacity || !std::has_single_bit(total - header_bytes)) {
                throw FlushingError{std::format("ring {}: not an initialised version {} ring (size {})", object, detail::kVersion, total)};
            }
            capacity = total - header_bytes;

            mapping = detail::map_ring(file.fd, capacity, false, prefault, object);
            header = reinterpret_cast<const detail::Header *>(mapping.base);
            if (header->magic.load(std::memory_order_acquire) != detail::kMagic || header->version != detail::kVersion ||
                header->header_bytes != header_bytes || header->capacity != capacity) {
                throw FlushingError{std::format("ring {}: not an initialised version {} ring", object, detail::kVersion)};
            }

            data = mapping.base + header_bytes;
            mask = capacity - 1;
            max_message = header->max_message;
            buffer.resize(max_message + 1);
            attach_at_tail();
        }

        explicit Reader(const Writer &writer)
            : header(writer.header), data(writer.data), capacity(writer.capacity), mask(writer.mask), max_message(writer.max_message) {
            buffer.resize(max_message + 1);
            attach_at_tail();
        }

        void attach_at_tail() {
            const auto start = std::chrono::steady_clock::now();
            for (std::uint64_t attempt = 1;; ++attempt) {
                const auto confirmed = header->next_sequence_confirm.load(std::memory_order_acquire);
                const auto tail = header->tail.load(std::memory_order_acquire);
                const auto next = header->next_sequence.load(std::memory_order_acquire);
                if (confirmed == next) {
                    current = {tail, next, header->epoch};
                    return;
                }
                if (attempt % 1024 == 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds{1}) {
                    throw FlushingError{"ring: writer stopped in the middle of a publish"};
                }
            }
        }

        bool attach_at(Cursor cursor) {
            if (cursor.epoch != header->epoch) return false;
            if (cursor.position > header->tail.load(std::memory_order_acquire)) return false;
            if (!intact(cursor.position)) return false;
            current = cursor;
            return true;
        }

        Poll poll(Frame &frame) {
            detail::FrameHeader head;
            const auto result = load_header(head);
            if (result != Poll::message) return result;

            const auto position = current.position;
            std::memcpy(buffer.data(), data + (position & mask) + sizeof head, head.length);
            if (!intact(position)) return Poll::lapped;

            buffer[head.length] = std::byte{0};
            fill(frame, head, position, buffer.data());
            current = {position + detail::frame_bytes(head.length), head.sequence + 1, current.epoch};
            return Poll::message;
        }

        Poll peek(Frame &frame) {
            detail::FrameHeader head;
            const auto result = load_header(head);
            if (result != Poll::message) return result;

            const auto position = current.position;
            fill(frame, head, position, data + (position & mask) + sizeof head);
            return Poll::message;
        }

        bool validate(const Frame &frame) const { return intact(frame.position); }

        void advance(const Frame &frame) {
            current = {frame.position + detail::frame_bytes(frame.payload.size()), frame.sequence + 1, current.epoch};
        }

        Cursor cursor() const { return current; }

        std::uint64_t lag_bytes() const {
            return header->tail.load(std::memory_order_acquire) - current.position;
        }

        std::uint64_t writer_age_nanos() const {
            const auto beat = header->heartbeat_ticks.load(std::memory_order_relaxed);
            const auto now = tsc::counter();
            return now > beat ? tsc::ticks_to_nanos(now - beat) : 0;
        }

        std::uint64_t epoch() const { return header->epoch; }

        std::span<const std::byte> user_header() const { return header->user_header; }

    private:
        bool intact(std::uint64_t position) const {
            std::atomic_thread_fence(std::memory_order_acquire);
            return header->tail_intent.load(std::memory_order_relaxed) - position <= capacity;
        }

        Poll load_header(detail::FrameHeader &head) const {
            const auto tail = header->tail.load(std::memory_order_acquire);
            const auto position = current.position;
            if (position == tail) return Poll::empty;
            if (tail - position > capacity) return Poll::lapped;

            std::memcpy(&head, data + (position & mask), sizeof head);
            if (!intact(position)) return Poll::lapped;

            if (head.sequence != current.sequence) [[unlikely]] detail::corrupt(position, "unexpected sequence");
            if (head.length > max_message) [[unlikely]] detail::corrupt(position, "length exceeds max_message");
            if (position + detail::frame_bytes(head.length) > tail) [[unlikely]] detail::corrupt(position, "frame extends past tail");
            return Poll::message;
        }

        static void fill(Frame &frame, const detail::FrameHeader &head, std::uint64_t position, const std::byte *payload) {
            frame.type = head.type;
            frame.sequence = head.sequence;
            frame.position = position;
            frame.publish_ticks = head.publish_ticks;
            frame.user = head.user;
            frame.payload = {payload, head.length};
        }

        detail::Mapping mapping;
        const detail::Header *header = nullptr;
        const std::byte *data = nullptr;
        std::uint64_t capacity = 0;
        std::uint64_t mask = 0;
        std::uint64_t max_message = 0;
        Cursor current{0, 1, 0};
        std::vector<std::byte> buffer;
    };

}// namespace seaplane::ring
