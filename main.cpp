#include "tests.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <atomic>
#include <exception>
#include <memory>
#include <thread>
#include <string_view>
#include <system_error>

#include <pthread.h>

/*
 * Preconditions which a system this test is run on should meet:
 *   1. Isolated CPU cores dedicated for the test (throw out OS, other processes, IRQs, kernel
 *      deferred tasks, timers, etc.)
 *   2. Switch off CPU frequency scaling, turn on maximum CPU frequency
 *
 * Notes:
 *   * The implementation uses posix calls so it can't compile on OSs other than posix-based like
 *     Linux, FreeBSD, Solaris.
 *   * There are a number of experiments (or test modes) here, each of them gives different results
 *     unfortunately.
 */

// like std::latch, but without going into kernel space
class spin_latch {
    std::atomic<std::ptrdiff_t> m_counter;
public:
    explicit spin_latch(std::ptrdiff_t expected) : m_counter(expected) {}
    spin_latch(const spin_latch&) = delete;
    void arrive_and_wait(std::ptrdiff_t n = 1) noexcept {
        if (m_counter.fetch_sub(n, std::memory_order_relaxed) == n)
            return;
        while (m_counter.load(std::memory_order_relaxed) != 0)
            ;
    }
};

class test_runner {
    const unsigned short m_cpuids[2];
    std::exception_ptr m_errors[2];
    spin_latch m_start_barrier;
public:
    explicit test_runner(unsigned short t1_cpuid, unsigned short t2_cpuid)
        : m_cpuids{t1_cpuid, t2_cpuid}, m_start_barrier{2} {
    }

    // Run two threads, bind them to specified CPU cores and execute test case on them
    int run(std::unique_ptr<test_case_iface> test_case) {
        int res = 0;
        std::thread t1{[this, &test_case](){
            try {
                set_thread_affinity(m_cpuids[0]);
                test_case->one_prepare();
            } catch (...) {
                m_errors[0] = std::current_exception();
            }

            m_start_barrier.arrive_and_wait();
            if (m_errors[1])
                return;

            test_case->one_work();
        }};
        std::thread t2{[this, &test_case](){
            try {
                set_thread_affinity(m_cpuids[1]);
                test_case->another_prepare();
            } catch (...) {
                m_errors[1] = std::current_exception();
            }

            m_start_barrier.arrive_and_wait();
            if (m_errors[0])
                return;

            test_case->another_work();
        }};

        t1.join();
        t2.join();

        std::cout << "Test case result:" << std::endl;
        test_case->report(std::cout);
        std::cout << std::endl;

        unsigned short worker_idx = 1;
        for (auto& exc_ptr : m_errors) {
            if (exc_ptr)
                try {
                    res = 1;
                    std::rethrow_exception(exc_ptr);
                } catch (const std::exception& e) {
                    std::cerr << "unexpected exception at worker " << worker_idx
                        << ": " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "unexpected error at worker " << worker_idx << std::endl;
                }
            ++worker_idx;
        }

        return res;
    }
private:
    static void set_thread_affinity(unsigned short cpuid) {
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(cpuid, &cpu_set);
        if (auto res = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set); res != 0)
            throw std::system_error{std::make_error_code((std::errc)res), "unable to set thread affinity"};
    }
};

int usage(const char* basename) {
    if (auto p = std::strrchr(basename, '/'))
        basename = p + 1;

    std::cout << "Usage: " << basename << " [OPTIONS] [TEST MODE OPTIONS]\n\n"
        "Options:\n"
        "  --t1-cpuid N - CPU ID of a CPU core a worker 1 should be bound to\n"
        "  --t2-cpuid N - CPU ID of a CPU core a worker 2 should be bound to\n"
        "  --attempts N - number of attempts for the test (default: 1000)\n"
        "  --mode N - test mode [0-2].\n\n"
        "Various test mode options can be specified only after providing --mode option\n"
        "on the command line.\n\n";

    std::cout << "Test mode 0 options:\n";
    one_side_test::usage(std::cout);
    std::cout << "\nTest mode 1 options:\n";
    one_side_asm_test::usage(std::cout);
    std::cout << "\nTest mode 2 options:\n";
    ping_pong_test::usage(std::cout);

    std::cout << std::endl;
    return 0;
}

int main(int argc, const char* argv[]) {
    using namespace std::string_view_literals;

    short cpuids[2]{-1, -1};
    test_case_iface::config test_case_cfg;
    std::unique_ptr<test_case_iface> test_case;

    if (argc == 1)
        return usage(argv[0]);

    for (int i = 1; i < argc; ++i) {
        if ("--help"sv == argv[i])
            return usage(argv[0]);
        else if ("--attempts"sv == argv[i] && i + 1 < argc) {
            std::istringstream is{argv[++i]};
            is >> test_case_cfg.m_attempts_count;
            if (is.fail() || is.bad() || ! is.eof()) {
                std::cerr << "unable to convert attempts argument into an acceptable number"sv << std::endl;
                return 1;
            }
        }
        else if ("--t1-cpuid"sv == argv[i] && i + 1 < argc) {
            std::istringstream is{argv[++i]};
            unsigned short v;
            is >> v;
            if (is.fail() || is.bad() || ! is.eof()) {
                std::cerr << "unable to convert t1 cpuid into an acceptable number"sv << std::endl;
                return 1;
            }
            cpuids[0] = static_cast<short>(v);
        }
        else if ("--t2-cpuid"sv == argv[i] && i + 1 < argc) {
            std::istringstream is{argv[++i]};
            unsigned short v;
            is >> v;
            if (is.fail() || is.bad() || ! is.eof()) {
                std::cerr << "unable to convert t2 cpuid into an acceptable number"sv << std::endl;
                return 1;
            }
            cpuids[1] = static_cast<short>(v);
        }
        else if ("--mode"sv == argv[i] && i + 1 < argc) {
            if ("0"sv == argv[i + 1])
                test_case = std::make_unique<one_side_test>();
            else if ("1"sv == argv[i + 1])
                test_case = std::make_unique<one_side_asm_test>();
            else if ("2"sv == argv[i + 1])
                test_case = std::make_unique<ping_pong_test>();
            else {
                std::cerr << "unknown test mode value"sv << std::endl;
                return 1;
            }
            ++i;
        } else {
            const auto parse_res = (test_case)
                ? test_case->process_args(std::cerr, argc, argv, i)
                : test_case_iface::parse_args::unknown;
            switch (parse_res) {
            case test_case_iface::parse_args::unknown:
                std::cerr << "unknown option \""sv << argv[i] << "\" or there is no mandatory argument" << std::endl;
                return 1;
            case test_case_iface::parse_args::invalid:
                return 1;
            default:
                break;
            }
        }
    }

    if (cpuids[0] == -1 || cpuids[1] == -1) {
        std::cerr << "some of cpu ids wasn't provided"sv << std::endl;
        return 1;
    }

    if (! test_case)
        test_case = std::make_unique<one_side_test>();

    test_case->set_config(std::move(test_case_cfg));
    return test_runner(cpuids[0], cpuids[1]).run(std::move(test_case));
}
