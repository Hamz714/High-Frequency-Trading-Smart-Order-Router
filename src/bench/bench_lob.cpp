#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/CycleClock.h"
#include "common/HostInfo.h"
#include "common/Types.h"
#include "lob/LimitOrderBook.h"

namespace {

constexpr int64_t TOUCH_BID = 10'000'000;
constexpr int64_t TOUCH_ASK = 10'000'100;
constexpr int64_t RESTING_QTY = 100;

constexpr int WARMUP_OPS = 2'000;
constexpr int MEASURED_OPS = 50'000;

constexpr int DEFAULT_REPEATS = 9;

constexpr size_t BENCH_POOL_CAPACITY = WARMUP_OPS + MEASURED_OPS + 16;

struct BenchResult {
    std::string name;
    int64_t ops = 0;
    double total_seconds = 0.0;
    double p50_ns = 0.0, p95_ns = 0.0, p99_ns = 0.0, p999_ns = 0.0, max_ns = 0.0;

    double ops_per_sec() const {
        return total_seconds > 0.0 ? static_cast<double>(ops) / total_seconds : 0.0;
    }
};

struct AggregatedResult {
    std::string name;
    int runs = 0;
    double median_ops_per_sec = 0.0, min_ops_per_sec = 0.0, max_ops_per_sec = 0.0;
    double p50_ns = 0.0, p95_ns = 0.0, p99_ns = 0.0, p999_ns = 0.0, max_ns = 0.0;
    double adj_p50_ns = 0.0, adj_p95_ns = 0.0, adj_p99_ns = 0.0, adj_p999_ns = 0.0, adj_max_ns = 0.0;
};

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(values.size() - 1));
    return values[idx];
}

BenchResult summarize(const std::string& name, const std::vector<double>& latencies_ns, double total_seconds) {
    BenchResult r;
    r.name = name;
    r.ops = static_cast<int64_t>(latencies_ns.size());
    r.total_seconds = total_seconds;
    r.p50_ns = percentile(latencies_ns, 0.50);
    r.p95_ns = percentile(latencies_ns, 0.95);
    r.p99_ns = percentile(latencies_ns, 0.99);
    r.p999_ns = percentile(latencies_ns, 0.999);
    r.max_ns = latencies_ns.empty() ? 0.0 : *std::max_element(latencies_ns.begin(), latencies_ns.end());
    return r;
}

double elapsed_seconds(uint64_t start_ticks) {
    return cycles_to_ns(cycle_now() - start_ticks) / 1e9;
}

void seed_touch(LimitOrderBook& book) {
    book.submit(Side::BUY, OrderType::LIMIT, TOUCH_BID, RESTING_QTY);
    book.submit(Side::SELL, OrderType::LIMIT, TOUCH_ASK, RESTING_QTY);
}

// Stays within ~100 ticks of the touch - comfortably inside the 256-wide
// ladder window, so these orders live in the fixed array, not the map.
int64_t shallow_offset(int i) { return 1 + (i % 90); }

// Far enough from the touch to land in the std::map overflow region and
// never become best-of-book itself, so no ladder-window shift is triggered.
int64_t deep_offset(int i) { return 5000 + (i % 2000); }

std::vector<OrderID> seed_liquidity(LimitOrderBook& book, int count, bool shallow) {
    std::vector<OrderID> ids;
    ids.reserve(count);
    for (int i = 0; i < count; ++i) {
        int64_t price = TOUCH_BID - (shallow ? shallow_offset(i) : deep_offset(i));
        auto fills = book.submit(Side::BUY, OrderType::LIMIT, price, RESTING_QTY);
        ids.push_back(fills.back().order_id);
    }
    return ids;
}

// Two back-to-back timer reads with no work between them: the additive cost the
// instrumented loops pay per sample, subtracted from every reported percentile.
BenchResult run_timer_floor_benchmark() {
    std::vector<double> latencies_ns;
    latencies_ns.reserve(MEASURED_OPS);

    uint64_t bench_start = cycle_now();
    for (int i = 0; i < MEASURED_OPS; ++i) {
        uint64_t t0 = cycle_now();
        uint64_t t1 = cycle_now();
        latencies_ns.push_back(cycles_to_ns(t1 - t0));
    }
    double total_seconds = elapsed_seconds(bench_start);

    return summarize("timer noise floor (no work between reads)", latencies_ns, total_seconds);
}

BenchResult run_insert_benchmark(const std::string& name, bool shallow, bool instrument) {
    LimitOrderBook book(BENCH_POOL_CAPACITY);
    seed_touch(book);
    seed_liquidity(book, WARMUP_OPS, shallow);  // warm-up, discarded

    std::vector<double> latencies_ns;
    if (instrument) latencies_ns.reserve(MEASURED_OPS);

    uint64_t bench_start = cycle_now();
    if (instrument) {
        for (int i = 0; i < MEASURED_OPS; ++i) {
            int64_t price = TOUCH_BID - (shallow ? shallow_offset(i) : deep_offset(i));

            uint64_t t0 = cycle_now();
            book.submit(Side::BUY, OrderType::LIMIT, price, RESTING_QTY);
            uint64_t t1 = cycle_now();

            latencies_ns.push_back(cycles_to_ns(t1 - t0));
        }
    } else {
        for (int i = 0; i < MEASURED_OPS; ++i) {
            int64_t price = TOUCH_BID - (shallow ? shallow_offset(i) : deep_offset(i));
            book.submit(Side::BUY, OrderType::LIMIT, price, RESTING_QTY);
        }
    }
    double total_seconds = elapsed_seconds(bench_start);

    BenchResult r = summarize(name, latencies_ns, total_seconds);
    if (!instrument) r.ops = MEASURED_OPS;
    return r;
}

BenchResult run_cancel_benchmark(const std::string& name, bool shallow, bool instrument) {
    LimitOrderBook book(BENCH_POOL_CAPACITY);
    seed_touch(book);

    int total_needed = WARMUP_OPS + MEASURED_OPS;
    std::vector<OrderID> ids = seed_liquidity(book, total_needed, shallow);

    for (int i = 0; i < WARMUP_OPS; ++i) {
        book.cancel(ids[i]);  // warm-up, discarded
    }

    std::vector<double> latencies_ns;
    if (instrument) latencies_ns.reserve(MEASURED_OPS);

    uint64_t bench_start = cycle_now();
    if (instrument) {
        for (int i = WARMUP_OPS; i < total_needed; ++i) {
            uint64_t t0 = cycle_now();
            book.cancel(ids[i]);
            uint64_t t1 = cycle_now();

            latencies_ns.push_back(cycles_to_ns(t1 - t0));
        }
    } else {
        for (int i = WARMUP_OPS; i < total_needed; ++i) {
            book.cancel(ids[i]);
        }
    }
    double total_seconds = elapsed_seconds(bench_start);

    BenchResult r = summarize(name, latencies_ns, total_seconds);
    if (!instrument) r.ops = MEASURED_OPS;
    return r;
}

// Repeatedly crosses the spread with a small marketable IOC against a single
// deep resting order at the touch - isolates the matching hot path (walk to
// best price level, partial-fill the head order) from level-eviction and
// ladder-window-shift costs.
BenchResult run_match_benchmark(const std::string& name, bool /*shallow*/, bool instrument) {
    LimitOrderBook book(BENCH_POOL_CAPACITY);
    constexpr int64_t DEEP_RESTING_QTY = 10'000'000;
    constexpr int64_t MATCH_QTY = 10;

    book.submit(Side::SELL, OrderType::LIMIT, TOUCH_ASK, DEEP_RESTING_QTY);

    for (int i = 0; i < WARMUP_OPS; ++i) {
        book.submit(Side::BUY, OrderType::IOC, TOUCH_ASK, MATCH_QTY);  // warm-up, discarded
    }

    std::vector<double> latencies_ns;
    if (instrument) latencies_ns.reserve(MEASURED_OPS);

    uint64_t bench_start = cycle_now();
    if (instrument) {
        for (int i = 0; i < MEASURED_OPS; ++i) {
            uint64_t t0 = cycle_now();
            book.submit(Side::BUY, OrderType::IOC, TOUCH_ASK, MATCH_QTY);
            uint64_t t1 = cycle_now();

            latencies_ns.push_back(cycles_to_ns(t1 - t0));
        }
    } else {
        for (int i = 0; i < MEASURED_OPS; ++i) {
            book.submit(Side::BUY, OrderType::IOC, TOUCH_ASK, MATCH_QTY);
        }
    }
    double total_seconds = elapsed_seconds(bench_start);

    BenchResult r = summarize(name, latencies_ns, total_seconds);
    if (!instrument) r.ops = MEASURED_OPS;
    return r;
}

using ScenarioFn = BenchResult (*)(const std::string&, bool, bool);

// Percentiles come from the instrumented pass; throughput comes from a second,
// uninstrumented pass so the per-op timer reads stay out of the denominator.
BenchResult run_scenario(ScenarioFn fn, const std::string& name, bool shallow) {
    BenchResult timed = fn(name, shallow, true);
    BenchResult untimed = fn(name, shallow, false);
    timed.total_seconds = untimed.total_seconds;
    timed.ops = untimed.ops;
    return timed;
}

double median_of(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    if (values.size() % 2 != 0) return values[mid];
    return (values[mid - 1] + values[mid]) / 2.0;
}

AggregatedResult aggregate(const std::vector<BenchResult>& runs, double floor_ns) {
    AggregatedResult agg;
    if (runs.empty()) return agg;

    agg.name = runs.front().name;
    agg.runs = static_cast<int>(runs.size());

    std::vector<double> throughputs;
    std::vector<double> p50s, p95s, p99s, p999s, maxes;
    throughputs.reserve(runs.size());

    for (const auto& r : runs) {
        throughputs.push_back(r.ops_per_sec());
        p50s.push_back(r.p50_ns);
        p95s.push_back(r.p95_ns);
        p99s.push_back(r.p99_ns);
        p999s.push_back(r.p999_ns);
        maxes.push_back(r.max_ns);
    }

    agg.median_ops_per_sec = median_of(throughputs);
    agg.min_ops_per_sec = *std::min_element(throughputs.begin(), throughputs.end());
    agg.max_ops_per_sec = *std::max_element(throughputs.begin(), throughputs.end());
    agg.p50_ns = median_of(p50s);
    agg.p95_ns = median_of(p95s);
    agg.p99_ns = median_of(p99s);
    agg.p999_ns = median_of(p999s);
    agg.max_ns = *std::max_element(maxes.begin(), maxes.end());

    auto adjust = [floor_ns](double raw) { return std::max(0.0, raw - floor_ns); };
    agg.adj_p50_ns = adjust(agg.p50_ns);
    agg.adj_p95_ns = adjust(agg.p95_ns);
    agg.adj_p99_ns = adjust(agg.p99_ns);
    agg.adj_p999_ns = adjust(agg.p999_ns);
    agg.adj_max_ns = adjust(agg.max_ns);

    return agg;
}

void print_host_banner(const HostInfo& host) {
    std::cout << "\n=== Measurement environment ===\n\n";
    std::cout << "  os                    " << host.os << "\n";
    std::cout << "  cpu                   " << host.cpu_brand << "\n";
    std::cout << "  logical cpus          " << host.logical_cpus << "\n";
    std::cout << "  pinned to cpu         "
              << (host.pinned_cpu >= 0 ? std::to_string(host.pinned_cpu)
                                       : std::string("not pinned")) << "\n";
    std::cout << "  timer backend         " << host.timer_backend;
    if (host.timer_backend == "rdtsc") {
        std::cout << " (" << std::fixed << std::setprecision(3) << host.cycles_per_ns << " GHz, "
                  << (host.tsc_invariant ? "invariant" : "NOT invariant") << ")";
    }
    std::cout << "\n";
    std::cout << "  min timer delta       " << std::fixed << std::setprecision(2)
              << host.timer_resolution_ns << " ns\n";
    std::cout << "  steady_clock delta    " << std::fixed << std::setprecision(2)
              << host.steady_clock_resolution_ns << " ns\n";
    std::cout << "  scaling governor      " << host.scaling_governor << "\n";
    std::cout << "  turbo                 " << host.turbo << "\n";
    std::cout << "  hypervisor            " << (host.hypervisor ? "yes (virtualised host)" : "no") << "\n";

    if (!host.tsc_invariant && host.timer_backend == "rdtsc") {
        std::cout << "\n  WARNING: TSC is not invariant on this host - cycle counts may drift with\n"
                     "  frequency or across cores. Treat latencies as indicative only.\n";
    }
    if (host.pinned_cpu < 0) {
        std::cout << "\n  WARNING: thread is not pinned - migrations between cores will show up in\n"
                     "  the tail percentiles.\n";
    }
    if (host.hypervisor) {
        std::cout << "\n  NOTE: running under a hypervisor (VM, WSL2, or cloud instance). Resolution\n"
                     "  is real, but core isolation and frequency policy are not under your control.\n";
    }
}

void print_floor_summary(const AggregatedResult& floor) {
    std::cout << "\n=== Timer noise floor ===\n\n";
    std::cout << "  Two back-to-back timer reads, no work between them, "
              << MEASURED_OPS << " samples x " << floor.runs << " runs.\n";
    std::cout << "  p50 " << std::fixed << std::setprecision(2) << floor.p50_ns << " ns"
              << "   p95 " << floor.p95_ns << " ns"
              << "   p99 " << floor.p99_ns << " ns"
              << "   p999 " << floor.p999_ns << " ns"
              << "   max " << floor.max_ns << " ns\n";
    std::cout << "  The p50 (" << floor.p50_ns
              << " ns) is subtracted from every percentile in the table below.\n";
}

void print_console_table(const std::vector<AggregatedResult>& results, int repeats, double floor_ns) {
    std::cout << "\n=== LimitOrderBook Microbenchmark (single-threaded, "
              << MEASURED_OPS << " measured ops/scenario x " << repeats << " runs) ===\n\n";
    std::cout << "Throughput is the median across runs of an uninstrumented pass, with the\n"
              << "observed range in brackets - no per-op timer reads are inside that loop.\n"
              << "Percentiles are medians across runs of a separate instrumented pass, with the\n"
              << "measured " << std::fixed << std::setprecision(2) << floor_ns
              << " ns timer floor subtracted. Raw (unsubtracted) values are in the CSV.\n\n";

    std::cout << std::left << std::setw(46) << "Scenario"
              << std::right << std::setw(14) << "ops/sec"
              << std::setw(26) << "range"
              << std::setw(10) << "p50(ns)"
              << std::setw(10) << "p95(ns)"
              << std::setw(10) << "p99(ns)"
              << std::setw(11) << "p999(ns)"
              << std::setw(11) << "max(ns)" << "\n";
    std::cout << std::string(138, '-') << "\n";

    for (const auto& r : results) {
        std::ostringstream range;
        range << std::fixed << std::setprecision(2)
              << "[" << r.min_ops_per_sec / 1e6 << "M - " << r.max_ops_per_sec / 1e6 << "M]";

        std::cout << std::left << std::setw(46) << r.name
                  << std::right << std::setw(14) << std::fixed << std::setprecision(0) << r.median_ops_per_sec
                  << std::setw(26) << range.str()
                  << std::fixed << std::setprecision(1)
                  << std::setw(10) << r.adj_p50_ns
                  << std::setw(10) << r.adj_p95_ns
                  << std::setw(10) << r.adj_p99_ns
                  << std::setw(11) << r.adj_p999_ns
                  << std::setw(11) << r.adj_max_ns << "\n";
    }
    std::cout << "\n";
}

void print_regime_comparison(const AggregatedResult& shallow, const AggregatedResult& deep, const std::string& op) {
    if (deep.median_ops_per_sec <= 0.0) {
        std::cout << "  " << op << ": no overflow-map throughput recorded\n";
        return;
    }

    double ratio = shallow.median_ops_per_sec / deep.median_ops_per_sec;
    bool ranges_overlap = shallow.min_ops_per_sec <= deep.max_ops_per_sec;

    std::cout << "  " << op << ": ladder region sustains " << std::fixed << std::setprecision(2)
              << ratio << "x the throughput of the overflow map"
              << (ranges_overlap ? " (ranges overlap - treat as indicative)" : " (ranges disjoint)") << "\n";
}

std::string timestamp_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << "_"
        << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void export_csv(const std::vector<AggregatedResult>& results, const AggregatedResult& floor,
                const HostInfo& host, const std::string& path) {
    std::ofstream out(path);
    out << "# " << describe_host(host) << "\n";
    out << "# measured_ops_per_run=" << MEASURED_OPS << "; runs=" << floor.runs
        << "; timer_floor_p50_ns=" << floor.p50_ns << "\n";
    out << "# throughput measured on an uninstrumented pass; adj_* percentiles have the"
           " timer floor p50 subtracted\n";

    out << "scenario,runs,median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,"
           "p50_ns,p95_ns,p99_ns,p999_ns,max_ns,"
           "adj_p50_ns,adj_p95_ns,adj_p99_ns,adj_p999_ns,adj_max_ns\n";

    auto write_row = [&out](const AggregatedResult& r) {
        out << '"' << r.name << '"' << "," << r.runs << "," << r.median_ops_per_sec << ","
            << r.min_ops_per_sec << "," << r.max_ops_per_sec << ","
            << r.p50_ns << "," << r.p95_ns << "," << r.p99_ns << "," << r.p999_ns << "," << r.max_ns << ","
            << r.adj_p50_ns << "," << r.adj_p95_ns << "," << r.adj_p99_ns << ","
            << r.adj_p999_ns << "," << r.adj_max_ns << "\n";
    };

    write_row(floor);
    for (const auto& r : results) write_row(r);
}

}  // namespace

int main(int argc, char** argv) {
    int repeats = DEFAULT_REPEATS;
    int requested_cpu = -2;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeats = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
            requested_cpu = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-pin") == 0) {
            requested_cpu = -1;
        }
    }

    unsigned available = std::max(1u, std::thread::hardware_concurrency());
    if (requested_cpu == -2) requested_cpu = static_cast<int>(available) - 1;

    HostInfo host = collect_host_info();
    if (requested_cpu >= 0 && pin_current_thread(requested_cpu)) {
        host.pinned_cpu = requested_cpu;
    }

    print_host_banner(host);

    std::cout << "\n[ BENCH_LOB ] running LimitOrderBook microbenchmark (" << repeats << " runs)...\n";

    std::vector<BenchResult> floor_runs, insert_shallow, insert_deep, cancel_shallow, cancel_deep, match;

    for (int run = 0; run < repeats; ++run) {
        floor_runs.push_back(run_timer_floor_benchmark());
        insert_shallow.push_back(run_scenario(run_insert_benchmark, "insert (ladder region)", true));
        insert_deep.push_back(run_scenario(run_insert_benchmark, "insert (overflow map region)", false));
        cancel_shallow.push_back(run_scenario(run_cancel_benchmark, "cancel (ladder region)", true));
        cancel_deep.push_back(run_scenario(run_cancel_benchmark, "cancel (overflow map region)", false));
        match.push_back(run_scenario(run_match_benchmark, "match (crossing, single-level partial fill)", true));
    }

    AggregatedResult agg_floor = aggregate(floor_runs, 0.0);
    double floor_ns = agg_floor.p50_ns;

    AggregatedResult agg_insert_shallow = aggregate(insert_shallow, floor_ns);
    AggregatedResult agg_insert_deep = aggregate(insert_deep, floor_ns);
    AggregatedResult agg_cancel_shallow = aggregate(cancel_shallow, floor_ns);
    AggregatedResult agg_cancel_deep = aggregate(cancel_deep, floor_ns);

    std::vector<AggregatedResult> results{ agg_insert_shallow, agg_insert_deep,
                                            agg_cancel_shallow, agg_cancel_deep,
                                            aggregate(match, floor_ns) };

    print_floor_summary(agg_floor);
    print_console_table(results, repeats, floor_ns);

    std::cout << "Ladder vs. overflow-map design validation:\n";
    print_regime_comparison(agg_insert_shallow, agg_insert_deep, "insert");
    print_regime_comparison(agg_cancel_shallow, agg_cancel_deep, "cancel");
    std::cout << "\n";

    std::filesystem::create_directories("results");
    std::string path = "results/lob_benchmark_" + timestamp_string() + ".csv";
    export_csv(results, agg_floor, host, path);
    std::cout << "[ BENCH_LOB ] wrote results to " << path << "\n";

    return 0;
}
