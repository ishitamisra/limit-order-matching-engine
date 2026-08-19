// Local web UI + JSON-ish REST API on top of MatchingEngine. This is a
// thin adapter layer: all matching logic lives in OrderBook/MatchingEngine
// (see order_book.hpp / matching_engine.hpp) -- this file only translates
// HTTP requests into engine calls and engine results into hand-formatted
// JSON (no JSON library; the response shapes are small and fixed, so a
// dependency would buy little).
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

#include "lob/http_server.hpp"
#include "lob/matching_engine.hpp"
#include "lob/web_ui.hpp"

using namespace lob;

namespace {

// Trades are reported back to the client that submitted the order (via
// this thread_local capture -- safe because MatchingEngine invokes the
// trade callback synchronously, on the calling thread, before submit_*
// returns) AND kept in a small rolling tape for the trade-tape panel.
thread_local std::vector<Trade> g_captured_trades;

struct TapeEntry { std::string symbol; Trade trade; };
std::mutex g_tape_mutex;
std::deque<TapeEntry> g_tape;
constexpr std::size_t kTapeCapacity = 500;

void on_trade(const std::string& symbol, const Trade& t) {
    g_captured_trades.push_back(t);
    std::lock_guard<std::mutex> lock(g_tape_mutex);
    g_tape.push_back({symbol, t});
    if (g_tape.size() > kTapeCapacity) g_tape.pop_front();
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::string trade_json(const Trade& t) {
    std::ostringstream out;
    out << "{\"price\":" << t.price << ",\"qty\":" << t.quantity
        << ",\"resting_id\":" << t.resting_order_id
        << ",\"aggressor_id\":" << t.aggressor_order_id << "}";
    return out.str();
}

const std::string& required_field(const HttpRequest& req, const std::string& name) {
    auto it = req.form.find(name);
    if (it == req.form.end()) throw std::runtime_error("missing field: " + name);
    return it->second;
}

std::string query_or(const HttpRequest& req, const std::string& name, const std::string& fallback) {
    auto it = req.query.find(name);
    return it == req.query.end() ? fallback : it->second;
}

} // namespace

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 8080;

    MatchingEngine engine(on_trade);
    HttpServer server;

    server.get("/", [](const HttpRequest&) {
        return HttpResponse{200, "OK", "text/html; charset=utf-8", kIndexHtml};
    });

    server.get("/api/book", [&engine](const HttpRequest& req) {
        std::string symbol = query_or(req, "symbol", "AAPL");
        std::size_t depth = static_cast<std::size_t>(std::stoul(query_or(req, "depth", "12")));

        auto levels = engine.with_book(symbol, [&](const OrderBook& book) {
            return std::make_pair(book.bid_levels(depth), book.ask_levels(depth));
        });

        std::ostringstream out;
        out << "{\"symbol\":\"" << json_escape(symbol) << "\",\"bids\":[";
        for (std::size_t i = 0; i < levels.first.size(); ++i) {
            if (i) out << ",";
            out << "[" << levels.first[i].price << "," << levels.first[i].quantity << "]";
        }
        out << "],\"asks\":[";
        for (std::size_t i = 0; i < levels.second.size(); ++i) {
            if (i) out << ",";
            out << "[" << levels.second[i].price << "," << levels.second[i].quantity << "]";
        }
        out << "]}";
        return HttpResponse{200, "OK", "application/json", out.str()};
    });

    server.get("/api/trades", [](const HttpRequest& req) {
        std::string symbol = query_or(req, "symbol", "");
        std::size_t limit = static_cast<std::size_t>(std::stoul(query_or(req, "limit", "25")));

        std::vector<TapeEntry> matched;
        {
            std::lock_guard<std::mutex> lock(g_tape_mutex);
            for (auto it = g_tape.rbegin(); it != g_tape.rend() && matched.size() < limit; ++it) {
                if (symbol.empty() || it->symbol == symbol) matched.push_back(*it);
            }
        }
        std::reverse(matched.begin(), matched.end());

        std::ostringstream out;
        out << "{\"trades\":[";
        for (std::size_t i = 0; i < matched.size(); ++i) {
            if (i) out << ",";
            out << trade_json(matched[i].trade);
        }
        out << "]}";
        return HttpResponse{200, "OK", "application/json", out.str()};
    });

    server.post("/api/order", [&engine](const HttpRequest& req) {
        std::string symbol = required_field(req, "symbol");
        std::string side_str = required_field(req, "side");
        std::string type_str = required_field(req, "type");
        Quantity qty = static_cast<Quantity>(std::stoull(required_field(req, "qty")));
        Side side = (side_str == "buy") ? Side::Buy : Side::Sell;

        g_captured_trades.clear();
        Quantity filled = 0;
        OrderId reported_id = 0;

        if (type_str == "market") {
            Quantity unfilled_qty = engine.submit_market_order(symbol, side, qty);
            filled = qty - unfilled_qty;
            reported_id = g_captured_trades.empty() ? 0 : g_captured_trades.front().aggressor_order_id;
        } else {
            Price price = static_cast<Price>(std::stoll(required_field(req, "price")));
            reported_id = engine.submit_limit_order(symbol, side, price, qty);
            for (const auto& t : g_captured_trades) filled += t.quantity;
        }

        std::ostringstream out;
        out << "{\"order_id\":" << reported_id << ",\"unfilled\":" << (qty - filled) << ",\"trades\":[";
        for (std::size_t i = 0; i < g_captured_trades.size(); ++i) {
            if (i) out << ",";
            out << trade_json(g_captured_trades[i]);
        }
        out << "]}";
        return HttpResponse{200, "OK", "application/json", out.str()};
    });

    server.post("/api/cancel", [&engine](const HttpRequest& req) {
        std::string symbol = required_field(req, "symbol");
        OrderId id = static_cast<OrderId>(std::stoull(required_field(req, "order_id")));
        bool ok = engine.cancel_order(symbol, id);
        return HttpResponse{200, "OK", "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}"};
    });

    // Populates a symbol with random two-sided resting liquidity so the
    // book looks alive without manually typing dozens of orders. Buys are
    // always placed strictly below mid and sells strictly above it, so
    // seeding itself never crosses the book (no unexpected fills).
    server.post("/api/seed", [&engine](const HttpRequest& req) {
        std::string symbol = required_field(req, "symbol");
        int count = req.form.count("count") ? std::stoi(req.form.at("count")) : 40;

        static thread_local std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<Price> offset_dist(1, 60);
        std::uniform_int_distribution<int> qty_dist(1, 50);
        constexpr Price kMid = 10000;

        for (int i = 0; i < count; ++i) {
            Price offset = offset_dist(rng);
            engine.submit_limit_order(symbol, Side::Buy, kMid - offset, static_cast<Quantity>(qty_dist(rng)));
            engine.submit_limit_order(symbol, Side::Sell, kMid + offset, static_cast<Quantity>(qty_dist(rng)));
        }
        return HttpResponse{200, "OK", "application/json", "{\"ok\":true}"};
    });

    server.listen(port);
    return 0;
}
