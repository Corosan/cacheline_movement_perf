// vim: textwidth=100
#include "tests.h"

#include <cmath>
#include <ostream>
#include <iomanip>
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
    asm volatile ("");
}

inline std::uint64_t rdtsc() {
    std::uint64_t res;
    asm volatile ("rdtsc\n"
                  "shl $32, %%rdx\n"
                  "or %%rdx, %0\n"
                  : "=a" (res)
                  :
                  : "cc", "rdx");
    return res;
}

inline std::uint64_t produce_and_get_cycles(std::uint32_t val) {
    std::uint64_t res;
    asm volatile ("mov %2, %1\n"
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
    asm volatile ("mov %2, %1\n"
                  "rdtsc\n"
                  "shl $32, %%rdx\n"
                  "or %%rdx, %0\n"
                  : "=a" (res), "=r" (val)
                  : "m" (g_test_data)
                  : "cc", "rdx");
    return res;
}

void calc_and_print_stat(std::ostream& os, std::vector<double>& samples, double cpufreq_ghz) {
    sort(samples.begin(), samples.end());

    // cut off edges from the samples sequence
    const std::size_t edge = samples.size() > 6 ? 3 : 0;
    auto mean = std::accumulate(samples.begin() + edge, samples.end() - edge, 0.0) / (samples.size() - 2 * edge);
    auto rms = std::pow(
        std::accumulate(samples.begin() + edge, samples.end() - edge, 0.0,
                [mean](auto l, auto r){ return std::pow(r - mean, 2.0) + l; })
            / (samples.size() - 2 * edge),
        0.5);

    if (cpufreq_ghz) {
        os <<
            "  freq, GHz    : " << cpufreq_ghz << "\n"
            "  measures     : " << samples.size() << "\n"
            "  cycles mean  : " << mean << " (" << mean / cpufreq_ghz << "ns)\n"
            "  cycles rms   : " << rms << " (" << rms / cpufreq_ghz << "ns)\n"
            "  cycles median: " << samples[samples.size() / 2] << " (" << samples[samples.size() / 2] << "ns)";
    } else {
        os <<
            "  freq, GHz    : ???\n"
            "  measures     : " << samples.size() << "\n"
            "  cycles mean  : " << mean << "\n"
            "  cycles rms   : " << rms << "\n"
            "  cycles median: " << samples[samples.size() / 2];
    }
}

} // ns anonymous

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

    for (std::uint32_t attempt = 0; attempt < m_config.m_attempts_count; ++attempt) {
        m_continue.store(1, std::memory_order_relaxed);

        do {
            // it's possible to get [end_cycle] < [start_cycle] in rare cases because it's read
            // before test data is checked. The logic here is to eliminate duration of the rdtsc
            // call in average.
            *end_cycle = rdtsc();
        }
        while (g_test_data.load(std::memory_order_relaxed) != data_sample);

        code_barrier();

        ++end_cycle;
        ++data_sample;
    }

    m_continue.store(-1);
}

void one_side_test::report(std::ostream& os, double cpufreq_ghz) {
    std::vector<double> samples;

    samples.reserve(m_start_cycles.size());
    for (std::size_t i = 0; i < m_start_cycles.size(); ++i)
        if (m_end_cycles[i])
            samples.push_back(static_cast<double>(m_end_cycles[i]) - static_cast<double>(m_start_cycles[i]));

    calc_and_print_stat(os, samples, cpufreq_ghz);
}

void one_side_asm_test::one_work() noexcept {
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

        *start_cycle = produce_and_get_cycles(data_sample);

        code_barrier();

        ++start_cycle;
        ++data_sample;
    }
}

void one_side_asm_test::another_work() noexcept {
    auto end_cycle = &m_end_cycles[0];
    std::uint32_t data_sample = 1;

    for (std::uint32_t attempt = 0; attempt < m_config.m_attempts_count; ++attempt) {
        m_continue.store(1, std::memory_order_relaxed);

        std::uint32_t v;
        do {
            // it's possible to get [end_cycle] < [start_cycle] in rare cases because it's read
            // before test data is checked. The logic here is to eliminate duration of the rdtsc
            // call in average.
            *end_cycle = consume_and_get_cycles(v);
        }
        while (v != data_sample);

        code_barrier();

        ++end_cycle;
        ++data_sample;
    }

    m_continue.store(-1);
}

void one_side_asm_relax_branch_pred_test::another_work() noexcept {
    auto end_cycle = &m_end_cycles[0];
    std::uint32_t data_sample = 1;

    for (std::uint32_t attempt = 0; attempt < m_config.m_attempts_count; ++attempt) {
        m_continue.store(1, std::memory_order_relaxed);

        // it's possible to get [end_cycle] < [start_cycle] in rare cases because it's read before
        // test data is checked. The logic here is to eliminate duration of the rdtsc call in
        // average.
        // Moreover it's possible to not find the expected test data state in case of this thread
        // was freezed unexpectedly.
        for (auto& sample : m_samples)
            sample.second = consume_and_get_cycles(sample.first);

        code_barrier();

        if (auto it = find_if(m_samples.begin(), m_samples.end(),
            [data_sample](auto& v){ return v.first == data_sample; }); it != m_samples.end())
            *end_cycle = it->second;

        ++end_cycle;
        ++data_sample;
    }

    m_continue.store(-1);
}


void ping_pong_test::one_work() noexcept {
    for (auto& cycles_on_attempt : m_cycles) {
        g_test_data.store(0, std::memory_order_relaxed);
        cycles_on_attempt = rdtsc();
        for (std::uint32_t i = 0; i < s_ping_pongs; ++i) {
            std::uint32_t v = i;
            while (! g_test_data.compare_exchange_strong(v, i+1, std::memory_order_relaxed, std::memory_order_relaxed))
                v = i;
            ++i;
        }
        cycles_on_attempt = rdtsc() - cycles_on_attempt;
    }
}

void ping_pong_test::another_work() noexcept {
    for (auto attempt = m_cycles.size(); attempt != 0; --attempt) {
        for (std::uint32_t i = 1; i < s_ping_pongs - 1; ++i) {
            std::uint32_t v = i;
            while (! g_test_data.compare_exchange_strong(v, i+1, std::memory_order_relaxed, std::memory_order_relaxed))
                v = i;
            ++i;
        }
    }
}

void ping_pong_test::report(std::ostream& os, double cpufreq_ghz) {
    std::vector<double> samples;

    samples.reserve(m_cycles.size());
    for (std::size_t i = 0; i < m_cycles.size(); ++i)
        samples.push_back(static_cast<double>(m_cycles[i]) / s_ping_pongs);

    calc_and_print_stat(os, samples, cpufreq_ghz);
}
