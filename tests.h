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
 * the main part will start the runner guarantees that preparation phases for both threads are
 * finished. It's done using user-space barrier.
 */
struct test_case_iface {
    struct config {
        std::uint32_t m_attempts_count = 1000;
    };

    virtual ~test_case_iface() = default;
    virtual void set_config(const config& cfg) = 0;
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
 * The test just writes a data in one thread and waits for it coming in another thread. Where to put
 * timestamp readers relative to store/load instructions? From practical point of view we are
 * intersting in a duration between two time points: a) we are ready to store a data; b) we have
 * read expected data:
 *
 *         T1                 T2
 *
 *   <-- get timestamp 1
 *   ^   [store]
 *   |
 *   v                       [load]
 *   <---------------------- get timestamp 2
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

    void one_prepare() override {
        m_start_cycles.resize(m_config.m_attempts_count);
    }

    void another_prepare() override {
        m_end_cycles.resize(m_config.m_attempts_count);
    }

    void one_work() noexcept override;
    void another_work() noexcept override;
    void report(std::ostream& os) override;
};

/*
 * The same as previous but writing data and getting CPU tsc is made in one asm block
 */
class one_side_asm_test : public one_side_test {
    void one_work() noexcept override;
    void another_work() noexcept override;
};

/*
 * The same as previous but waiting cycle doesn't depend on expected data state.
 * I expect it will allow branch predictor to work more smoothly executing waiting cycle.
 */
class one_side_asm_relax_branch_pred_test : public one_side_asm_test {
    static constexpr std::size_t s_samples_size = 10000;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> m_samples;

    void another_prepare() override {
        one_side_asm_test::another_prepare();
        m_samples.resize(s_samples_size);
    }
    void another_work() noexcept override;
};

/*
 * The test increments data many times in two threads sequentially and measures duration
 * of the whole operation. Results could show faster data exchange between caches comparing with
 * other tests. This may be caused by the fact that there are no additional instructions in the test
 * code which execute next step after getting expected data - the only instruction which waits
 * for the data and makes a next step - is compare_exchange.
 */
class ping_pong_test : public test_case_iface {
    static constexpr std::uint32_t s_ping_pongs = 100;

    config m_config;
    std::vector<std::uint64_t> m_cycles;

    void set_config(const config& cfg) override { m_config = cfg; }
    void one_prepare() override { m_cycles.resize(m_config.m_attempts_count); }
    void another_prepare() override {};
    void one_work() noexcept override;
    void another_work() noexcept override;
    void report(std::ostream& os) override;
};

