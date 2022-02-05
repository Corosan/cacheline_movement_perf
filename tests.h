// vim: textwidth=100
#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include <utility>
#include <iosfwd>

/*
 * A test case consists of two sequences run in separate threads bound to specified CPU cores.
 * Every sequence consists of two parts: preparation and the main part ( dance:) ). Before
 * the main part starts the runner guarantees that preparation phases for both threads are
 * finished. It's done using user-space barrier.
 */
struct test_case_iface {
    enum class parse_args : char {
        ok, invalid, unknown
    };
    struct config {
        std::uint32_t m_attempts_count = 1000;
    };

    virtual ~test_case_iface() = default;
    virtual void set_config(const config& cfg) = 0;
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

/*
 * The test just writes a data in one thread and waits for it coming in another thread
 */
class one_side_test : public test_case_iface {
protected:
    static constexpr int s_warmup_cycles = 1000;

    std::atomic<std::int8_t> m_continue{0};
    config m_config;

    // Store start and end cycles separately by each thread to not get possible cache ping-pong
    std::vector<std::uint64_t> m_start_cycles;
    std::vector<std::uint64_t> m_end_cycles;

    void set_config(const config& cfg) override { m_config = cfg; }
    //parse_args process_args(std::ostream& err_os, const int argc, const char** argv, int& arg_idx) override;

    void one_prepare() override {
        m_start_cycles.resize(m_config.m_attempts_count);
    }

    void another_prepare() override {
        m_end_cycles.resize(m_config.m_attempts_count);
    }

    void one_work() noexcept override;
    void another_work() noexcept override;
    void report(std::ostream& os) override;
public:
    static void usage(std::ostream& os);
};

/*
 * The same as previous but writing data and getting CPU tsc is made in one asm block
 */
class one_side_asm_test : public one_side_test {
    void one_work() noexcept override;
    void another_work() noexcept override;
public:
    static void usage(std::ostream& os);
};

class one_side_asm_relax_branch_pred_test : public one_side_asm_test {
    static constexpr std::size_t s_samples_size = 10000;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> m_samples;

    void another_prepare() override {
        one_side_asm_test::another_prepare();
        m_samples.resize(s_samples_size);
    }
    void another_work() noexcept override;
public:
    static void usage(std::ostream& os);
};

/*
 * The test increments data many times in two threads sequentially and measures duration
 * of the whole operation
 */
class ping_pong_test : public test_case_iface {
    static constexpr std::uint32_t s_ping_pongs = 100;

    config m_config;
    std::vector<std::uint64_t> m_cycles;

    void set_config(const config& cfg) override { m_config = cfg; }
    // possibly handle additional arguments from a command line
    virtual parse_args process_args(std::ostream& err_os, const int argc, const char** argv, int& arg_idx)
    {
        return parse_args::unknown;
    }
    void one_prepare() override { m_cycles.resize(m_config.m_attempts_count); }
    void another_prepare() override {};
    void one_work() noexcept override;
    void another_work() noexcept override;
    void report(std::ostream& os) override;
public:
    static void usage(std::ostream& os);
};

