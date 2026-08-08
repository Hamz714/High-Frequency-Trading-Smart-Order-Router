#include "common/HostInfo.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "common/CycleClock.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__linux__)
    #include <sched.h>
#endif

#if defined(__APPLE__)
    #include <sys/sysctl.h>
#endif

#if HFT_CYCLE_CLOCK_TSC && !defined(_MSC_VER)
    #include <cpuid.h>
#endif

namespace {

bool cpuid_leaf(unsigned leaf, unsigned out[4]) {
#if !HFT_CYCLE_CLOCK_TSC
    (void)leaf;
    (void)out;
    return false;
#elif defined(_MSC_VER)
    int regs[4];
    unsigned base = (leaf & 0x80000000u) ? 0x80000000u : 0u;
    __cpuid(regs, static_cast<int>(base));
    if (static_cast<unsigned>(regs[0]) < leaf) return false;
    __cpuid(regs, static_cast<int>(leaf));
    for (int i = 0; i < 4; ++i) out[i] = static_cast<unsigned>(regs[i]);
    return true;
#else
    return __get_cpuid(leaf, &out[0], &out[1], &out[2], &out[3]) != 0;
#endif
}

std::string trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

#if defined(__linux__)
std::string read_first_line(const char* path) {
    std::ifstream in(path);
    if (!in) return "";
    std::string line;
    std::getline(in, line);
    return trim(line);
}
#endif

std::string detect_cpu_brand() {
    unsigned regs[4];
    if (cpuid_leaf(0x80000004u, regs)) {
        char brand[49] = {};
        for (unsigned leaf = 0x80000002u; leaf <= 0x80000004u; ++leaf) {
            if (!cpuid_leaf(leaf, regs)) break;
            std::memcpy(brand + static_cast<size_t>(leaf - 0x80000002u) * 16, regs, 16);
        }
        std::string result = trim(brand);
        if (!result.empty()) return result;
    }

#if defined(__APPLE__)
    char buffer[256] = {};
    size_t size = sizeof(buffer);
    if (sysctlbyname("machdep.cpu.brand_string", buffer, &size, nullptr, 0) == 0) {
        return trim(buffer);
    }
#endif

#if defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        if (key == "model name" || key == "Model") {
            return trim(line.substr(colon + 1));
        }
    }
#endif

    return "unknown";
}

std::string detect_os() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    std::ifstream version("/proc/version");
    std::string text;
    std::getline(version, text);
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered.find("microsoft") != std::string::npos) return "Linux (WSL)";
    return "Linux";
#else
    return "unknown";
#endif
}

double probe_cycle_delta_ns() {
    constexpr int SAMPLES = 4096;
    double best = 0.0;
    for (int i = 0; i < SAMPLES; ++i) {
        uint64_t a = cycle_now();
        uint64_t b = cycle_now();
        if (b > a) {
            double ns = cycles_to_ns(b - a);
            if (best == 0.0 || ns < best) best = ns;
        }
    }
    return best;
}

double probe_steady_clock_delta_ns() {
    constexpr int SAMPLES = 4096;
    double best = 0.0;
    for (int i = 0; i < SAMPLES; ++i) {
        auto a = std::chrono::steady_clock::now();
        auto b = std::chrono::steady_clock::now();
        double ns = std::chrono::duration<double, std::nano>(b - a).count();
        if (ns > 0.0 && (best == 0.0 || ns < best)) best = ns;
    }
    return best;
}

std::string detect_governor() {
#if defined(__linux__)
    std::string governor = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    return governor.empty() ? "unavailable" : governor;
#else
    return "n/a";
#endif
}

std::string detect_turbo() {
#if defined(__linux__)
    std::string no_turbo = read_first_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
    if (!no_turbo.empty()) return no_turbo == "0" ? "on" : "off";

    std::string boost = read_first_line("/sys/devices/system/cpu/cpufreq/boost");
    if (!boost.empty()) return boost == "1" ? "on" : "off";

    return "unavailable";
#else
    return "unknown";
#endif
}

}  // namespace

HostInfo collect_host_info() {
    HostInfo host;
    host.os = detect_os();
    host.cpu_brand = detect_cpu_brand();
    host.timer_backend = cycle_clock_backend();
    host.cycles_per_ns = cycles_per_ns();
    host.timer_resolution_ns = probe_cycle_delta_ns();
    host.steady_clock_resolution_ns = probe_steady_clock_delta_ns();
    host.logical_cpus = std::thread::hardware_concurrency();
    host.scaling_governor = detect_governor();
    host.turbo = detect_turbo();

    unsigned regs[4];
    if (cpuid_leaf(0x80000007u, regs)) {
        host.tsc_invariant = (regs[3] & (1u << 8)) != 0;
    }
    if (cpuid_leaf(1u, regs)) {
        host.hypervisor = (regs[2] & (1u << 31)) != 0;
    }

    return host;
}

bool pin_current_thread(int cpu) {
    if (cpu < 0) return false;

#if defined(_WIN32)
    if (cpu >= 64) return false;
    DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    return false;
#endif
}

std::string describe_host(const HostInfo& host) {
    std::ostringstream out;
    out << "os=" << host.os
        << "; cpu=" << host.cpu_brand
        << "; logical_cpus=" << host.logical_cpus
        << "; pinned_cpu=" << (host.pinned_cpu >= 0 ? std::to_string(host.pinned_cpu) : std::string("none"))
        << "; timer=" << host.timer_backend;

    if (host.timer_backend == std::string("rdtsc")) {
        out << "; tsc_invariant=" << (host.tsc_invariant ? "yes" : "no")
            << "; tsc_ghz=" << host.cycles_per_ns;
    }

    out << "; min_timer_delta_ns=" << host.timer_resolution_ns
        << "; steady_clock_delta_ns=" << host.steady_clock_resolution_ns
        << "; governor=" << host.scaling_governor
        << "; turbo=" << host.turbo
        << "; hypervisor=" << (host.hypervisor ? "yes" : "no");

    return out.str();
}
