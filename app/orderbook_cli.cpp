#include "matchingengine/matching_engine.hpp"

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

using matchingengine::ClientId;
using matchingengine::ClientOrder;
using matchingengine::ClientOrderId;
using matchingengine::ClientTrade;
using matchingengine::MatchingEngine;
using orderbook::CancelResult;
using orderbook::CancelStatus;
using orderbook::OrderBookConfig;
using orderbook::Side;
using orderbook::SubmitResult;
using orderbook::SubmitStatus;
using orderbook::TimeInForce;

void enable_debug_leak_checking() noexcept
{
#if defined(_MSC_VER) && defined(_DEBUG)
    const int current_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    _CrtSetDbgFlag(
        current_flags | _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
}

struct CliOptions {
    OrderBookConfig book{
        .max_order_quantity = 1'000'000,
        .max_orders = 10'000,
        .min_price_ticks = 1,
        .max_price_ticks = 1'000'000
    };
    bool show_prompt{true};
    bool display_after_command{true};
    bool use_fast_book{true};
};

template <typename T>
concept UnsignedInteger = std::unsigned_integral<T>;

template <UnsignedInteger T>
[[nodiscard]] std::optional<T> parse_unsigned(std::string_view text) noexcept
{
    T value = 0;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string lowercase(std::string_view text)
{
    std::string result(text);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char character) {
            if (character >= 'A' && character <= 'Z') {
                return static_cast<char>(character - 'A' + 'a');
            }
            return static_cast<char>(character);
        });
    return result;
}

[[nodiscard]] std::vector<std::string_view> split_words(std::string_view line)
{
    std::vector<std::string_view> words;
    std::size_t position = 0;
    while (position < line.size()) {
        position = line.find_first_not_of(" \t\r\n", position);
        if (position == std::string_view::npos) {
            break;
        }
        if (line[position] == '#') {
            break;
        }

        const std::size_t end = line.find_first_of(" \t\r\n", position);
        words.push_back(line.substr(position, end - position));
        if (end == std::string_view::npos) {
            break;
        }
        position = end;
    }
    return words;
}

[[nodiscard]] std::optional<Side> parse_side(std::string_view text)
{
    const std::string value = lowercase(text);
    if (value == "buy" || value == "b") {
        return Side::buy;
    }
    if (value == "sell" || value == "s") {
        return Side::sell;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<TimeInForce> parse_time_in_force(std::string_view text)
{
    const std::string value = lowercase(text);
    if (value == "gtc") {
        return TimeInForce::gtc;
    }
    if (value == "ioc") {
        return TimeInForce::ioc;
    }
    if (value == "fok") {
        return TimeInForce::fok;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view text)
{
    const std::string value = lowercase(text);
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view name(SubmitStatus status) noexcept
{
    switch (status) {
    case SubmitStatus::accepted:
        return "accepted";
    case SubmitStatus::invalid:
        return "invalid";
    case SubmitStatus::duplicate_id:
        return "duplicate_id";
    case SubmitStatus::capacity_exhausted:
        return "capacity_exhausted";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view name(CancelStatus status) noexcept
{
    switch (status) {
    case CancelStatus::accepted:
        return "accepted";
    case CancelStatus::invalid:
        return "invalid";
    case CancelStatus::not_found:
        return "not_found";
    }
    return "unknown";
}

class OrderBookCli {
public:
    explicit OrderBookCli(
        OrderBookConfig config,
        bool display_after_command,
        bool use_fast_book)
        : engine_(config, use_fast_book),
          display_after_command_(display_after_command)
    {
    }

    int run(std::istream& input, std::ostream& output, bool show_prompt)
    {
        output << "Order book CLI. Type 'help' for commands.\n";
        std::string line;
        while (running_) {
            if (show_prompt) {
                output << "> " << std::flush;
            }
            if (!std::getline(input, line)) {
                break;
            }
            dispatch(split_words(line), output);
        }
        return input.bad() || output.bad() ? 1 : 0;
    }

private:
    struct TradePrinter {
        std::ostream& output;
        bool failed{false};
        bool notional_overflow{false};
        std::uint64_t notional_cents{0};

        void on_trade(const ClientTrade& trade) noexcept
        {
            accumulate_notional(trade);
            try {
                output << "trade maker=" << trade.maker.client_id << ':'
                       << trade.maker.order_id << " taker="
                       << trade.taker.client_id << ':' << trade.taker.order_id
                       << " price=" << trade.price_ticks
                       << " quantity=" << trade.quantity << '\n';
            } catch (...) {
                failed = true;
            }
        }

    private:
        void accumulate_notional(const ClientTrade& trade) noexcept
        {
            constexpr std::uint64_t maximum =
                std::numeric_limits<std::uint64_t>::max();
            if (trade.price_ticks != 0 &&
                trade.quantity > maximum / trade.price_ticks) {
                notional_overflow = true;
                return;
            }

            const std::uint64_t trade_cents = trade.quantity * trade.price_ticks;
            if (notional_cents > maximum - trade_cents) {
                notional_overflow = true;
                return;
            }
            notional_cents += trade_cents;
        }
    };

    void dispatch(
        const std::vector<std::string_view>& words,
        std::ostream& output)
    {
        if (words.empty()) {
            return;
        }

        const std::string command = lowercase(words.front());
        const std::span<const std::string_view> arguments(words.begin() + 1, words.end());
        if (command == "register") {
            register_client(arguments, output);
        } else if (command == "submit") {
            submit(arguments, output);
        } else if (command == "cancel") {
            cancel(arguments, output);
        } else if (command == "quote") {
            quote(arguments, output);
        } else if (command == "book") {
            book(arguments, output);
        } else if (command == "stats") {
            stats(arguments, output);
        } else if (command == "check") {
            check(arguments, output);
        } else if (command == "help") {
            help(arguments, output);
        } else if (command == "quit" || command == "exit") {
            quit(arguments, output);
        } else {
            output << "error unknown command: " << words.front()
                   << " (type 'help')\n";
        }
    }

    void register_client(
        std::span<const std::string_view> arguments,
        std::ostream& output)
    {
        if (!arguments.empty()) {
            output << "usage: register\n";
            return;
        }

        const std::optional<ClientId> client = engine_.register_client();
        if (client.has_value()) {
            output << "registered client=" << *client << '\n';
        } else {
            output << "register failed: client ID space exhausted\n";
        }
    }

    void submit(
        std::span<const std::string_view> arguments,
        std::ostream& output)
    {
        if (arguments.size() != 6) {
            output << "usage: submit <client> <order> <buy|sell> "
                      "<price_ticks> <quantity> <gtc|ioc|fok>\n";
            return;
        }

        const auto client = parse_unsigned<ClientId>(arguments[0]);
        const auto order_id = parse_unsigned<ClientOrderId>(arguments[1]);
        const auto side = parse_side(arguments[2]);
        const auto price = parse_unsigned<std::uint32_t>(arguments[3]);
        const auto quantity = parse_unsigned<std::uint64_t>(arguments[4]);
        const auto time_in_force = parse_time_in_force(arguments[5]);
        if (!client || !order_id || !side || !price || !quantity || !time_in_force) {
            output << "error invalid submit argument\n";
            return;
        }

        TradePrinter sink{output};
        const SubmitResult result = engine_.submit(
            *client,
            ClientOrder{
                .order_id = *order_id,
                .quantity = *quantity,
                .price_ticks = *price,
                .side = *side,
                .time_in_force = *time_in_force
            },
            sink);
        if (sink.failed) {
            output.clear();
            output << "warning: at least one trade could not be printed\n";
        }
        print(result, *side, sink.notional_cents, sink.notional_overflow, output);
        display_book_if_enabled(output);
    }

    void cancel(
        std::span<const std::string_view> arguments,
        std::ostream& output)
    {
        if (arguments.size() != 2) {
            output << "usage: cancel <client> <order>\n";
            return;
        }

        const auto client = parse_unsigned<ClientId>(arguments[0]);
        const auto order_id = parse_unsigned<ClientOrderId>(arguments[1]);
        if (!client || !order_id) {
            output << "error invalid cancel argument\n";
            return;
        }
        const CancelResult result = engine_.cancel(*client, *order_id);
        print(result, output);
        display_book_if_enabled(output);
    }

    void quote(
        std::span<const std::string_view> arguments,
        std::ostream& output) const
    {
        if (!arguments.empty()) {
            output << "usage: quote\n";
            return;
        }

        const orderbook::Quote value = engine_.best_quote();
        output << "quote bid=";
        if (value.bid) {
            output << value.bid->quantity << '@' << value.bid->price_ticks;
        } else {
            output << "none";
        }
        output << " ask=";
        if (value.ask) {
            output << value.ask->quantity << '@' << value.ask->price_ticks;
        } else {
            output << "none";
        }
        output << '\n';
    }

    void stats(
        std::span<const std::string_view> arguments,
        std::ostream& output) const
    {
        if (!arguments.empty()) {
            output << "usage: stats\n";
            return;
        }

        const orderbook::BookStats value = engine_.stats();
        output << "stats live_orders=" << value.live_orders
               << " free_slots=" << value.free_order_slots
               << " bid_levels=" << value.bid_levels
               << " ask_levels=" << value.ask_levels
               << " bid_quantity=" << value.bid_quantity
               << " ask_quantity=" << value.ask_quantity
               << " active_ids=" << value.active_id_entries << '\n';
    }

    void book(
        std::span<const std::string_view> arguments,
        std::ostream& output) const
    {
        if (!arguments.empty()) {
            output << "usage: book\n";
            return;
        }

        const matchingengine::ClientBookSnapshot snapshot = engine_.snapshot();
        print_book(snapshot, output);
    }

    void check(
        std::span<const std::string_view> arguments,
        std::ostream& output) const
    {
        if (!arguments.empty()) {
            output << "usage: check\n";
            return;
        }
        output << "invariants=" << (engine_.check_invariants() ? "ok" : "FAILED") << '\n';
    }

    void display_book_if_enabled(std::ostream& output) const
    {
        if (display_after_command_) {
            print_book(engine_.snapshot(), output);
        }
    }

    static void help(
        std::span<const std::string_view> arguments,
        std::ostream& output)
    {
        if (!arguments.empty()) {
            output << "usage: help\n";
            return;
        }
        output
            << "Commands:\n"
            << "  register\n"
            << "  submit <client> <order> <buy|sell> <price_ticks> "
               "<quantity> <gtc|ioc|fok>\n"
            << "  cancel <client> <order>\n"
            << "  quote\n"
            << "  book\n"
            << "  stats\n"
            << "  check\n"
            << "  help\n"
            << "  quit\n"
            << "Prices are integer ticks; one tick is one cent for transaction totals.\n"
            << "Blank lines and text following # are ignored.\n";
    }

    void quit(
        std::span<const std::string_view> arguments,
        std::ostream& output)
    {
        if (!arguments.empty()) {
            output << "usage: quit\n";
            return;
        }
        running_ = false;
        output << "bye\n";
    }

    static void print(
        const SubmitResult& result,
        Side side,
        std::uint64_t notional_cents,
        bool notional_overflow,
        std::ostream& output)
    {
        output << "submit status=" << name(result.status)
               << " executed=" << result.executed_quantity
               << " resting=" << result.resting_quantity
               << " canceled=" << result.canceled_quantity
               << (side == Side::buy
                       ? " transaction_cost="
                       : " transaction_proceeds=");
        if (notional_overflow) {
            output << "overflow";
        } else {
            print_dollars(notional_cents, output);
        }
        output << '\n';
    }

    static void print(const CancelResult& result, std::ostream& output)
    {
        output << "cancel status=" << name(result.status)
               << " canceled=" << result.canceled_quantity << '\n';
    }

    static void print_dollars(std::uint64_t cents, std::ostream& output)
    {
        const char previous_fill = output.fill('0');
        output << '$' << cents / 100 << '.' << std::setw(2) << cents % 100;
        output.fill(previous_fill);
    }

    static void print_book(
        const matchingengine::ClientBookSnapshot& snapshot,
        std::ostream& output)
    {
        constexpr int order_column_width = 12;
        constexpr int quantity_column_width = 16;
        constexpr int price_column_width = 12;

        output << "book\n"
               << std::right
               << std::setw(order_column_width) << "BUY ORDERS"
               << std::setw(quantity_column_width) << "BUY QUANTITY"
               << std::setw(price_column_width) << "PRICE"
               << std::setw(quantity_column_width) << "SELL QUANTITY"
               << std::setw(order_column_width) << "SELL ORDERS" << '\n';

        if (snapshot.bids.empty() && snapshot.asks.empty()) {
            output << std::right
                   << std::setw(
                          order_column_width + quantity_column_width +
                          price_column_width)
                   << "(empty)" << '\n';
            return;
        }

        // Asks are stored best-to-worst (ascending), so reverse them to make
        // the whole table descend by price. Bids are already descending.
        for (auto level = snapshot.asks.rbegin(); level != snapshot.asks.rend(); ++level) {
            output << std::right
                   << std::setw(order_column_width) << ""
                   << std::setw(quantity_column_width) << ""
                   << std::setw(price_column_width) << level->price_ticks
                   << std::setw(quantity_column_width) << level->total_quantity
                   << std::setw(order_column_width) << level->orders.size() << '\n';
        }
        for (const matchingengine::ClientPriceLevel& level : snapshot.bids) {
            output << std::right
                   << std::setw(order_column_width) << level.orders.size()
                   << std::setw(quantity_column_width) << level.total_quantity
                   << std::setw(price_column_width) << level.price_ticks << '\n';
        }
    }

    MatchingEngine engine_;
    bool display_after_command_{true};
    bool running_{true};
};

[[nodiscard]] CliOptions parse_options(int argc, char** argv)
{
    CliOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout
                << "Usage: orderbook_cli [--no-prompt] [--display true|false] "
                   "[--fast-book true|false] "
                   "[--max-orders N] "
                   "[--max-quantity N] [--min-price N] [--max-price N]\n";
            std::exit(0);
        }
        if (argument == "--no-prompt") {
            options.show_prompt = false;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(argument) + " requires a value");
        }

        const std::string_view value(argv[++index]);
        if (argument == "--display") {
            const auto parsed = parse_bool(value);
            if (!parsed) {
                throw std::invalid_argument(
                    "--display requires either true or false");
            }
            options.display_after_command = *parsed;
        } else if (argument == "--fast-book") {
            const auto parsed = parse_bool(value);
            if (!parsed) {
                throw std::invalid_argument(
                    "--fast-book requires either true or false");
            }
            options.use_fast_book = *parsed;
        } else if (argument == "--max-orders") {
            const auto parsed = parse_unsigned<std::uint32_t>(value);
            if (!parsed) {
                throw std::invalid_argument("invalid --max-orders value");
            }
            options.book.max_orders = *parsed;
        } else if (argument == "--max-quantity") {
            const auto parsed = parse_unsigned<std::uint64_t>(value);
            if (!parsed) {
                throw std::invalid_argument("invalid --max-quantity value");
            }
            options.book.max_order_quantity = *parsed;
        } else if (argument == "--min-price") {
            const auto parsed = parse_unsigned<std::uint32_t>(value);
            if (!parsed) {
                throw std::invalid_argument("invalid --min-price value");
            }
            options.book.min_price_ticks = *parsed;
        } else if (argument == "--max-price") {
            const auto parsed = parse_unsigned<std::uint32_t>(value);
            if (!parsed) {
                throw std::invalid_argument("invalid --max-price value");
            }
            options.book.max_price_ticks = *parsed;
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv)
{
    enable_debug_leak_checking();
    try {
        const CliOptions options = parse_options(argc, argv);
        OrderBookCli cli(
            options.book,
            options.display_after_command,
            options.use_fast_book);
        return cli.run(std::cin, std::cout, options.show_prompt);
    } catch (const std::exception& error) {
        std::cerr << "orderbook_cli: " << error.what() << '\n';
        return 1;
    }
}
