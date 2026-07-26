#include "analytics/ReportPrinter.h"

#include <iostream>
#include <iomanip>

void print_report(const ExecutionReport& report) {
    std::cout << "\n[ ANALYTICS      ] Execution Report for Order " << report.parent_id << "\n";
    std::cout << "  Side:                     " << (report.side == Side::BUY ? "BUY" : "SELL") << "\n";
    std::cout << "  Intended / Filled:        " << report.intended_size << " / " << report.filled_size << "\n";
    std::cout << "  Fill Rate:                " << std::fixed << std::setprecision(2) << (report.fill_rate * 100.0) << "%\n";
    std::cout << "  Decision Price:           " << report.decision_price << "\n";
    std::cout << "  Avg Fill Price:           " << report.avg_fill_price << "\n";
    std::cout << "  Window VWAP:              " << report.window_vwap << "\n";
    std::cout << "  Implementation Shortfall: " << report.implementation_shortfall << "\n";
    std::cout << "  VWAP Slippage:            " << report.vwap_slippage << "\n";
    std::cout << "  Timed Out:                " << (report.timed_out ? "yes" : "no") << "\n";
    std::cout << "  Venue Breakdown:\n";
    for (const auto& [venue_id, stats] : report.venue_breakdown) {
        double avg_price = stats.filled_qty > 0 ? stats.total_notional / static_cast<double>(stats.filled_qty) : 0.0;
        std::cout << "    Venue " << venue_id << ": qty=" << stats.filled_qty
                  << " fills=" << stats.fill_count << " avg_price=" << avg_price << "\n";
    }
}
