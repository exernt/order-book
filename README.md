# C++ Order Book Implementation

This repository contains a limit order book implementation using standard containers, as well as a latency-optimised implementation. 

## Features

Both books aim to support GTC/IOC/FOK order submission/cancellation, price-time priority matching, and maker-price execution.

Key features of the latency-focused implementation include:
- Preallocated contiguous storage for orders
- Intrusive queue for each price level
- Price-indexed bid/ask arrays
- 4-level bitmap for price discovery
- Cached best bid/ask
- Rapidhash-based table for active order ID lookup


## Building

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

## Build options

- `ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS=ON`: collects probe counts for analysis; off by default because counters cost throughput.
- `ORDERBOOK_USE_FLAT_PRICE_BITMAP=ON`: specialize for price domains capped near 1,024 ticks; the hierarchical default is safe through one million ticks.
- `ORDERBOOK_ENABLE_IPO=OFF`: disable release interprocedural optimization.
- `ORDERBOOK_ENABLE_NATIVE_ARCH=ON`: generate non-portable host-specific code (disabled by default due to apparent performance penalty).
- `ORDERBOOK_ENABLE_SANITIZERS=ON`: enable AddressSanitizer on MSVC or AddressSanitizer plus UndefinedBehaviorSanitizer where the GCC/Clang runtimes are installed.

## CLI

A CLI tool is provided for manually operating the book. It uses the fast order book by default; pass `--help` for the full list of flags.

```powershell
./build/Release/orderbook_cli.exe
```

## Benchmark

```powershell
./build/Release/orderbook_bench.exe --commands 2000000 --warmup 200000 --trials 9 --max-orders 50000 --price-levels 201
```

The benchmark runs six workloads:
- resting order heavy
- matching heavy
- dense sweep
- sparse sweep
- FOK-heavy
- cancel-heavy

The benchmark reports reference and fast throughput, repeated-trial dispersion, p50/p90/p99/p99.9 latency, trades per command, final depth, ID-index diagnostics when enabled, checksum, and fast-book preallocated storage.
