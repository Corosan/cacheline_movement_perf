// vim: textwidth=100
#include "tests.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <locale>
#include <iostream>
#include <sstream>
#include <fstream>
#include <atomic>
#include <algorithm>
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
 *   2. Switch off CPU frequency scaling, turn on maximum CPU frequency for all CPU cores else
 *      timing in nanoseconds will be incorrect.
 *
 * Notes:
 *   * The implementation uses posix calls so it can't compile on OSs other than posix-based like
 *     Linux, FreeBSD, Solaris. Though there small such cases which should be simply isolate and
 *     make this code cross-compiled.
 *   * There are a number of experiments (or test modes) here, each of them gives different results
 *     unfortunately.
 *   * Initial config / command line parsing implemented using high-level c++ std library features
 *     so it's far from efficiency. But it doesn't matter as it doesn't influence on tests
 *     execution.
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

    static double get_cpu_freq_ghz();
public:
    explicit test_runner(unsigned short t1_cpuid, unsigned short t2_cpuid)
        : m_cpuids{t1_cpuid, t2_cpuid}, m_start_barrier{2} {
    }

    // Run two threads, bind them to specified CPU cores and execute the test case on them
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

        if (res == 0) {
            std::cout << "Test case result:" << std::endl;
            test_case->report(std::cout, get_cpu_freq_ghz());
            std::cout << std::endl;
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

/*
 * Tries to get CPU frequency from the first CPU description block available at /proc/cpuinfo. The
 * method is useless on systems with a few CPU cores working on possibly different frequencies or if
 * a frequency can change over time.
 */
double test_runner::get_cpu_freq_ghz() {
    using namespace std::string_view_literals;

    std::ifstream ifs{"/proc/cpuinfo"};
    std::string line;
    double res = 0.0;

    while (getline(ifs, line)) {
        transform(line.begin(), line.end(), line.begin(), [](auto c){ return std::tolower(c, std::locale()); });
        if (auto p = line.find("cpu mhz"sv); p != std::string::npos) {
            if (p = line.find(":"sv, p); p != std::string::npos) {
                std::istringstream is{line.substr(p + 1)};
                is >> res;
                if (is.bad() || is.fail())
                    res = 0.0;
                else
                    res /= 1000.0;
            }
            break;
        }
    }

    return res;
}

int usage(const char* basename) {
    if (auto p = std::strrchr(basename, '/'))
        basename = p + 1;

    std::cout << "Usage: " << basename << " [OPTIONS]\n"
        "\n"
        "The program tries to calculate how long to transfer a CPU cache line\n"
        "from one CPU core to another. It can do this by using different approaches\n"
        "which could provide slightly different results. The measurement is reported\n"
        "in number of CPU cycles (using rdtsc instruction) and in nanoseconds,\n"
        "which is calculated from cycles based on system info about CPU clock\n"
        "frequency.\n"
        "\n"
        "Options:\n"
        "  --t1-cpuid N - CPU ID of a CPU core a worker 1 should be bound to\n"
        "  --t2-cpuid N - CPU ID of a CPU core a worker 2 should be bound to\n"
        "  --attempts N - number of attempts for the test (default: 1000)\n"
        "  --mode N - test mode [0-3] (default: 0)" << std::endl;
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
            else if ("3"sv == argv[i + 1])
                test_case = std::make_unique<one_side_asm_relax_branch_pred_test>();
            else {
                std::cerr << "unknown test mode value"sv << std::endl;
                return 1;
            }
            ++i;
        } else {
            std::cerr << "unknown option \""sv << argv[i] << "\" or there is no mandatory argument"sv << std::endl;
            return 1;
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
