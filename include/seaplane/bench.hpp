#pragma once

#include <cstdint>
#include <cstdio>
#include <print>

namespace seaplane::bench {

    inline std::uint64_t counter_start() noexcept {
#if defined(__x86_64__)
        std::uint32_t low, high;
        __asm__ volatile("lfence\n\trdtsc" : "=a"(low), "=d"(high)::"memory");
        return (static_cast<std::uint64_t>(high) << 32) | low;
#elif defined(__aarch64__) && defined(__APPLE__)
        std::uint64_t val;
        __asm__ volatile("dsb ish\n\tisb\n\tmrs %0, cntpct_el0" : "=r"(val)::"memory");
        return val;
#elif defined(__aarch64__)
        std::uint64_t val;
        __asm__ volatile("dsb ish\n\tisb\n\tmrs %0, cntvct_el0" : "=r"(val)::"memory");
        return val;
#else
#error "Unsupported architecture"
#endif
    }

    inline std::uint64_t counter_end() noexcept {
#if defined(__x86_64__)
        std::uint32_t low, high, aux;
        __asm__ volatile("rdtscp\n\tlfence" : "=a"(low), "=d"(high), "=c"(aux)::"memory");
        return (static_cast<std::uint64_t>(high) << 32) | low;
#elif defined(__aarch64__) && defined(__APPLE__)
        std::uint64_t val;
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(val)::"memory");
        return val;
#elif defined(__aarch64__)
        std::uint64_t val;
        __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(val)::"memory");
        return val;
#else
#error "Unsupported architecture"
#endif
    }

    template<typename T>
    inline void keep(T &val) noexcept {
        if constexpr (sizeof(T) <= 2 * sizeof(void *)) {
            __asm__ volatile("" : "+r,m"(val)::"memory");
        } else {
            __asm__ volatile("" : "+m"(val)::"memory");
        }
    }

    template<typename T>
    inline void keep(const T &val) noexcept {
        if constexpr (sizeof(T) <= 2 * sizeof(void *)) {
            __asm__ volatile("" ::"r,m"(val) : "memory");
        } else {
            __asm__ volatile("" ::"m"(val) : "memory");
        }
    }

    inline void clobber_memory() noexcept {
        __asm__ volatile("" ::: "memory");
    }

    namespace detail {

        inline std::uint64_t calibrate_overhead() noexcept {
            std::uint64_t best = UINT64_MAX;
            for (int i = 0; i < 1024; ++i) {
                auto start = counter_start();
                clobber_memory();
                auto end = counter_end();
                auto elapsed = end - start;
                if (elapsed < best) best = elapsed;
            }
            return best;
        }

        inline std::uint64_t overhead() noexcept {
            static const std::uint64_t val = calibrate_overhead();
            return val;
        }

    }// namespace detail

    struct Result {
        std::uint64_t min;
        std::uint64_t max;
        std::uint64_t total;
        std::uint64_t count;

        std::uint64_t avg() const noexcept { return count > 0 ? total / count : 0; }

        void print(const char *label, std::FILE *file = stdout) const {
            std::println(file, "{:<24}  min: {:6}  avg: {:6}  max: {:6}  (n={})",
                         label, min, avg(), max, count);
        }
    };

    template<typename F>
    inline std::uint64_t measure_once(F &&func) {
        auto start = counter_start();
        func();
        auto end = counter_end();
        auto elapsed = end - start;
        auto overhead = detail::overhead();
        return elapsed > overhead ? elapsed - overhead : 0;
    }

    template<typename F>
    inline Result measure(F &&func, std::uint64_t iterations) {
        Result result{UINT64_MAX, 0, 0, iterations};
        auto overhead = detail::overhead();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            auto start = counter_start();
            func();
            auto end = counter_end();
            auto raw = end - start;
            auto elapsed = raw > overhead ? raw - overhead : 0;
            if (elapsed < result.min) result.min = elapsed;
            if (elapsed > result.max) result.max = elapsed;
            result.total += elapsed;
        }
        return result;
    }

    template<typename F>
    inline std::uint64_t measure_batch(F &&func, std::uint64_t iterations) {
        auto start = counter_start();
        for (std::uint64_t i = 0; i < iterations; ++i) {
            func();
        }
        auto end = counter_end();
        auto overhead = detail::overhead();
        auto total = end - start;
        total = total > overhead ? total - overhead : 0;
        return total / iterations;
    }

}// namespace seaplane::bench
