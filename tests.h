#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include <iosfwd>

struct test_case_iface {
    enum class parse_args : char {
        ok, invalid, unknown
    };

    virtual ~test_case_iface() = default;
    // possibly handle additional arguments from a command line
    virtual parse_args process_args(std::ostream& err_os, const int argc, const char** argv, int& arg_idx)
    {
        return parse_args::unknown;
    }
    // preparation step for the first worker before the main dance begins
    virtual void one_prepare() = 0;
    // preparation step for the second worker before the main dance begins
    virtual void another_prepare() = 0;
    // the main dance of the first worker
    virtual void one_work() noexcept = 0;
    // the main dance of the second worker
    virtual void another_work() noexcept = 0;
    // say what you want to say at the end
    virtual void report(std::ostream& os) = 0;
};

class one_side_test : public test_case_iface {
    static constexpr int s_warmup_cycles = 1000;

    std::atomic<std::int8_t> m_continue{0};
    std::uint32_t m_attempts_count = 1000;

    // Store start and end cycles separately by each thread to not get possible cache ping-pong
    std::vector<std::uint64_t> m_start_cycles;
    std::vector<std::uint64_t> m_end_cycles;

    parse_args process_args(std::ostream& err_os, const int argc, const char** argv, int& arg_idx);

    void one_prepare() override {
        m_start_cycles.resize(m_attempts_count);
    }

    void another_prepare() override {
        m_end_cycles.resize(m_attempts_count);
    }

    void one_work() noexcept override;
    void another_work() noexcept override;
    void report(std::ostream& os) override;
public:
    static void usage(std::ostream& os);
};
