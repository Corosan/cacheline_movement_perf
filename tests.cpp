#include "tests.h"

#include <cmath>
#include <ostream>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <string_view>

constexpr std::size_t g_cache_line_size = 64;

namespace {

/*
 * Assume that any operation on caches operates with a data block of one cache line, so it doesn't
 * matter, whether we invalidate one byte or the whole cache line. Surround the data with buffers
 * big enough to eliminate false cache sharing.
 */
char g_pad1[g_cache_line_size];
std::atomic<std::uint32_t> g_test_data;
char g_pad2[g_cache_line_size];

inline void code_barrier() {
    asm inline volatile ("");
}

inline std::uint64_t rdtsc() {
    std::uint64_t res;
    asm inline volatile ("rdtsc\n"
                         "shl $32, %%rdx\n"
                         "or %%rdx, %0\n"
                         : "=a" (res)
                         :
                         : "cc", "rdx");
    return res;
}

inline std::uint64_t produce_and_get_cycles(std::uint32_t val) {
    std::uint64_t res;
    asm inline volatile ("mov %2, %1\n"
                         "rdtsc\n"
                         "shl $32, %%rdx\n"
                         "or %%rdx, %0\n"
                         : "=a" (res), "=m" (g_test_data)
                         : "b" (val)
                         : "cc", "rdx");
    return res;
}

inline std::uint64_t consume_and_get_cycles(std::uint32_t& val) {
    std::uint64_t res;
    asm inline volatile ("mov %2, %1\n"
                         "rdtsc\n"
                         "shl $32, %%rdx\n"
                         "or %%rdx, %0\n"
                         : "=a" (res), "=r" (val)
                         : "m" (g_test_data)
                         : "cc", "rdx");
    return res;
}

} // ns anonymous

void one_side_test::usage(std::ostream& os) {
    os << "  --attempts N - an attempts count";
}

auto one_side_test::process_args(std::ostream& err_os, const int argc, const char** argv, int& arg_idx)
        -> parse_args {
    using namespace std::string_view_literals;

    if ("--attempts"sv == argv[arg_idx] && arg_idx + 1 < argc) {
        std::istringstream is{argv[++arg_idx]};
        is >> m_attempts_count;
        if (is.bad() || is.fail() || ! is.eof()) {
            err_os << "unable to convert attempts count into an acceptable number"sv << std::endl;
            return parse_args::invalid;
        }
        return parse_args::ok;
    }
    return parse_args::unknown;
}

void one_side_test::one_work() noexcept {
    std::int8_t cont;
    std::uint32_t data_sample = 1;
    auto start_cycle = &m_start_cycles[0];

    while (true) {
        do {
            if (cont = m_continue.load(std::memory_order_relaxed); cont < 0)
                return;
        } while (cont == 0);

        m_continue.store(0, std::memory_order_relaxed);

        // give a chance for another side to prepare for waiting the data change
        for (int i = 0; i < s_warmup_cycles; ++i)
            code_barrier();

        *start_cycle = rdtsc();
        g_test_data.store(data_sample, std::memory_order_relaxed);

        code_barrier();

        ++start_cycle;
        ++data_sample;
    }
}

void one_side_test::another_work() noexcept {
    auto end_cycle = &m_end_cycles[0];
    std::uint32_t data_sample = 1;

    for (std::uint32_t attempt = 0; attempt < m_attempts_count; ++attempt) {
        m_continue.store(1, std::memory_order_relaxed);

        do {
            // it's possible to get [end_cycle] < [start_cycle] in rare cases because it's read before
            // test data is checked. The logic here is to eliminate duration of the rdtsc call in average.
            *end_cycle = rdtsc();
        }
        while (g_test_data.load(std::memory_order_relaxed) != data_sample);

        code_barrier();

        ++end_cycle;
        ++data_sample;
    }

    m_continue.store(-1);
}

void one_side_test::report(std::ostream& os) {
    std::vector<std::int64_t> samples;

    samples.reserve(m_attempts_count);
    for (std::size_t i = 0; i < static_cast<std::size_t>(m_attempts_count); ++i)
        samples.push_back(static_cast<std::int64_t>(m_end_cycles[i]) - static_cast<std::int64_t>(m_start_cycles[i]));

    sort(samples.begin(), samples.end());

    auto mean = std::accumulate(samples.begin(), samples.end(), 0.0) / m_attempts_count;
    auto rms = std::pow(
        std::accumulate(samples.begin(), samples.end(), 0.0, [mean](auto l, auto r){
                return std::pow(r - mean, 2.0) + l;
            }) / m_attempts_count,
        0.5);

    os <<
        "  cycles mean  : " << mean << "\n"
        "  cycles rms   : " << rms << "\n"
        "  cycles median: " << samples[m_attempts_count / 2] << std::endl;
}

//inline void warmup() {
//    // setup the cache line being tested into [shared] state by reading some piece of data from
//    // (https://en.wikipedia.org/wiki/MESI_protocol)
//    volatile std::uint32_t dummy = g_test_data.load(std::memory_order_relaxed);
//    (void)dummy;
//}


//void cpu1_worker(spin_latch& start_barrier) {
//    //warmup();
//    if (! set_self_affinity(g_cpu1_cpuid))
//        g_cpu1_errormsg = "unable to set cpu1 affinity";
//
//    start_barrier.arrive_and_wait();
//
//    // wait some time before making an experiment to be sure that consumer is ready
//    for (int i = 0; i < 10000; ++i)
//        asm volatile ("");
///*
////    g_start_cycle = rdtsc();
////    g_test_data.store(1, std::memory_order_relaxed);
//    g_start_cycle = produce_and_get_cycles(1);
//*/
//    g_start_cycle = rdtsc();
//    for (std::uint32_t i = 0; i < 200; ++i) {
//        std::uint32_t v = i;
//        while (! g_test_data.compare_exchange_weak(v, i+1, std::memory_order_relaxed, std::memory_order_relaxed))
//            v = i;
//        ++i;
//    }
//    g_end_cycle = rdtsc();
//}
//
//void consumer1(spin_latch& start_barrier) {
//    warmup();
//    start_barrier.arrive_and_wait();
//
////    uint32_t v;
//    // we hope that the cycle will spin many times before seen expected value so the branch
//    // predictor will be turned to jump back
////    do {
////        g_end_cycle = rdtsc();
////        g_end_cycle = consume_and_get_cycles(v);
////    }
////    while (/*g_test_data.load(std::memory_order_relaxed) != 1*/ v != 1);
//
//    for (std::uint32_t i = 1; i < 200; ++i) {
//        std::uint32_t v = i;
//        while (! g_test_data.compare_exchange_weak(v, i+1, std::memory_order_relaxed, std::memory_order_relaxed))
//            v = i;
//        ++i;
//    }
//};
//
//void consumer2(spin_latch& start_barrier) {
//    constexpr std::size_t wait_window_size = 1024*10;
//    // the vector whould provide warmed buffer for next scanning step
//    std::vector<std::pair<std::uint32_t, std::uint64_t>> samples(wait_window_size);
//    warmup();
//    start_barrier.arrive_and_wait();
//
//    auto samples_ptr = &samples[0];
//    for (std::size_t i = 0; i < wait_window_size; ++i) {
//        samples_ptr->second = rdtsc();
//        samples_ptr++->first = g_test_data.load(std::memory_order_relaxed);
//    }
//
//    samples_ptr = &samples[0];
//    for (std::size_t i = 0; i < wait_window_size; ++i)
//        if (samples_ptr++->first == 1) {
//            g_end_cycle = samples_ptr->second;
//            break;
//        }
//}
