module;
// WinSock2 must precede the Windows networking headers.
// clang-format off
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
// clang-format on
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <type_traits>

export module ces.server;

import ces.types;
import ces.engine;
import ces.internal;
import ces.native;

// Cold application coordinator: load Native, select protocol, stop, join every thread,
// check all release invariants, destroy resources, then publish final statistics.
export ces_exit_code ces_run_server(const ces_options* options, std::atomic<bool>* stop_requested) noexcept;
export void          ces_engine_print_final_statistics(ces_protocol                 protocol,
                                                       const ces_engine_statistics* statistics,
                                                       std::uint64_t                elapsed_milliseconds,
                                                       std::uint32_t                worker_count,
                                                       std::uint32_t                terminal_count) noexcept;

void ces_engine_print_final_statistics(ces_protocol                 protocol,
                                       const ces_engine_statistics* statistics,
                                       std::uint64_t                elapsed_milliseconds,
                                       std::uint32_t                worker_count,
                                       std::uint32_t                terminal_count) noexcept {
    const ULONGLONG guarded_elapsed      = std::max<ULONGLONG>(elapsed_milliseconds, 1U);
    const double    elapsed_seconds      = static_cast<double>(guarded_elapsed) / 1000.0;
    const double    mebibytes_per_second = static_cast<double>(statistics->bytes) / (1024.0 * 1024.0) / elapsed_seconds;
    std::printf(
        "final protocol=%s elapsed_ms=%llu workers=%u accepted=%llu active=0 outstanding=%u completions=%llu "
        "receives=%llu sends=%llu received_bytes=%llu sent_bytes=%llu bytes=%llu network_errors=%llu rejected=%llu "
        "MiB_per_sec=%.2f\n",
        protocol == ces_protocol::tcp ? "tcp" : "udp", static_cast<unsigned long long>(elapsed_milliseconds),
        worker_count, static_cast<unsigned long long>(statistics->accepted), terminal_count,
        static_cast<unsigned long long>(statistics->completions), static_cast<unsigned long long>(statistics->receives),
        static_cast<unsigned long long>(statistics->sends), static_cast<unsigned long long>(statistics->received_bytes),
        static_cast<unsigned long long>(statistics->sent_bytes), static_cast<unsigned long long>(statistics->bytes),
        static_cast<unsigned long long>(statistics->network_errors),
        static_cast<unsigned long long>(statistics->rejected), mebibytes_per_second);
}

static ces_exit_code ces_engine_run_tcp(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                        const ces_options*                  options,
                                        std::atomic<bool>*                  stop_requested) noexcept {
    std::uint32_t worker_count = options->worker_count;
    if (worker_count == 0) {
        worker_count =
            std::clamp(static_cast<std::uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), 1U, CES_MAX_WORKERS);
    }
    // A real C++ allocation: the worker owns an atomic and must be constructed, not carved out of
    // a heap block and memset.
    std::unique_ptr<ces_engine_worker[]>           workers{ new (std::nothrow) ces_engine_worker[worker_count] };
    std::unique_ptr<ces_engine_worker_resources[]> worker_resources{ new (std::nothrow)
                                                                         ces_engine_worker_resources[worker_count] };
    if (!workers || !worker_resources) {
        ces_engine_report(L"worker array allocation", ERROR_NOT_ENOUGH_MEMORY);
        return ces_exit_code::network;
    }
    std::atomic<bool> failed{ false };
    std::uint32_t     initialized     = 0;
    std::uint32_t     prepared        = 0;
    std::uint32_t     started_workers = 0;
    for (; initialized < worker_count; ++initialized) {
        prepared         = initialized + 1U;
        const bool ready = ces_engine_worker_initialize(&workers[initialized], rio, options, &failed, initialized,
                                                        worker_count, &worker_resources[initialized]);
        if (workers[initialized].transport != nullptr && workers[initialized].transport->thread != nullptr) {
            ++started_workers;
        }
        if (!ready) {
            failed.store(true, std::memory_order_release);
            break;
        }
    }

    ces_engine_acceptor_resources acceptor_resources{};
    ces_engine_acceptor           acceptor{};
    if (acceptor.transport != nullptr) {
        acceptor.transport->resources = &acceptor_resources;
        acceptor.transport->listener  = INVALID_SOCKET;
    }
    bool acceptor_started = false;
    if (!failed.load(std::memory_order_acquire)) {
        acceptor_started = ces_engine_acceptor_initialize(&acceptor, rio, options, workers.get(), worker_count, &failed,
                                                          &acceptor_resources);
        if (!acceptor_started) {
            failed.store(true, std::memory_order_release);
        }
    }

    const ULONGLONG start = GetTickCount64();
    while (!failed.load(std::memory_order_acquire) && !stop_requested->load(std::memory_order_acquire)) {
        if (options->run_seconds != 0 && GetTickCount64() - start >= options->run_seconds * 1000ULL) {
            stop_requested->store(true, std::memory_order_release);
            break;
        }
        Sleep(10);
    }
    if (acceptor_started) {
        if (PostQueuedCompletionStatus(acceptor.transport->port, 0, ces_engine_stop_key, nullptr) == FALSE) {
            ces_engine_fail_fast(L"PostQueuedCompletionStatus(acceptor stop)", static_cast<int>(GetLastError()));
        }
    }
    ces_engine_acceptor_join(&acceptor);
    for (std::uint32_t index = 0; index < prepared; ++index) {
        if (workers[index].transport == nullptr || workers[index].transport->thread == nullptr) {
            continue;
        }
        if (PostQueuedCompletionStatus(workers[index].transport->port, 0, ces_engine_admission_closed_key, nullptr) ==
                FALSE ||
            PostQueuedCompletionStatus(workers[index].transport->port, 0, ces_engine_stop_key, nullptr) == FALSE) {
            ces_engine_fail_fast(L"PostQueuedCompletionStatus(worker shutdown)", static_cast<int>(GetLastError()));
        }
    }
    ces_engine_statistics statistics{};
    statistics.network_errors = acceptor.core.network_errors;
    statistics.rejected       = acceptor.core.rejected;
    for (std::uint32_t index = 0; index < prepared; ++index) {
        ces_engine_worker_join(&workers[index]);
    }
    ces_engine_acceptor_require_release(&acceptor);
    for (std::uint32_t index = 0; index < prepared; ++index) {
        ces_engine_worker_require_release(&workers[index]);
    }
    ces_engine_acceptor_destroy(&acceptor);
    for (std::uint32_t index = 0; index < prepared; ++index) {
        ces_engine_worker_destroy(&workers[index]);
        if (workers[index].core != nullptr) {
            ces_statistics_add(&statistics, &workers[index].core->statistics);
        }
    }
    // Every worker thread has joined; its counters remain alive in the worker array after resource
    // teardown. Snapshot only now, when no worker can still update them.
    std::array<ces_notify_diagnostic_snapshot, CES_MAX_WORKERS> snapshots{};
    for (std::uint32_t index = 0; index < started_workers; ++index) {
        snapshots[index].worker          = index;
        snapshots[index].arms            = workers[index].transport->notify_arms;
        snapshots[index].deliveries      = workers[index].transport->notify_deliveries;
        snapshots[index].timeout_wakeups = workers[index].transport->notify_timeout_wakeups;
    }
    (void) ces_write_diagnostics(snapshots.data(), started_workers, ces_protocol::tcp);
    if (options->stats) {
        // Report the workers that actually started: a partial initialisation failure must not claim
        // the requested worker count.
        ces_engine_print_final_statistics(ces_protocol::tcp, &statistics, GetTickCount64() - start, started_workers, 0);
    }
    return failed.load(std::memory_order_acquire) ? ces_exit_code::network : ces_exit_code::success;
}

ces_exit_code ces_run_server(const ces_options* options, std::atomic<bool>* stop_requested) noexcept {
    if (options == nullptr || stop_requested == nullptr) {
        return ces_exit_code::internal;
    }
    ces_engine_winsock winsock{};
    const int          winsock_status = winsock.start();
    if (winsock_status != 0) {
        ces_engine_report(L"WSAStartup", winsock_status);
        return ces_exit_code::network;
    }
    RIO_EXTENSION_FUNCTION_TABLE rio{};
    if (!ces_engine_load_rio(&rio)) {
        return ces_exit_code::network;
    }
    if (options->protocol == ces_protocol::tcp) {
        return ces_engine_run_tcp(&rio, options, stop_requested);
    }
    if (options->protocol == ces_protocol::udp) {
        return ces_engine_run_udp(&rio, options, stop_requested);
    }
    return ces_exit_code::usage;
}
