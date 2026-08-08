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
#include <vector>

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

void seed_touch(LimitOrderBook& book) {
    book.submit(BUY, LIMIT, TOUCH_BID, RESTING_QTY);
    book.submit(SELL, LIMIT, TOUCH_ASK, RESTING_QTY);
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
        auto fills = book.submit(BUY, LIMIT, price, RESTING_QTY);
        ids.push_back(fills.back().order_id);
    }
    return ids;
}

BenchResult run_insert_benchmark(const std::string& name, bool shallow) {
    LimitOrderBook book(BENCH_POOL_CAPACITY);
    seed_touch(book);
    seed_liquidity(book, WARMUP_OPS, shallow);  // warm-up, discarded

    std::vector<double> latencies_ns;
    latencies_ns.reserve(MEASURED_OPS);

    auto bench_start = std::chrono::steady_clock::now();
    for (int i = 0; i < MEASURED_OPS; ++i) {
        int64_t price = TOUCH_BID - (shallow ? shallow_offset(i) : deep_offset(i));

        auto t0 = std::chrono::steady_clock::now();
        book.submit(BUY, LIMIT, price, RESTING_QTY);
        auto t1 = std::chrono::steady_clock::now();

        latencies_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    double total_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - bench_start).count();

    return summarize(name, latencies_ns, total_seconds);
}

BenchResult run_cancel_benchmark(const std::string& name, bool shallow) {
    LimitOrderBook book(BENCH_POOL_CAPACITY);
    seed_touch(book);

    int total_needed = WARMUP_OPS + MEASURED_OPS;
    std::vector<OrderID> ids = seed_liquidity(book, total_needed, shallow);

    for (int i = 0; i < WARMUP_OPS; ++i) {
        book.cancel(ids[i]);  // warm-up, discarded
    }

    std::vector<double> latencies_ns;
    latencies_ns.reserve(MEASURED_OPS);

    auto bench_start = std::chrono::steady_clock::now();
    for (int i = WARMUP_OPS; i < total_needed; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        book.cancel(ids[i]);
        auto t1 = std::chrono::steady_clock::now();

        latencies_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    double total_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - bench_start).count();

    return summarize(name, latencies_ns, total_seconds);
}

// Repeatedly crosses the spread with a small marketable IOC against a single
// deep resting order at the touch - isolates the matching hot path (walk to
// best price level, partial-fill the head order) from level-eviction and
// ladder-window-shift costs.
BenchResult run_match_benchmark() {
    LimitOrderBook book(BENCH_POOL_CAPACITY);
    constexpr int64_t DEEP_RESTING_QTY = 10'000'000;
    constexpr int64_t MATCH_QTY = 10;

    book.submit(SELL, LIMIT, TOUCH_ASK, DEEP_RESTING_QTY);

    for (int i = 0; i < WARMUP_OPS; ++i) {
        book.submit(BUY, IOC, TOUCH_ASK, MATCH_QTY);  // warm-up, discarded
    }

    std::vector<double> latencies_ns;
    latencies_ns.reserve(MEASURED_OPS);

    auto bench_start = std::chrono::steady_clock::now();
    for (int i = 0; i < MEASURED_OPS; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        book.submit(BUY, IOC, TOUCH_ASK, MATCH_QTY);
        auto t1 = std::chrono::steady_clock::now();

        latencies_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    double total_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - bench_start).count();

    return summarize("match (crossing, single-level partial fill)", latencies_ns, total_seconds);
}

double median_of(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    if (values.size() % 2 != 0) return values[mid];
    return (values[mid - 1] + values[mid]) / 2.0;
}

AggregatedResult aggregate(const std::vector<BenchResult>& runs) {
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

    return agg;
}

void print_console_table(const std::vector<AggregatedResult>& results, int repeats) {
    std::cout << "\n=== LimitOrderBook Microbenchmark (single-threaded, "
              << MEASURED_OPS << " measured ops/scenario x " << repeats << " runs) ===\n\n";
    std::cout << "Throughput is the median across runs, with the observed range in brackets.\n"
              << "Percentiles are medians across runs; on a ~100ns clock they are quantised\n"
              << "and should be read as bounds, not exact latencies.\n\n";

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
                  << std::setw(10) << r.p50_ns
                  << std::setw(10) << r.p95_ns
                  << std::setw(10) << r.p99_ns
                  << std::setw(11) << r.p999_ns
                  << std::setw(11) << r.max_ns << "\n";
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

void export_csv(const std::vector<AggregatedResult>& results, const std::string& path) {
    std::ofstream out(path);
    out << "scenario,runs,median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,"
           "p50_ns,p95_ns,p99_ns,p999_ns,max_ns\n";
    for (const auto& r : results) {
        out << '"' << r.name << '"' << "," << r.runs << "," << r.median_ops_per_sec << ","
            << r.min_ops_per_sec << "," << r.max_ops_per_sec << ","
            << r.p50_ns << "," << r.p95_ns << "," << r.p99_ns << "," << r.p999_ns << "," << r.max_ns << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    int repeats = DEFAULT_REPEATS;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeats = std::max(1, std::atoi(argv[++i]));
        }
    }

    std::cout << "[ BENCH_LOB ] running LimitOrderBook microbenchmark (" << repeats << " runs)...\n";

    std::vector<BenchResult> insert_shallow, insert_deep, cancel_shallow, cancel_deep, match;

    for (int run = 0; run < repeats; ++run) {
        insert_shallow.push_back(run_insert_benchmark("insert (ladder region)", /*shallow=*/true));
        insert_deep.push_back(run_insert_benchmark("insert (overflow map region)", /*shallow=*/false));
        cancel_shallow.push_back(run_cancel_benchmark("cancel (ladder region)", /*shallow=*/true));
        cancel_deep.push_back(run_cancel_benchmark("cancel (overflow map region)", /*shallow=*/false));
        match.push_back(run_match_benchmark());
    }

    AggregatedResult agg_insert_shallow = aggregate(insert_shallow);
    AggregatedResult agg_insert_deep = aggregate(insert_deep);
    AggregatedResult agg_cancel_shallow = aggregate(cancel_shallow);
    AggregatedResult agg_cancel_deep = aggregate(cancel_deep);

    std::vector<AggregatedResult> results{ agg_insert_shallow, agg_insert_deep,
                                            agg_cancel_shallow, agg_cancel_deep, aggregate(match) };
    print_console_table(results, repeats);

    std::cout << "Ladder vs. overflow-map design validation:\n";
    print_regime_comparison(agg_insert_shallow, agg_insert_deep, "insert");
    print_regime_comparison(agg_cancel_shallow, agg_cancel_deep, "cancel");
    std::cout << "\n";

    std::filesystem::create_directories("results");
    std::string path = "results/lob_benchmark_" + timestamp_string() + ".csv";
    export_csv(results, path);
    std::cout << "[ BENCH_LOB ] wrote results to " << path << "\n";

    return 0;
}
