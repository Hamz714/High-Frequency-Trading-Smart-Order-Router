#pragma once

#include <string>

struct HostInfo {
    std::string os;
    std::string cpu_brand;
    std::string timer_backend;
    std::string scaling_governor = "n/a";
    std::string turbo = "unknown";
    double cycles_per_ns = 0.0;
    double timer_resolution_ns = 0.0;
    double steady_clock_resolution_ns = 0.0;
    bool tsc_invariant = false;
    bool hypervisor = false;
    unsigned logical_cpus = 0;
    int pinned_cpu = -1;
};

HostInfo collect_host_info();

bool pin_current_thread(int cpu);

std::string describe_host(const HostInfo& host);
