#pragma once

#ifndef MALLOC
#define MALLOC 2
#endif

#include <cpuid.h>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstring>

#include <string>
#include <iostream>
#include <iomanip>
#include <sstream>

#include <pthread.h>
#if defined(__APPLE__) || MALLOC == 0
#elif MALLOC == 1
#include <jemalloc/jemalloc.h>
#elif MALLOC == 2
#include "rpmalloc/rpmalloc.h"
#endif

static constexpr uint32_t level_bstr  = 0x80000004;

enum class Reg : int {eax = 0, ebx, ecx, edx, size};

class CpuidQuery {
public:
    static constexpr uint32_t query_level = 0;
    static constexpr uint32_t result_bit = 0;
    static constexpr Reg result_reg = Reg::eax;
};

class TscQuery : public CpuidQuery {
public:
    static constexpr uint32_t query_level = 0x80000001;
    static constexpr uint32_t result_bit = (1 << 27);
    static constexpr Reg result_reg = Reg::edx;
};

class IvTscQuery : public CpuidQuery {
public:
    static constexpr uint32_t query_level = 0x80000007;
    static constexpr uint32_t result_bit = (1 << 8);
    static constexpr Reg result_reg = Reg::edx;
};

template <typename Query>
inline bool cpu_has_feature() {
    uint32_t max_level = __get_cpuid_max(0x80000000, nullptr);
    if (max_level < Query::query_level) {
        return false;
    } else {
        uint32_t regs[static_cast<int>(Reg::size)];
        int r = __get_cpuid(Query::query_level,
                            &regs[static_cast<int>(Reg::eax)],
                            &regs[static_cast<int>(Reg::ebx)],
                            &regs[static_cast<int>(Reg::ecx)],
                            &regs[static_cast<int>(Reg::edx)]);
        if (r == 0)
            return false;
        else
            return (regs[static_cast<int>(Query::result_reg)] & Query::result_bit);
    }
}

inline std::string get_cpu_brand_string() {
    uint32_t max_level = __get_cpuid_max(0x80000000, nullptr);
    if (max_level < level_bstr)
        return std::string(); // brand string not supported

    uint32_t regs[static_cast<int>(Reg::size)];
    std::string bs;
    for (unsigned int i = 0; i < 3; ++i) {
        int r = __get_cpuid(0x80000002 + i,
                            &regs[static_cast<int>(Reg::eax)],
                            &regs[static_cast<int>(Reg::ebx)],
                            &regs[static_cast<int>(Reg::ecx)],
                            &regs[static_cast<int>(Reg::edx)]);
        (void)r;
        for (int j = 0; j < static_cast<int>(Reg::size); ++j) {
            for (int k = 0; k < (int)sizeof(uint32_t); ++k) {
                auto c = reinterpret_cast<char *>(&regs[j])[k];
                if (c != 0x00)
                    bs.push_back(c);
            }
        }
    }
    return bs;
}

inline double get_cpu_brand_frequency() {
    static constexpr double multipliers[] = {0.001, 1.0, 1000.0};

    std::string bs = get_cpu_brand_string();
    std::cout << "Info: CPU detected as " << bs << std::endl;

    int unit = 0;
    std::string::size_type freq_str_end = bs.rfind("MHz");
    if (freq_str_end == std::string::npos) {
        ++unit;
        freq_str_end = bs.rfind("GHz");
    }
    if (freq_str_end == std::string::npos) {
        ++unit;
        freq_str_end = bs.find("THz");
    }
    if (freq_str_end == std::string::npos)
        return 0.0;

    auto freq_str_begin = freq_str_end;
    char c;
    do {
        --freq_str_begin;
        c = bs[freq_str_begin];
    } while (isdigit(c) || (c == '.'));
    ++freq_str_begin;
    auto len = freq_str_end - freq_str_begin;
    auto fs = bs.substr(freq_str_begin, len);
    std::stringstream ss(fs);
    double freq;
    ss >> freq;

    return freq * multipliers[unit];
}

inline double determine_cpu_freq() {
#if defined(__APPLE__) || MALLOC == 0
#elif MALLOC == 1
    char const* val = nullptr;
    size_t len = sizeof(val);
    int r;
    r = mallctl("opt.metadata_thp", &val, &len, NULL, 0);
    if (r == 0)
        std::cout << "jemalloc metadata THP: " << std::string(val) << std::endl;

    len = sizeof(val);
    r = mallctl("opt.thp", &val, &len, NULL, 0);
    if (r == 0)
        std::cout << "jemalloc THP: " << std::string(val) << std::endl;
#elif MALLOC == 2
    rpmalloc_config_t config;
    memset(&config, 0, sizeof(config));
    config.page_size = 2048 * 1024;
    config.enable_huge_pages = 1;
    rpmalloc_initialize_config(&config);
#endif

    double freq = 0.0;
    std::cout << "Checking for rdtscp support..." << std::flush;
    if (!cpu_has_feature<TscQuery>()) {
        std::cout << std::endl;
        std::cerr << "Fatal error: CPU lacks timestamp counter (tsc) capability." << std::endl;
        return freq;
    } else {
        std::cout << " Yes" << std::endl;
    }

    std::cout << "Checking for invariant tsc support..." << std::flush;
    if (!cpu_has_feature<IvTscQuery>()) {
        std::cout << std::endl;
        std::cout << "Warning: CPU does not report support for invariant tsc. Please double check timing measurement."
                  << std::endl;
    } else {
        std::cout << " Yes" << std::endl;
    }

    std::cout << "Determining processor frequency..." << std::endl;
    freq = get_cpu_brand_frequency();
    if (freq == 0.0) {
        std::cout << "Warning: Can't determine processor tsc frequency from CPU brand string. Using the default value "
                "of 1 GHz." << std::endl;
        freq = 1.0;
    }
    std::cout << "Info: CPU tsc frequency determined as "
              << std::fixed << std::setprecision(2) << freq << " GHz." << std::endl;
    return freq;
}

inline void set_affinity(int runner_id) {
#if defined(__APPLE__)
    always_assert(false, "macOS not supported to run -- compile only.");
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    // FIXME: This is only valid for the GATECH machine
    //int cpu_id = 24 * (runner_id % 8) + (runner_id / 8);
    // This is for AWS m4.16xlarge instances (64 threads, 32 cores, 2 sockets)
    //int cpu_id = runner_id / 2 + 16 * (runner_id % 2) + (runner_id / 32) * 16;
    int cpu_id = runner_id;
    CPU_SET(cpu_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        abort();
    }
#endif
}
