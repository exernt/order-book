#include "orderbook/order_book.hpp"
#include "orderbook/fast_order_book.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#define ORDERBOOK_HAS_INVARIANT_TSC 1
#elif defined(__GNUC__) && defined(__x86_64__)
#include <x86intrin.h>
#define ORDERBOOK_HAS_INVARIANT_TSC 1
#else
#define ORDERBOOK_HAS_INVARIANT_TSC 0
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
using orderbook::NewOrder;
using orderbook::FastOrderBook;
using orderbook::OrderBook;
using orderbook::OrderId;
using orderbook::Side;
using orderbook::SubmitResult;
using orderbook::SubmitStatus;
using orderbook::TimeInForce;
using orderbook::Trade;

constexpr std::uint32_t bid_price = 999;
constexpr std::uint32_t ask_price = 1'001;

#if ORDERBOOK_HAS_INVARIANT_TSC
[[nodiscard]] std::uint64_t latency_start() noexcept
{
    _mm_lfence();
    return __rdtsc();
}

[[nodiscard]] std::uint64_t latency_stop() noexcept
{
    unsigned auxiliary = 0;
    const std::uint64_t result = __rdtscp(&auxiliary);
    _mm_lfence();
    return result;
}

[[nodiscard]] double tsc_ticks_per_nanosecond()
{
    static const double result = [] {
        const auto wall_start = Clock::now();
        const std::uint64_t tick_start = latency_start();
        const auto target = wall_start + std::chrono::milliseconds(50);
        while (Clock::now() < target) {
            _mm_pause();
        }
        const std::uint64_t tick_stop = latency_stop();
        const auto wall_stop = Clock::now();
        const double nanoseconds = std::chrono::duration<double, std::nano>(
            wall_stop - wall_start).count();
        return static_cast<double>(tick_stop - tick_start) / nanoseconds;
    }();
    return result;
}
#endif

struct Options {
    std::size_t commands{200'000};
    std::size_t warmup{20'000};
    std::size_t trials{5};
    std::uint32_t max_orders{50'000};
    std::uint32_t price_levels{201};
};

enum class CommandKind : std::uint8_t { submit, cancel };

struct Command {
    CommandKind kind{CommandKind::submit};
    NewOrder order{};
    OrderId cancel_id{0};

    [[nodiscard]] static Command submission(NewOrder value) noexcept
    {
        return {.kind = CommandKind::submit, .order = value};
    }

    [[nodiscard]] static Command cancellation(OrderId id) noexcept
    {
        return {.kind = CommandKind::cancel, .cancel_id = id};
    }
};

struct Scenario {
    std::string name;
    std::string description;
    std::vector<NewOrder> setup;
    std::vector<Command> commands;
};

struct ChecksumSink {
    std::uint64_t checksum{0xcbf29ce484222325ULL};
    std::uint64_t trades{0};

    void combine(std::uint64_t value) noexcept
    {
        checksum ^= value + 0x9e3779b97f4a7c15ULL +
                    (checksum << 6U) + (checksum >> 2U);
    }

    void on_trade(const Trade& trade) noexcept
    {
        combine(trade.maker_id);
        combine(trade.taker_id);
        combine(trade.quantity);
        combine(trade.price_ticks);
        ++trades;
    }
};

struct BatchRun {
    double seconds{0.0};
    std::uint64_t checksum{0};
    std::uint32_t live_orders{0};
};

struct LatencyRun {
    std::vector<double> nanoseconds;
    std::uint64_t trades{0};
    std::uint64_t checksum{0};
    std::uint32_t live_orders{0};
    std::uint64_t id_index_operations{0};
    std::uint64_t total_probes{0};
    std::uint32_t id_index_capacity{0};
    std::uint32_t id_index_tombstones{0};
    std::uint32_t max_probe_length{0};
    std::uint64_t preallocated_storage_bytes{0};
};

struct Result {
    std::string implementation;
    std::string name;
    std::string description;
    double median_rate{0.0};
    double minimum_rate{0.0};
    double maximum_rate{0.0};
    double p50{0.0};
    double p90{0.0};
    double p99{0.0};
    double p999{0.0};
    double trades_per_command{0.0};
    std::uint32_t live_orders{0};
    std::uint64_t checksum{0};
    double probes_per_id_operation{0.0};
    std::uint32_t id_index_capacity{0};
    std::uint32_t id_index_tombstones{0};
    std::uint32_t max_probe_length{0};
    std::uint64_t preallocated_storage_bytes{0};
};

[[nodiscard]] constexpr std::uint64_t mix_checksum(
    std::uint64_t seed,
    std::uint64_t value) noexcept
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL +
                   (seed << 6U) + (seed >> 2U));
}

[[nodiscard]] constexpr NewOrder make_order(
    OrderId id,
    Side side,
    std::uint32_t price,
    std::uint64_t quantity,
    TimeInForce time_in_force = TimeInForce::gtc) noexcept
{
    return {
        .id = id,
        .quantity = quantity,
        .price_ticks = price,
        .side = side,
        .time_in_force = time_in_force
    };
}

[[nodiscard]] constexpr orderbook::OrderBookConfig config(
    std::uint32_t max_orders,
    std::uint32_t price_levels) noexcept
{
    return {
        .max_order_quantity = 1'000'000,
        .max_orders = max_orders,
        .min_price_ticks = 900,
        .max_price_ticks = 900 + price_levels - 1
    };
}

void combine(ChecksumSink& sink, const SubmitResult& result) noexcept
{
    sink.combine(result.executed_quantity);
    sink.combine(result.resting_quantity);
    sink.combine(result.canceled_quantity);
    sink.combine(static_cast<std::uint8_t>(result.status));
}

template <typename Book>
void execute(Book& book, const Command& command, ChecksumSink& sink) noexcept
{
    if (command.kind == CommandKind::submit) {
        combine(sink, book.submit(command.order, sink));
        return;
    }

    const auto result = book.cancel(command.cancel_id);
    sink.combine(result.canceled_quantity);
    sink.combine(static_cast<std::uint8_t>(result.status));
}

template <typename Book>
void prepare(Book& book, const Scenario& scenario, ChecksumSink& sink)
{
    for (const NewOrder& order : scenario.setup) {
        const SubmitResult result = book.submit(order, sink);
        if (result.status != SubmitStatus::accepted ||
            result.resting_quantity != order.quantity) {
            throw std::runtime_error("benchmark setup order was not admitted");
        }
    }
}

template <typename Book>
void warm_up(
    Book& book,
    const Scenario& scenario,
    std::size_t count,
    ChecksumSink& sink) noexcept
{
    for (std::size_t index = 0; index < count; ++index) {
        execute(book, scenario.commands[index], sink);
    }
}

template <typename Book>
[[nodiscard]] BatchRun run_batch(const Scenario& scenario, const Options& options)
{
    Book book(config(options.max_orders, options.price_levels));
    ChecksumSink sink;
    prepare(book, scenario, sink);
    warm_up(book, scenario, options.warmup, sink);

    const auto started = Clock::now();
    for (std::size_t index = options.warmup; index < scenario.commands.size(); ++index) {
        execute(book, scenario.commands[index], sink);
    }
    const auto stopped = Clock::now();

    if (!book.check_invariants()) {
        throw std::runtime_error("invariant failure after batch run");
    }
    return {
        .seconds = std::chrono::duration<double>(stopped - started).count(),
        .checksum = sink.checksum,
        .live_orders = book.stats().live_orders
    };
}

template <typename Book>
[[nodiscard]] LatencyRun run_latencies(
    const Scenario& scenario,
    const Options& options)
{
    Book book(config(options.max_orders, options.price_levels));
    ChecksumSink sink;
    prepare(book, scenario, sink);
    warm_up(book, scenario, options.warmup, sink);
    const auto stats_before = book.stats();

    LatencyRun run;
    run.nanoseconds.reserve(options.commands);
    const std::uint64_t trades_before = sink.trades;
    for (std::size_t index = options.warmup; index < scenario.commands.size(); ++index) {
#if ORDERBOOK_HAS_INVARIANT_TSC
        const std::uint64_t started = latency_start();
        execute(book, scenario.commands[index], sink);
        const std::uint64_t stopped = latency_stop();
        run.nanoseconds.push_back(
            static_cast<double>(stopped - started) / tsc_ticks_per_nanosecond());
#else
        const auto started = Clock::now();
        execute(book, scenario.commands[index], sink);
        const auto stopped = Clock::now();
        run.nanoseconds.push_back(
            std::chrono::duration<double, std::nano>(stopped - started).count());
#endif
    }

    const auto stats = book.stats();
    if (!book.check_invariants()) {
        throw std::runtime_error("invariant failure after latency run");
    }
    run.trades = sink.trades - trades_before;
    run.checksum = sink.checksum;
    run.live_orders = stats.live_orders;
    run.id_index_operations =
        stats.id_index_operations - stats_before.id_index_operations;
    run.total_probes = stats.total_probes - stats_before.total_probes;
    run.id_index_capacity = stats.id_index_capacity;
    run.id_index_tombstones = stats.id_index_tombstones;
    run.max_probe_length = stats.max_probe_length;
    run.preallocated_storage_bytes = stats.preallocated_storage_bytes;
    return run;
}

[[nodiscard]] double percentile(
    const std::vector<double>& sorted,
    double fraction)
{
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::min(rank == 0 ? std::size_t{0} : rank - 1, sorted.size() - 1)];
}

template <typename Book>
[[nodiscard]] Result benchmark(
    const Scenario& scenario,
    const Options& options,
    std::string implementation)
{
    std::vector<double> rates;
    rates.reserve(options.trials);
    std::uint64_t checksum = 0;
    std::uint32_t live_orders = 0;

    for (std::size_t trial = 0; trial < options.trials; ++trial) {
        const BatchRun run = run_batch<Book>(scenario, options);
        rates.push_back(
            static_cast<double>(options.commands) / run.seconds / 1'000'000.0);
        checksum = mix_checksum(checksum, run.checksum + trial);
        live_orders = run.live_orders;
    }
    std::sort(rates.begin(), rates.end());

    LatencyRun latency = run_latencies<Book>(scenario, options);
    std::sort(latency.nanoseconds.begin(), latency.nanoseconds.end());
    checksum = mix_checksum(checksum, latency.checksum);

    return {
        .implementation = std::move(implementation),
        .name = scenario.name,
        .description = scenario.description,
        .median_rate = rates[rates.size() / 2],
        .minimum_rate = rates.front(),
        .maximum_rate = rates.back(),
        .p50 = percentile(latency.nanoseconds, 0.50),
        .p90 = percentile(latency.nanoseconds, 0.90),
        .p99 = percentile(latency.nanoseconds, 0.99),
        .p999 = percentile(latency.nanoseconds, 0.999),
        .trades_per_command =
            static_cast<double>(latency.trades) / static_cast<double>(options.commands),
        .live_orders = live_orders,
        .checksum = checksum,
        .probes_per_id_operation = latency.id_index_operations == 0
            ? 0.0
            : static_cast<double>(latency.total_probes) /
                  static_cast<double>(latency.id_index_operations),
        .id_index_capacity = latency.id_index_capacity,
        .id_index_tombstones = latency.id_index_tombstones,
        .max_probe_length = latency.max_probe_length,
        .preallocated_storage_bytes = latency.preallocated_storage_bytes
    };
}

[[nodiscard]] Scenario resting_heavy(std::size_t count)
{
    Scenario scenario{
        .name = "resting",
        .description = "non-crossing GTC insertion followed by cancellation"
    };
    scenario.commands.reserve(count);

    OrderId next_id = 1;
    OrderId live_id = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (index % 2 == 0) {
            live_id = next_id++;
            scenario.commands.push_back(Command::submission(
                make_order(live_id, Side::buy, bid_price, 10)));
        } else {
            scenario.commands.push_back(Command::cancellation(live_id));
        }
    }
    return scenario;
}

[[nodiscard]] Scenario matching_heavy(std::size_t count)
{
    Scenario scenario{
        .name = "matching",
        .description = "one maker insertion followed by one aggressive IOC"
    };
    scenario.commands.reserve(count);

    OrderId next_id = 1'000'000;
    for (std::size_t index = 0; index < count; ++index) {
        switch (index % 4) {
        case 0:
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::sell, ask_price, 10)));
            break;
        case 1:
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::buy, ask_price, 10, TimeInForce::ioc)));
            break;
        case 2:
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::buy, bid_price, 10)));
            break;
        default:
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::sell, bid_price, 10, TimeInForce::ioc)));
            break;
        }
    }
    return scenario;
}

[[nodiscard]] Scenario sweep_heavy(std::size_t count)
{
    constexpr std::size_t makers = 8;
    constexpr std::size_t cycle = makers + 1;

    Scenario scenario{
        .name = "sweep",
        .description = "eight one-lot makers across levels, then one sweeping IOC"
    };
    scenario.commands.reserve(count);

    OrderId next_id = 2'000'000;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t position = index % cycle;
        if (position < makers) {
            const auto price = static_cast<std::uint32_t>(ask_price + position);
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::sell, price, 1)));
        } else {
            scenario.commands.push_back(Command::submission(make_order(
                next_id++,
                Side::buy,
                ask_price + static_cast<std::uint32_t>(makers - 1),
                makers,
                TimeInForce::ioc)));
        }
    }
    return scenario;
}

[[nodiscard]] Scenario sparse_sweep_heavy(
    std::size_t count,
    std::uint32_t max_price)
{
    constexpr std::size_t makers = 8;
    constexpr std::size_t cycle = makers + 1;
    const std::uint32_t stride = (max_price - ask_price) /
                                 static_cast<std::uint32_t>(makers - 1);
    Scenario scenario{
        .name = "sparse-sweep",
        .description = "eight one-lot makers spread across the configured price band"
    };
    scenario.commands.reserve(count);

    OrderId next_id = 2'500'000;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t position = index % cycle;
        if (position < makers) {
            const auto price = ask_price +
                static_cast<std::uint32_t>(position) * stride;
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::sell, price, 1)));
        } else {
            scenario.commands.push_back(Command::submission(make_order(
                next_id++, Side::buy, ask_price +
                    static_cast<std::uint32_t>(makers - 1) * stride,
                makers, TimeInForce::ioc)));
        }
    }
    return scenario;
}

[[nodiscard]] Scenario fok_heavy(std::size_t count)
{
    Scenario scenario{
        .name = "fok",
        .description = "two makers, one failed FOK preflight, then one successful FOK"
    };
    scenario.commands.reserve(count);

    OrderId next_id = 3'000'000;
    for (std::size_t index = 0; index < count; ++index) {
        switch (index % 4) {
        case 0:
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::sell, ask_price, 10)));
            break;
        case 1:
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::sell, ask_price + 1, 10)));
            break;
        case 2:
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::buy, ask_price + 1, 21, TimeInForce::fok)));
            break;
        default:
            scenario.commands.push_back(Command::submission(
                make_order(next_id++, Side::buy, ask_price + 1, 20, TimeInForce::fok)));
            break;
        }
    }
    return scenario;
}

[[nodiscard]] Scenario cancel_heavy(std::size_t count)
{
    constexpr std::size_t initial_depth = 4'096;
    Scenario scenario{
        .name = "cancel",
        .description = "deep-queue insert, middle cancellation, and controlled miss"
    };
    scenario.setup.reserve(initial_depth);
    scenario.commands.reserve(count);

    std::deque<OrderId> live_ids;
    OrderId next_id = 4'000'000;
    for (std::size_t index = 0; index < initial_depth; ++index) {
        const OrderId id = next_id++;
        scenario.setup.push_back(make_order(id, Side::buy, bid_price, 1));
        live_ids.push_back(id);
    }

    for (std::size_t index = 0; index < count; ++index) {
        switch (index % 3) {
        case 0: {
            const OrderId id = next_id++;
            scenario.commands.push_back(Command::submission(
                make_order(id, Side::buy, bid_price, 1)));
            live_ids.push_back(id);
            break;
        }
        case 1: {
            const auto middle = live_ids.begin() +
                                static_cast<std::ptrdiff_t>(live_ids.size() / 2);
            scenario.commands.push_back(Command::cancellation(*middle));
            live_ids.erase(middle);
            break;
        }
        default:
            scenario.commands.push_back(Command::cancellation(
                std::numeric_limits<OrderId>::max() - index));
            break;
        }
    }
    return scenario;
}

[[nodiscard]] double clock_overhead()
{
    constexpr std::size_t samples = 100'000;
#if ORDERBOOK_HAS_INVARIANT_TSC
    std::uint64_t ticks = 0;
    for (std::size_t index = 0; index < samples; ++index) {
        const std::uint64_t started = latency_start();
        const std::uint64_t stopped = latency_stop();
        ticks += stopped - started;
    }
    return static_cast<double>(ticks) /
           static_cast<double>(samples) / tsc_ticks_per_nanosecond();
#else
    const auto batch_started = Clock::now();
    for (std::size_t index = 0; index < samples; ++index) {
        const auto started = Clock::now();
        const auto stopped = Clock::now();
        if (stopped < started) {
            throw std::runtime_error("steady clock moved backward");
        }
    }
    const auto batch_stopped = Clock::now();
    const auto elapsed = std::chrono::duration_cast<Nanoseconds>(
        batch_stopped - batch_started).count();
    return static_cast<double>(elapsed) / static_cast<double>(samples);
#endif
}

[[nodiscard]] std::size_t parse_size(std::string_view text, std::string_view name)
{
    std::size_t value = 0;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last || value == 0) {
        throw std::invalid_argument(std::string(name) + " requires a positive integer");
    }
    return value;
}

[[nodiscard]] Options parse_options(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            std::cout << "Usage: orderbook_bench "
                         "[--commands N] [--warmup N] [--trials N] "
                         "[--max-orders N] [--price-levels N]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(argument) + " requires a value");
        }
        const std::string_view value(argv[++index]);
        if (argument == "--commands") {
            options.commands = parse_size(value, argument);
        } else if (argument == "--warmup") {
            options.warmup = parse_size(value, argument);
        } else if (argument == "--trials") {
            options.trials = parse_size(value, argument);
        } else if (argument == "--max-orders") {
            const std::size_t parsed = parse_size(value, argument);
            if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("--max-orders is too large");
            }
            options.max_orders = static_cast<std::uint32_t>(parsed);
        } else if (argument == "--price-levels") {
            const std::size_t parsed = parse_size(value, argument);
            if (parsed > FastOrderBook::max_dense_price_levels || parsed < 109) {
                throw std::invalid_argument(
                    "--price-levels must be between 109 and 1000000");
            }
            options.price_levels = static_cast<std::uint32_t>(parsed);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

void print_results(
    const Options& options,
    double overhead,
    const std::vector<Result>& results)
{
    std::cout << "Reference versus Milestone 7 order book benchmark\n"
              << "  measured commands/workload: " << options.commands << '\n'
              << "  warm-up commands/workload:  " << options.warmup << '\n'
              << "  batch trials/workload:      " << options.trials << '\n'
              << "  configured order capacity:  " << options.max_orders << '\n'
              << "  configured price levels:    " << options.price_levels << '\n'
              << "  latency timer:              "
#if ORDERBOOK_HAS_INVARIANT_TSC
              << "serialized invariant TSC (" << std::fixed << std::setprecision(3)
              << tsc_ticks_per_nanosecond() << " ticks/ns)\n"
#else
              << "std::chrono::steady_clock\n"
#endif
              << "  estimated timer-pair cost:  " << std::setprecision(1)
              << overhead << " ns\n\n";

    std::cout << std::left << std::setw(14) << "workload"
              << std::setw(11) << "book"
              << std::right << std::setw(11) << "Mcmd/s"
              << std::setw(10) << "min"
              << std::setw(10) << "max"
              << std::setw(9) << "p50ns"
              << std::setw(9) << "p90ns"
              << std::setw(9) << "p99ns"
              << std::setw(10) << "p99.9ns"
              << std::setw(11) << "trades/cmd"
              << std::setw(9) << "live" << '\n';

    for (const Result& result : results) {
        std::cout << std::left << std::setw(14) << result.name
                  << std::setw(11) << result.implementation
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(11) << result.median_rate
                  << std::setw(10) << result.minimum_rate
                  << std::setw(10) << result.maximum_rate
                  << std::setprecision(1)
                  << std::setw(9) << result.p50
                  << std::setw(9) << result.p90
                  << std::setw(9) << result.p99
                  << std::setw(10) << result.p999 << std::setprecision(3)
                  << std::setw(11) << result.trades_per_command
                  << std::setw(9) << result.live_orders << '\n';
    }

    std::cout << "\nFast/reference comparison:\n"
              << std::left << std::setw(14) << "workload"
              << std::right << std::setw(15) << "throughput"
              << std::setw(15) << "ref avg ns"
              << std::setw(15) << "fast avg ns"
              << std::setw(15) << "avg change"
              << std::setw(15) << "p99 change" << '\n';
    for (std::size_t index = 0; index + 1 < results.size(); index += 2) {
        const Result& reference = results[index];
        const Result& fast = results[index + 1];
        const double throughput_ratio = fast.median_rate / reference.median_rate;
        const double reference_average_ns = 1'000.0 / reference.median_rate;
        const double fast_average_ns = 1'000.0 / fast.median_rate;
        const double average_change =
            100.0 * (fast_average_ns - reference_average_ns) / reference_average_ns;
        const double p99_change = reference.p99 == 0.0
            ? 0.0
            : 100.0 * (fast.p99 - reference.p99) / reference.p99;
        std::cout << std::left << std::setw(14) << reference.name
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(14) << throughput_ratio << 'x'
                  << std::setw(15) << reference_average_ns
                  << std::setw(15) << fast_average_ns
                  << std::showpos << std::setprecision(1)
                  << std::setw(14) << average_change << '%'
                  << std::setw(14) << p99_change << '%' << std::noshowpos << '\n';
    }

    std::cout << "\nFast active-ID index probes:\n"
              << std::left << std::setw(14) << "workload"
              << std::right << std::setw(15) << "avg probes/op"
              << std::setw(12) << "max probe"
              << std::setw(12) << "buckets"
              << std::setw(12) << "tombstones" << '\n';
    for (std::size_t index = 1; index < results.size(); index += 2) {
        const Result& fast = results[index];
        std::cout << std::left << std::setw(14) << fast.name
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(15) << fast.probes_per_id_operation
                  << std::setw(12) << fast.max_probe_length
                  << std::setw(12) << fast.id_index_capacity
                  << std::setw(12) << fast.id_index_tombstones << '\n';
    }

    if (results.size() >= 2) {
        const double mebibytes = static_cast<double>(
            results[1].preallocated_storage_bytes) / (1024.0 * 1024.0);
        std::cout << "\nFast preallocated array storage: " << std::fixed
                  << std::setprecision(3) << mebibytes << " MiB\n";
    }

    std::uint64_t checksum = 0;
    std::cout << "\nWorkloads:\n";
    for (std::size_t index = 0; index < results.size(); index += 2) {
        std::cout << "  " << results[index].name << ": "
                  << results[index].description << '\n';
    }
    for (const Result& result : results) {
        checksum = mix_checksum(checksum, result.checksum);
    }
    std::cout << "\nchecksum: 0x" << std::hex << checksum << std::dec << '\n'
              << "Latency includes the reported timer-pair cost and is not adjusted.\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parse_options(argc, argv);
        if (options.commands >
            std::numeric_limits<std::size_t>::max() - options.warmup) {
            throw std::invalid_argument("command count is too large");
        }
        const std::size_t total = options.commands + options.warmup;

        std::vector<Scenario> scenarios;
        scenarios.reserve(6);
        scenarios.push_back(resting_heavy(total));
        scenarios.push_back(matching_heavy(total));
        scenarios.push_back(sweep_heavy(total));
        scenarios.push_back(sparse_sweep_heavy(
            total, 900 + options.price_levels - 1));
        scenarios.push_back(fok_heavy(total));
        scenarios.push_back(cancel_heavy(total));

        std::vector<Result> results;
        results.reserve(scenarios.size() * 2);
        for (const Scenario& scenario : scenarios) {
            Result reference = benchmark<OrderBook>(scenario, options, "reference");
            Result fast = benchmark<FastOrderBook>(scenario, options, "fast");
            if (reference.checksum != fast.checksum ||
                reference.live_orders != fast.live_orders ||
                reference.trades_per_command != fast.trades_per_command) {
                throw std::runtime_error(
                    "benchmark semantic mismatch for workload: " + scenario.name);
            }
            results.push_back(std::move(reference));
            results.push_back(std::move(fast));
        }
        print_results(options, clock_overhead(), results);
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
