// Small interactive demo of MatchingEngine. Not a benchmark (see
// bench/benchmark.cpp for that) -- this is here so you can see and feel
// price-time priority matching happen order by order.
//
// Commands:
//   limit <symbol> buy|sell <price> <qty>
//   market <symbol> buy|sell <qty>
//   cancel <symbol> <order_id>
//   book <symbol> [depth]
//   quit
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

void print_trade(const std::string& symbol, const Trade& t) {
    std::cout << "  TRADE " << symbol << " " << t.quantity << " @ " << t.price
              << "  (resting #" << t.resting_order_id << " x aggressor #"
              << t.aggressor_order_id << ")\n";
}

void print_book(MatchingEngine& engine, const std::string& symbol, std::size_t depth) {
    engine.with_book(symbol, [&](const OrderBook& book) {
        auto asks = book.ask_levels(depth);
        auto bids = book.bid_levels(depth);
        std::cout << std::left << std::setw(12) << "ASK PRICE" << "QTY\n";
        for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
            std::cout << std::left << std::setw(12) << it->price << it->quantity << "\n";
        }
        std::cout << "----\n";
        for (const auto& lvl : bids) {
            std::cout << std::left << std::setw(12) << lvl.price << lvl.quantity << "\n";
        }
        std::cout << std::left << std::setw(12) << "BID PRICE" << "QTY\n";
        return 0;
    });
}

Side parse_side(const std::string& s) {
    if (s == "buy") return Side::Buy;
    if (s == "sell") return Side::Sell;
    throw std::invalid_argument("side must be 'buy' or 'sell', got: " + s);
}

} // namespace

int main() {
    MatchingEngine engine(print_trade);

    std::cout << "limit order book demo. Commands:\n"
              << "  limit <symbol> buy|sell <price> <qty>\n"
              << "  market <symbol> buy|sell <qty>\n"
              << "  cancel <symbol> <order_id>\n"
              << "  book <symbol> [depth]\n"
              << "  quit\n\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) continue;

        try {
            if (cmd == "quit" || cmd == "exit") {
                break;
            } else if (cmd == "limit") {
                std::string symbol, side_str;
                Price price;
                Quantity qty;
                iss >> symbol >> side_str >> price >> qty;
                OrderId id = engine.submit_limit_order(symbol, parse_side(side_str), price, qty);
                std::cout << "  order #" << id << " accepted\n";
            } else if (cmd == "market") {
                std::string symbol, side_str;
                Quantity qty;
                iss >> symbol >> side_str >> qty;
                Quantity unfilled = engine.submit_market_order(symbol, parse_side(side_str), qty);
                std::cout << "  " << (qty - unfilled) << "/" << qty << " filled, "
                          << unfilled << " unfilled (dropped)\n";
            } else if (cmd == "cancel") {
                std::string symbol;
                OrderId id;
                iss >> symbol >> id;
                bool ok = engine.cancel_order(symbol, id);
                std::cout << "  " << (ok ? "cancelled" : "unknown order id") << "\n";
            } else if (cmd == "book") {
                std::string symbol;
                std::size_t depth = 5;
                iss >> symbol;
                if (iss >> depth) { /* optional override */ }
                print_book(engine, symbol, depth);
            } else {
                std::cout << "  unknown command: " << cmd << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << "  error: " << e.what() << "\n";
        }
    }
    return 0;
}
