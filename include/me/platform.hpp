#pragma once
// ---------------------------------------------------------------------------
// platform.hpp — the few things that differ between Apple Silicon (your Mac)
// and an x86 Linux box (a typical trading server).
// ---------------------------------------------------------------------------
#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>  // _mm_pause
#endif

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace me {

// Size of a CPU cache line. Two variables written by different threads must
// live on DIFFERENT cache lines, otherwise the cores fight over the line
// ("false sharing"). Apple M-series cores use 128-byte lines; x86 uses 64.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// Hint to the CPU that we are inside a spin-wait loop. It saves power and,
// on hyper-threaded x86, gives the sibling thread the execution resources.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Keep a latency-critical thread on one core so its caches stay warm and the
// OS doesn't migrate it. Linux supports hard pinning. macOS does NOT allow
// pinning to a core; the best we can do is ask for the highest QoS class,
// which makes the scheduler prefer performance (P) cores.
inline void pin_current_thread([[maybe_unused]] int cpu) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

}  // namespace me
