#include "ces_engine_internal.h"
#include "ces_rio_layout.h"
#include "ces_types.h"

// WinSock2.h must be included before the Windows networking headers below.
// clang-format off
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

class ces_engine_winsock {
  public:
    bool started = false;

    int start() noexcept {
        WSADATA   data{};
        const int status = WSAStartup(MAKEWORD(2, 2), &data);
        started          = status == 0;
        return status;
    }

    ~ces_engine_winsock() noexcept {
        if (started) {
            WSACleanup();
        }
    }
};

static constexpr ULONG_PTR     ces_engine_stop_key                  = 1U;
static constexpr ULONG_PTR     ces_engine_admission_closed_key      = 2U;
static constexpr ULONG         ces_engine_batch_size                = 256U;
static constexpr std::uint32_t ces_engine_max_drain_batches         = 64U;
static constexpr std::uint32_t ces_engine_max_inline_accept_retries = 4U;

static void ces_engine_acceptor_listener_close(ces_engine_acceptor* acceptor) noexcept {
    acceptor->resources->listener.reset();
    acceptor->listener = INVALID_SOCKET;
}

static void ces_engine_connection_socket_close(ces_engine_connection* connection) noexcept {
    connection->owner->resources->connection_sockets[connection->index].reset();
    connection->socket = INVALID_SOCKET;
}

static void ces_engine_accept_socket_close(ces_engine_accept_operation* operation) noexcept {
    operation->owner->resources->operation_sockets[operation->index].reset();
    operation->socket = INVALID_SOCKET;
}

static void ces_engine_owned_socket_close(ces_socket_owner* owner, SOCKET* mirror) noexcept {
    owner->reset();
    *mirror = INVALID_SOCKET;
}

static void ces_engine_report(const wchar_t* stage, int error) noexcept {
    std::fwprintf(stderr, L"%ls failed: native_error=%d\n", stage, error);
}

static void ces_engine_print_final_statistics(ces_protocol                 protocol,
                                              const ces_engine_statistics* statistics,
                                              ULONGLONG                    elapsed_milliseconds,
                                              std::uint32_t                worker_count,
                                              std::uint32_t                terminal_count) noexcept {
    const ULONGLONG guarded_elapsed      = std::max<ULONGLONG>(elapsed_milliseconds, 1U);
    const double    elapsed_seconds      = static_cast<double>(guarded_elapsed) / 1000.0;
    const double    mebibytes_per_second = static_cast<double>(statistics->bytes) / (1024.0 * 1024.0) / elapsed_seconds;
    if (protocol == ces_protocol::tcp) {
        std::fwprintf(stdout,
                      L"final protocol=tcp elapsed_ms=%llu accepted=%llu completions=%llu receives=%llu sends=%llu "
                      L"bytes=%llu MiB_per_sec=%.2f active=%u workers=%u received_bytes=%llu sent_bytes=%llu "
                      L"network_errors=%llu rejected=%llu\n",
                      static_cast<unsigned long long>(elapsed_milliseconds),
                      static_cast<unsigned long long>(statistics->accepted),
                      static_cast<unsigned long long>(statistics->completions),
                      static_cast<unsigned long long>(statistics->receives),
                      static_cast<unsigned long long>(statistics->sends),
                      static_cast<unsigned long long>(statistics->bytes), mebibytes_per_second, terminal_count,
                      worker_count, static_cast<unsigned long long>(statistics->received_bytes),
                      static_cast<unsigned long long>(statistics->sent_bytes),
                      static_cast<unsigned long long>(statistics->network_errors),
                      static_cast<unsigned long long>(statistics->rejected));
        return;
    }
    std::fwprintf(
        stdout,
        L"final protocol=udp elapsed_ms=%llu completions=%llu receives=%llu sends=%llu bytes=%llu "
        L"MiB_per_sec=%.2f outstanding=%u workers=%u received_bytes=%llu sent_bytes=%llu network_errors=%llu "
        L"rejected=%llu\n",
        static_cast<unsigned long long>(elapsed_milliseconds), static_cast<unsigned long long>(statistics->completions),
        static_cast<unsigned long long>(statistics->receives), static_cast<unsigned long long>(statistics->sends),
        static_cast<unsigned long long>(statistics->bytes), mebibytes_per_second, terminal_count, worker_count,
        static_cast<unsigned long long>(statistics->received_bytes),
        static_cast<unsigned long long>(statistics->sent_bytes),
        static_cast<unsigned long long>(statistics->network_errors),
        static_cast<unsigned long long>(statistics->rejected));
}

static bool ces_engine_load_rio(RIO_EXTENSION_FUNCTION_TABLE* table) noexcept {
    ces_socket_owner probe_owner{ WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                             WSA_FLAG_OVERLAPPED | WSA_FLAG_REGISTERED_IO) };
    const SOCKET     probe = probe_owner.get();
    if (probe == INVALID_SOCKET) {
        ces_engine_report(L"WSASocketW(RIO probe)", WSAGetLastError());
        return false;
    }
    GUID  identifier = WSAID_MULTIPLE_RIO;
    DWORD bytes      = 0;
    std::memset(table, 0, sizeof(*table));
    table->cbSize    = sizeof(*table);
    const int status = WSAIoctl(probe, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &identifier, sizeof(identifier),
                                table, sizeof(*table), &bytes, nullptr, nullptr);
    const int error  = status == 0 ? 0 : WSAGetLastError();
    if (status != 0) {
        ces_engine_report(L"SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER(RIO)", error);
        return false;
    }
    return true;
}

static SOCKET ces_engine_registered_socket(int type, int protocol) noexcept {
    return WSASocketW(AF_INET, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_REGISTERED_IO);
}

static bool ces_engine_configure_socket(SOCKET socket_value, const ces_options* options, bool tcp) noexcept {
    if (options->socket_buffer_bytes != 0) {
        const int size = static_cast<int>(options->socket_buffer_bytes);
        if (setsockopt(socket_value, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size)) != 0 ||
            setsockopt(socket_value, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size)) != 0) {
            ces_engine_report(L"setsockopt(SO_SNDBUF/SO_RCVBUF)", WSAGetLastError());
            return false;
        }
    }
    if (tcp) {
        const BOOL enabled = TRUE;
        if (setsockopt(socket_value, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                       sizeof(enabled)) != 0) {
            ces_engine_report(L"setsockopt(TCP_NODELAY)", WSAGetLastError());
            return false;
        }
    }
    return true;
}

static void ces_engine_arm(ces_engine_worker* worker) noexcept {
    if (worker->notification_armed) {
        ces_engine_fail_fast(L"duplicate worker RIONotify", ERROR_INVALID_STATE);
    }
    std::memset(&worker->notification_overlapped, 0, sizeof(worker->notification_overlapped));
    const int status = worker->rio->RIONotify(worker->completion_queue);
    ces_require_rio_notify_success(status, L"RIONotify(worker)");
    if (!ces_notification_mark_rearmed(&worker->notification_armed)) {
        ces_engine_fail_fast(L"notification rearm transition", ERROR_INVALID_STATE);
    }
    ++worker->notify_arms;
}

// Lazy arm: the completion queue is armed exactly while RIO work is outstanding and no notification
// is pending, and every RIO post and drain calls it, so a missed arm surfaces as blocked waits in
// the notification counters rather than as a hang.
static void ces_engine_maybe_arm(ces_engine_worker* worker) noexcept {
    if (!ces_notify_should_arm(worker->notification_armed, worker->rio_outstanding)) {
        return;
    }
    ces_engine_arm(worker);
}

static void ces_engine_release_connection(ces_engine_connection* connection) noexcept {
    ces_engine_worker* worker = connection->owner;
    ces_engine_connection_socket_close(connection);
    connection->request_queue                = RIO_INVALID_RQ;
    connection->active                       = false;
    connection->closing                      = false;
    connection->outstanding                  = 0;
    worker->free_indices[worker->free_count] = connection->index;
    ++worker->free_count;
    --worker->active_count;
    // The slot reservation made by the acceptor is returned exactly here, once the connection has
    // released its RQ and socket, and on the handoff rollback paths in ces_engine_take_socket.
    ces_worker_release_admission_credit(worker);
}

static void ces_engine_close_connection(ces_engine_connection* connection) noexcept {
    if (!connection->active || connection->closing) {
        return;
    }
    connection->closing = true;
    (void) ces_timer_remove(&connection->owner->timers, connection->index);
    ces_engine_connection_socket_close(connection);
    if (connection->outstanding == 0) {
        ces_engine_release_connection(connection);
    }
}

static bool ces_engine_post_receive(ces_engine_connection* connection) noexcept {
    connection->request.operation = ces_engine_operation::receive;
    connection->buffer.Length     = connection->owner->stride;
    if (connection->owner->rio->RIOReceive(connection->request_queue, &connection->buffer, 1, 0,
                                           &connection->request) == FALSE) {
        ces_engine_report(L"RIOReceive", WSAGetLastError());
        ++connection->owner->statistics.network_errors;
        return false;
    }
    ++connection->outstanding;
    ++connection->owner->rio_outstanding;
    ces_engine_maybe_arm(connection->owner);
    connection->deadline =
        GetTickCount64() + static_cast<ULONGLONG>(connection->owner->options->timeout_seconds) * 1000ULL;
    if (!ces_timer_insert_or_update(&connection->owner->timers, connection->index, connection->deadline)) {
        ces_engine_fail_fast(L"ces_timer_insert_or_update(receive)", ERROR_INVALID_DATA);
    }
    return true;
}

static bool ces_engine_post_send(ces_engine_connection* connection) noexcept {
    connection->request.operation = ces_engine_operation::send;
    connection->buffer.Offset =
        connection->index * connection->owner->stride + static_cast<ULONG>(connection->send_offset);
    connection->buffer.Length = static_cast<ULONG>(connection->echo_bytes - connection->send_offset);
    if (connection->owner->rio->RIOSend(connection->request_queue, &connection->buffer, 1, 0, &connection->request) ==
        FALSE) {
        ces_engine_report(L"RIOSend", WSAGetLastError());
        ++connection->owner->statistics.network_errors;
        return false;
    }
    ++connection->outstanding;
    ++connection->owner->rio_outstanding;
    ces_engine_maybe_arm(connection->owner);
    connection->deadline =
        GetTickCount64() + static_cast<ULONGLONG>(connection->owner->options->timeout_seconds) * 1000ULL;
    if (!ces_timer_insert_or_update(&connection->owner->timers, connection->index, connection->deadline)) {
        ces_engine_fail_fast(L"ces_timer_insert_or_update(send)", ERROR_INVALID_DATA);
    }
    return true;
}

static void ces_engine_process_result(ces_engine_worker* worker, const RIORESULT& result) noexcept {
    ces_engine_request* request =
        reinterpret_cast<ces_engine_request*>(static_cast<std::uintptr_t>(result.RequestContext));
    if (request == nullptr || request->connection == nullptr || request->connection->owner != worker) {
        ces_engine_fail_fast(L"worker RIO RequestContext", ERROR_INVALID_DATA);
    }
    ces_engine_connection* connection = request->connection;
    if (connection->outstanding == 0 || worker->rio_outstanding == 0) {
        ces_engine_fail_fast(L"worker RIO outstanding count", ERROR_INVALID_DATA);
    }
    --connection->outstanding;
    --worker->rio_outstanding;
    ces_statistics_record_completion(&worker->statistics, request->operation, result.Status, result.BytesTransferred,
                                     connection->closing);
    if (connection->closing) {
        if (connection->outstanding == 0) {
            ces_engine_release_connection(connection);
        }
        return;
    }
    if (result.Status != ERROR_SUCCESS) {
        ces_engine_close_connection(connection);
        return;
    }

    if (request->operation == ces_engine_operation::receive) {
        if (result.BytesTransferred == 0) {
            ces_engine_close_connection(connection);
            return;
        }
        connection->echo_bytes  = result.BytesTransferred;
        connection->send_offset = 0;
        if (!ces_engine_post_send(connection)) {
            ces_engine_close_connection(connection);
        }
        return;
    }

    if (!ces_advance_offset(connection->echo_bytes, result.BytesTransferred, &connection->send_offset)) {
        ++worker->statistics.network_errors;
        ces_engine_close_connection(connection);
        return;
    }
    if (connection->send_offset < connection->echo_bytes) {
        if (!ces_engine_post_send(connection)) {
            ces_engine_close_connection(connection);
        }
        return;
    }
    connection->buffer.Offset = connection->index * worker->stride;
    if (!ces_engine_post_receive(connection)) {
        ces_engine_close_connection(connection);
    }
}

static void ces_engine_drain_worker(ces_engine_worker* worker) noexcept {
    std::array<RIORESULT, ces_engine_batch_size> results{};
    for (std::uint32_t batch = 0; batch < ces_engine_max_drain_batches; ++batch) {
        const ULONG count =
            ces_require_valid_dequeue_count(worker->rio->RIODequeueCompletion(worker->completion_queue, results.data(),
                                                                              static_cast<ULONG>(results.size())),
                                            L"RIODequeueCompletion(worker)");
        if (count == 0) {
            return;
        }
        for (ULONG index = 0; index < count; ++index) {
            ces_engine_process_result(worker, results[index]);
        }
    }
}

static void ces_engine_ack_accept(ces_engine_accept_operation* operation) noexcept {
    const ULONG_PTR key = static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(operation));
    if (PostQueuedCompletionStatus(operation->accept_port, 0, key, nullptr) == FALSE) {
        ces_engine_fail_fast(L"PostQueuedCompletionStatus(accept ack)", static_cast<int>(GetLastError()));
    }
}

static void ces_engine_take_socket(ces_engine_worker* worker, ces_engine_accept_operation* operation) noexcept {
    ces_socket_owner accepted_owner{ operation->owner->resources->operation_sockets[operation->index].release() };
    const SOCKET     accepted = accepted_owner.get();
    operation->socket         = INVALID_SOCKET;
    if (worker->stopping) {
        ces_worker_release_admission_credit(worker);
        ces_engine_ack_accept(operation);
        return;
    }
    if (worker->free_count == 0) {
        // Defensive: the acceptor reserves a slot before publishing a handoff, so this path only
        // runs if the reservation and the worker pool ever disagree. Return the reservation rather
        // than leaking it, and keep counting the rejection.
        ++worker->statistics.rejected;
        ces_worker_release_admission_credit(worker);
        ces_engine_ack_accept(operation);
        return;
    }
    const std::uint32_t    index      = worker->free_indices[--worker->free_count];
    ces_engine_connection* connection = &worker->connections[index];
    connection->owner                 = worker;
    worker->resources->connection_sockets[index].reset(accepted_owner.release());
    connection->socket             = accepted;
    connection->index              = index;
    connection->active             = true;
    connection->closing            = false;
    connection->outstanding        = 0;
    connection->echo_bytes         = 0;
    connection->send_offset        = 0;
    connection->request.connection = connection;
    connection->request.operation  = ces_engine_operation::receive;
    connection->buffer.BufferId    = worker->registration;
    connection->buffer.Offset      = index * worker->stride;
    connection->buffer.Length      = worker->stride;
    connection->request_queue      = worker->rio->RIOCreateRequestQueue(accepted, 1, 1, 1, 1, worker->completion_queue,
                                                                        worker->completion_queue, connection);
    if (connection->request_queue == RIO_INVALID_RQ) {
        ces_engine_report(L"RIOCreateRequestQueue(TCP)", WSAGetLastError());
        ++worker->statistics.network_errors;
        connection->active                         = false;
        worker->free_indices[worker->free_count++] = index;
        ces_worker_release_admission_credit(worker);
        ces_engine_connection_socket_close(connection);
        ces_engine_ack_accept(operation);
        return;
    }
    ++worker->active_count;
    ++worker->statistics.accepted;
    if (!ces_engine_post_receive(connection)) {
        ces_engine_close_connection(connection);
    }
    ces_engine_ack_accept(operation);
}

static void ces_engine_stop_worker(ces_engine_worker* worker) noexcept {
    worker->stopping = true;
    for (std::uint32_t index = 0; index < worker->slot_count; ++index) {
        if (worker->connections[index].active) {
            ces_engine_close_connection(&worker->connections[index]);
        }
    }
}

static DWORD WINAPI ces_engine_worker_thread(void* parameter) noexcept {
    ces_engine_worker* worker = static_cast<ces_engine_worker*>(parameter);
    ces_engine_maybe_arm(worker);
    worker->ready = true;
    if (SetEvent(worker->ready_event) == FALSE) {
        ces_engine_fail_fast(L"SetEvent(worker ready)", static_cast<int>(GetLastError()));
    }
    for (;;) {
        DWORD       transferred       = 0;
        ULONG_PTR   key               = 0;
        OVERLAPPED* overlapped        = nullptr;
        const DWORD wait_milliseconds = ces_timer_wait_milliseconds(&worker->timers, GetTickCount64());
        const BOOL  ok    = GetQueuedCompletionStatus(worker->port, &transferred, &key, &overlapped, wait_milliseconds);
        const DWORD error = ok == FALSE ? GetLastError() : ERROR_SUCCESS;
        if (overlapped == &worker->notification_overlapped) {
            if (ok == FALSE) {
                ces_engine_fail_fast(L"GetQueuedCompletionStatus(worker notification)", static_cast<int>(error));
            }
            if (!ces_notification_packet_matches(key, overlapped,
                                                 static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(worker)),
                                                 &worker->notification_overlapped)) {
                ces_engine_fail_fast(L"worker RIO notification key", ERROR_INVALID_DATA);
            }
            if (!ces_notification_mark_delivered(&worker->notification_armed)) {
                ces_engine_fail_fast(L"notification delivery transition", ERROR_INVALID_STATE);
            }
            ++worker->notify_deliveries;
            ces_engine_drain_worker(worker);
            // A bounded drain may leave results queued; arming an already nonempty CQ notifies
            // immediately, so the rearm only has to happen while work is still outstanding.
            ces_engine_maybe_arm(worker);
        } else if (overlapped == nullptr && key == ces_engine_stop_key) {
            ces_engine_stop_worker(worker);
        } else if (overlapped == nullptr && key == ces_engine_admission_closed_key) {
            worker->admission_closed = true;
        } else if (overlapped == nullptr && key > ces_engine_admission_closed_key) {
            ces_engine_accept_operation* operation =
                reinterpret_cast<ces_engine_accept_operation*>(static_cast<std::uintptr_t>(key));
            ces_engine_take_socket(worker, operation);
        } else if (ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr) {
            if (!worker->stopping && worker->rio_outstanding != 0 && !worker->notification_armed) {
                ++worker->notify_timeout_wakeups;
            }
        } else if (ok == FALSE && error != WAIT_TIMEOUT) {
            ces_engine_report(L"GetQueuedCompletionStatus(worker)", static_cast<int>(error));
            ++worker->statistics.network_errors;
            worker->failed->store(true, std::memory_order_release);
            ces_engine_stop_worker(worker);
        } else if (!(ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr)) {
            ces_engine_fail_fast(L"unexpected worker IOCP packet", ERROR_INVALID_DATA);
        }

        const ULONGLONG now           = GetTickCount64();
        std::uint32_t   expired_index = 0;
        while (ces_timer_pop_expired(&worker->timers, now, &expired_index)) {
            ces_engine_connection* connection = &worker->connections[expired_index];
            if (connection->active && !connection->closing) {
                ces_engine_close_connection(connection);
            }
        }

        if (worker->stopping && worker->admission_closed && worker->active_count == 0) {
            break;
        }
    }
    // Teardown contract: the loop only returns once every connection has retired all of its RIO
    // work, so the completion queue can be closed without waiting for the last RIONotify delivery;
    // the notification OVERLAPPED stays valid until the IOCP handle is closed in
    // ces_engine_worker_destroy, which is the last object that can reference it. Forging an IOCP
    // packet here would claim a notification that RIO never delivered.
    if (worker->rio_outstanding != 0) {
        ces_engine_fail_fast(L"worker RIO cleanup with outstanding operations", ERROR_IO_INCOMPLETE);
    }
#ifndef NDEBUG
    if (worker->notify_timeout_wakeups != 0) {
        ces_engine_fail_fast(L"worker notification starvation", ERROR_INVALID_STATE);
    }
    if (worker->notify_arms < worker->notify_deliveries || worker->notify_arms - worker->notify_deliveries > 1U) {
        ces_engine_fail_fast(L"worker notification accounting", ERROR_INVALID_STATE);
    }
#endif
    return worker->failed->load(std::memory_order_acquire) ? 1U : 0U;
}

static bool ces_engine_worker_initialize(ces_engine_worker*                  worker,
                                         const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                         const ces_options*                  options,
                                         std::atomic<bool>*                  failed,
                                         std::uint32_t                       worker_index,
                                         std::uint32_t                       worker_count,
                                         ces_engine_worker_resources*        resources) noexcept {
    std::memset(worker, 0, sizeof(*worker));
    worker->resources        = resources;
    worker->completion_queue = RIO_INVALID_CQ;
    worker->registration     = RIO_INVALID_BUFFERID;
    worker->rio              = rio;
    worker->options          = options;
    worker->failed           = failed;
    worker->worker_index     = worker_index;
    worker->stride           = options->rio_buffer_bytes;
    worker->port             = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    resources->port.reset(worker->port);
    worker->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    resources->ready_event.reset(worker->ready_event);
    if (worker->port == nullptr || worker->ready_event == nullptr) {
        ces_engine_report(L"CreateIoCompletionPort(worker)", static_cast<int>(GetLastError()));
        return false;
    }

    const std::uint64_t memory_share   = options->memory_bytes / worker_count;
    const std::uint64_t possible_slots = memory_share / worker->stride;
    if (possible_slots == 0) {
        ces_engine_report(L"worker registered arena capacity", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    worker->slot_count = ces_tcp_connection_capacity(options->cq_capacity, possible_slots);
    if (worker->slot_count == 0) {
        ces_engine_report(L"worker RIO CQ capacity", ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    worker->admission_credit.store(worker->slot_count, std::memory_order_relaxed);
    std::size_t arena_bytes = 0;
    if (!ces_checked_arena_bytes(worker->slot_count, worker->stride, memory_share, &arena_bytes) ||
        arena_bytes > std::numeric_limits<DWORD>::max()) {
        ces_engine_report(L"worker registered arena size", ERROR_ARITHMETIC_OVERFLOW);
        return false;
    }

    worker->memory = static_cast<char*>(VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    resources->arena.reset(worker->memory);
    worker->connections = static_cast<ces_engine_connection*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ces_engine_connection) * worker->slot_count));
    resources->connections.reset(worker->connections);
    worker->free_indices =
        static_cast<std::uint32_t*>(HeapAlloc(GetProcessHeap(), 0, sizeof(std::uint32_t) * worker->slot_count));
    resources->free_indices.reset(worker->free_indices);
    worker->timer_nodes =
        static_cast<ces_timer_node*>(HeapAlloc(GetProcessHeap(), 0, sizeof(ces_timer_node) * worker->slot_count));
    resources->timer_nodes.reset(worker->timer_nodes);
    worker->timer_positions =
        static_cast<std::uint32_t*>(HeapAlloc(GetProcessHeap(), 0, sizeof(std::uint32_t) * worker->slot_count));
    resources->timer_positions.reset(worker->timer_positions);
    resources->connection_sockets.reset(new (std::nothrow) ces_socket_owner[worker->slot_count]);
    if (worker->memory == nullptr || worker->connections == nullptr || worker->free_indices == nullptr ||
        worker->timer_nodes == nullptr || worker->timer_positions == nullptr || !resources->connection_sockets ||
        !ces_timer_initialize(&worker->timers, worker->timer_nodes, worker->timer_positions, worker->slot_count)) {
        ces_engine_report(L"worker allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    worker->registration = rio->RIORegisterBuffer(worker->memory, static_cast<DWORD>(arena_bytes));
    resources->registration.reset(rio, worker->registration);
    if (worker->registration == RIO_INVALID_BUFFERID) {
        ces_engine_report(L"RIORegisterBuffer(worker)", WSAGetLastError());
        return false;
    }

    RIO_NOTIFICATION_COMPLETION notification{};
    notification.Type               = RIO_IOCP_COMPLETION;
    notification.Iocp.IocpHandle    = worker->port;
    notification.Iocp.CompletionKey = worker;
    notification.Iocp.Overlapped    = &worker->notification_overlapped;
    worker->completion_queue        = rio->RIOCreateCompletionQueue(worker->slot_count * 2U, &notification);
    resources->completion_queue.reset(rio, worker->completion_queue);
    if (worker->completion_queue == RIO_INVALID_CQ) {
        ces_engine_report(L"RIOCreateCompletionQueue(worker)", WSAGetLastError());
        return false;
    }
    for (std::uint32_t index = 0; index < worker->slot_count; ++index) {
        worker->free_indices[index]              = worker->slot_count - index - 1U;
        worker->connections[index].socket        = INVALID_SOCKET;
        worker->connections[index].request_queue = RIO_INVALID_RQ;
    }
    worker->free_count = worker->slot_count;
    worker->thread     = CreateThread(nullptr, 0, ces_engine_worker_thread, worker, 0, nullptr);
    resources->thread.reset(worker->thread);
    if (worker->thread == nullptr) {
        ces_engine_report(L"CreateThread(worker)", static_cast<int>(GetLastError()));
        return false;
    }
    if (WaitForSingleObject(worker->ready_event, INFINITE) != WAIT_OBJECT_0 || !worker->ready) {
        ces_engine_report(L"worker startup readiness", ERROR_INVALID_STATE);
        return false;
    }
    return true;
}

static void ces_engine_worker_destroy(ces_engine_worker* worker) noexcept {
    const bool had_ready_worker = worker->thread != nullptr && worker->ready;
    if (worker->thread != nullptr) {
        WaitForSingleObject(worker->thread, INFINITE);
        worker->resources->thread.reset();
        worker->thread = nullptr;
    }
    if (had_ready_worker) {
        if (!worker->stopping || !worker->admission_closed || worker->active_count != 0 ||
            worker->free_count != worker->slot_count ||
            worker->admission_credit.load(std::memory_order_relaxed) != worker->slot_count ||
            worker->rio_outstanding != 0 || worker->timers.size != 0) {
            ces_engine_fail_fast(L"worker release precondition", ERROR_INVALID_STATE);
        }
    }
    if (worker->options != nullptr && worker->options->stats) {
        std::fwprintf(stdout,
                      L"[worker %u] accepted=%llu completions=%llu receives=%llu sends=%llu bytes=%llu active=%u\n",
                      worker->worker_index, static_cast<unsigned long long>(worker->statistics.accepted),
                      static_cast<unsigned long long>(worker->statistics.completions),
                      static_cast<unsigned long long>(worker->statistics.receives),
                      static_cast<unsigned long long>(worker->statistics.sends),
                      static_cast<unsigned long long>(worker->statistics.bytes), worker->active_count);
    }
    worker->resources->completion_queue.reset();
    worker->completion_queue = RIO_INVALID_CQ;
    worker->resources->registration.reset();
    worker->registration = RIO_INVALID_BUFFERID;
    worker->resources->arena.reset();
    worker->memory = nullptr;
    worker->resources->connection_sockets.reset();
    worker->resources->connections.reset();
    worker->connections = nullptr;
    worker->resources->free_indices.reset();
    worker->free_indices = nullptr;
    worker->resources->timer_nodes.reset();
    worker->timer_nodes = nullptr;
    worker->resources->timer_positions.reset();
    worker->timer_positions = nullptr;
    worker->resources->ready_event.reset();
    worker->ready_event = nullptr;
    worker->resources->port.reset();
    worker->port = nullptr;
}

static bool ces_engine_accept_error_is_recoverable(DWORD error) noexcept {
    return error == ERROR_NETNAME_DELETED || error == static_cast<DWORD>(WSAECONNRESET) ||
           error == static_cast<DWORD>(WSAECONNABORTED);
}

static bool ces_engine_defer_accept_repost(ces_engine_acceptor*         acceptor,
                                           ces_engine_accept_operation* operation) noexcept {
    // Keep queued reposts live until their control packet is consumed during shutdown.
    operation->state    = ces_accept_state::deferred;
    const ULONG_PTR key = static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(operation));
    if (PostQueuedCompletionStatus(acceptor->port, 0, key, nullptr) == FALSE) {
        ces_engine_fail_fast(L"PostQueuedCompletionStatus(deferred AcceptEx repost)", static_cast<int>(GetLastError()));
    }
    return true;
}

static bool ces_engine_post_accept(ces_engine_acceptor* acceptor, ces_engine_accept_operation* operation) noexcept {
    if (operation->state != ces_accept_state::idle || operation->socket != INVALID_SOCKET) {
        ces_engine_fail_fast(L"AcceptEx repost precondition", ERROR_INVALID_STATE);
    }
    for (std::uint32_t retry = 0; retry < ces_engine_max_inline_accept_retries; ++retry) {
        if (acceptor->stopping || acceptor->failed->load(std::memory_order_acquire) ||
            acceptor->listener == INVALID_SOCKET) {
            return true;
        }
        std::memset(&operation->overlapped, 0, sizeof(operation->overlapped));
        operation->socket = ces_engine_registered_socket(SOCK_STREAM, IPPROTO_TCP);
        acceptor->resources->operation_sockets[operation->index].reset(operation->socket);
        if (operation->socket == INVALID_SOCKET) {
            ces_engine_report(L"WSASocketW(accepted RIO socket)", WSAGetLastError());
            ++acceptor->network_errors;
            return false;
        }
        DWORD received      = 0;
        operation->state    = ces_accept_state::posted;
        const BOOL accepted = acceptor->accept_ex(acceptor->listener, operation->socket, operation->addresses.data(), 0,
                                                  sizeof(SOCKADDR_STORAGE) + 16U, sizeof(SOCKADDR_STORAGE) + 16U,
                                                  &received, &operation->overlapped);
        if (accepted != FALSE) {
            return true;
        }
        const DWORD error = static_cast<DWORD>(WSAGetLastError());
        if (error == ERROR_IO_PENDING) {
            return true;
        }
        operation->state = ces_accept_state::idle;
        ces_engine_accept_socket_close(operation);
        ++acceptor->network_errors;
        if (ces_engine_accept_error_is_recoverable(error)) {
            continue;
        }
        ces_engine_report(L"AcceptEx", static_cast<int>(error));
        return false;
    }
    return ces_engine_defer_accept_repost(acceptor, operation);
}

static void ces_engine_stop_acceptor(ces_engine_acceptor* acceptor) noexcept {
    acceptor->stopping = true;
    ces_engine_acceptor_listener_close(acceptor);
    for (std::uint32_t index = 0; index < acceptor->operation_count; ++index) {
        if (acceptor->operations[index].state == ces_accept_state::posted) {
            // Cancellation still owns the OVERLAPPED until its IOCP completion arrives.
            ces_engine_accept_socket_close(&acceptor->operations[index]);
        }
    }
}

static bool ces_engine_acceptor_has_live(const ces_engine_acceptor* acceptor) noexcept {
    for (std::uint32_t index = 0; index < acceptor->operation_count; ++index) {
        if (acceptor->operations[index].state != ces_accept_state::idle) {
            return true;
        }
    }
    return false;
}

static DWORD WINAPI ces_engine_acceptor_thread(void* parameter) noexcept {
    ces_engine_acceptor* acceptor = static_cast<ces_engine_acceptor*>(parameter);
    for (std::uint32_t index = 0; index < acceptor->operation_count; ++index) {
        if (!ces_engine_post_accept(acceptor, &acceptor->operations[index])) {
            acceptor->failed->store(true, std::memory_order_release);
            ces_engine_stop_acceptor(acceptor);
            break;
        }
    }

    while (!acceptor->stopping || ces_engine_acceptor_has_live(acceptor)) {
        if (!acceptor->stopping && acceptor->failed->load(std::memory_order_acquire)) {
            ces_engine_stop_acceptor(acceptor);
            if (!ces_engine_acceptor_has_live(acceptor)) {
                break;
            }
        }
        DWORD       transferred = 0;
        ULONG_PTR   key         = 0;
        OVERLAPPED* overlapped  = nullptr;
        const BOOL  ok          = GetQueuedCompletionStatus(acceptor->port, &transferred, &key, &overlapped, 100);
        const DWORD wait_error  = ok == FALSE ? GetLastError() : ERROR_SUCCESS;
        if (overlapped == nullptr && key == ces_engine_stop_key) {
            ces_engine_stop_acceptor(acceptor);
            continue;
        }
        if (overlapped == nullptr && key > ces_engine_stop_key) {
            ces_engine_accept_operation* operation =
                reinterpret_cast<ces_engine_accept_operation*>(static_cast<std::uintptr_t>(key));
            if (operation->state != ces_accept_state::transit && operation->state != ces_accept_state::deferred) {
                ces_engine_fail_fast(L"AcceptEx repost packet state", ERROR_INVALID_STATE);
            }
            operation->state = ces_accept_state::idle;
            if (!acceptor->stopping && !ces_engine_post_accept(acceptor, operation)) {
                acceptor->failed->store(true, std::memory_order_release);
                ces_engine_stop_acceptor(acceptor);
            }
            continue;
        }
        if (overlapped != nullptr) {
            ces_engine_accept_operation* operation = reinterpret_cast<ces_engine_accept_operation*>(overlapped);
            if (operation->state != ces_accept_state::posted) {
                ces_engine_fail_fast(L"AcceptEx completion state", ERROR_INVALID_STATE);
            }
            if (ok == FALSE) {
                operation->state = ces_accept_state::idle;
                ces_engine_accept_socket_close(operation);
                if (acceptor->stopping || acceptor->failed->load(std::memory_order_acquire)) {
                    continue;
                }
                ++acceptor->network_errors;
                if (ces_engine_accept_error_is_recoverable(wait_error)) {
                    if (!ces_engine_post_accept(acceptor, operation)) {
                        acceptor->failed->store(true, std::memory_order_release);
                        ces_engine_stop_acceptor(acceptor);
                    }
                    continue;
                }
                ces_engine_report(L"AcceptEx completion", static_cast<int>(wait_error));
                acceptor->failed->store(true, std::memory_order_release);
                ces_engine_stop_acceptor(acceptor);
                continue;
            }
            if (acceptor->stopping || acceptor->failed->load(std::memory_order_acquire) ||
                acceptor->listener == INVALID_SOCKET || operation->socket == INVALID_SOCKET) {
                operation->state = ces_accept_state::idle;
                ces_engine_accept_socket_close(operation);
                continue;
            }
            if (setsockopt(operation->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                           reinterpret_cast<const char*>(&acceptor->listener), sizeof(acceptor->listener)) != 0 ||
                !ces_engine_configure_socket(operation->socket, acceptor->options, true)) {
                ++acceptor->network_errors;
                operation->state = ces_accept_state::idle;
                ces_engine_accept_socket_close(operation);
                acceptor->failed->store(true, std::memory_order_release);
                ces_engine_stop_acceptor(acceptor);
                continue;
            }
            SOCKADDR* local_address  = nullptr;
            SOCKADDR* remote_address = nullptr;
            int       local_length   = 0;
            int       remote_length  = 0;
            acceptor->get_accept_addresses(operation->addresses.data(), 0, sizeof(SOCKADDR_STORAGE) + 16U,
                                           sizeof(SOCKADDR_STORAGE) + 16U, &local_address, &local_length,
                                           &remote_address, &remote_length);
            if (local_address == nullptr || remote_address == nullptr || local_length <= 0 || remote_length <= 0) {
                ces_engine_report(L"GetAcceptExSockaddrs", WSAEINVAL);
                operation->state = ces_accept_state::idle;
                ces_engine_accept_socket_close(operation);
                acceptor->failed->store(true, std::memory_order_release);
                ces_engine_stop_acceptor(acceptor);
                continue;
            }
            // Reserve a slot on the target worker before publishing the handoff, so a connection is
            // never accepted and then rejected while another worker still has capacity.
            ces_engine_worker* worker = ces_acceptor_select_worker(acceptor);
            if (worker == nullptr) {
                // Every worker has reserved all of its slots, so the server really is at capacity:
                // withdraw this accept socket and repost the accept slot.
                ++acceptor->rejected;
                operation->state = ces_accept_state::idle;
                ces_engine_accept_socket_close(operation);
                if (!ces_engine_post_accept(acceptor, operation)) {
                    acceptor->failed->store(true, std::memory_order_release);
                    ces_engine_stop_acceptor(acceptor);
                }
                continue;
            }
            operation->state            = ces_accept_state::transit;
            const ULONG_PTR handoff_key = static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(operation));
            if (PostQueuedCompletionStatus(worker->port, 0, handoff_key, nullptr) == FALSE) {
                ces_engine_fail_fast(L"PostQueuedCompletionStatus(accept handoff)", static_cast<int>(GetLastError()));
            }
            continue;
        }
        if (ok == FALSE && wait_error != WAIT_TIMEOUT) {
            ces_engine_report(L"GetQueuedCompletionStatus(acceptor)", static_cast<int>(wait_error));
            ++acceptor->network_errors;
            acceptor->failed->store(true, std::memory_order_release);
            ces_engine_stop_acceptor(acceptor);
        }
    }
    return acceptor->failed->load(std::memory_order_acquire) ? 1U : 0U;
}

static bool ces_engine_acceptor_initialize(ces_engine_acceptor*                acceptor,
                                           const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                           const ces_options*                  options,
                                           ces_engine_worker*                  workers,
                                           std::uint32_t                       worker_count,
                                           std::atomic<bool>*                  failed,
                                           ces_engine_acceptor_resources*      resources) noexcept {
    std::memset(acceptor, 0, sizeof(*acceptor));
    acceptor->resources    = resources;
    acceptor->rio          = rio;
    acceptor->options      = options;
    acceptor->workers      = workers;
    acceptor->worker_count = worker_count;
    acceptor->failed       = failed;
    acceptor->listener     = ces_engine_registered_socket(SOCK_STREAM, IPPROTO_TCP);
    resources->listener.reset(acceptor->listener);
    acceptor->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    resources->port.reset(acceptor->port);
    if (acceptor->listener == INVALID_SOCKET || acceptor->port == nullptr) {
        ces_engine_report(L"TCP listener/IOCP creation", WSAGetLastError());
        ++acceptor->network_errors;
        return false;
    }
    if (!ces_engine_configure_socket(acceptor->listener, options, true)) {
        ++acceptor->network_errors;
        return false;
    }
    SOCKADDR_IN address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port        = htons(options->port);
    if (bind(acceptor->listener, reinterpret_cast<const SOCKADDR*>(&address), sizeof(address)) != 0 ||
        listen(acceptor->listener, SOMAXCONN) != 0) {
        ces_engine_report(L"bind/listen", WSAGetLastError());
        ++acceptor->network_errors;
        return false;
    }
    if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(acceptor->listener), acceptor->port, 0, 1) != acceptor->port) {
        ces_engine_report(L"CreateIoCompletionPort(listener association)", static_cast<int>(GetLastError()));
        ++acceptor->network_errors;
        return false;
    }
    GUID  accept_identifier  = WSAID_ACCEPTEX;
    GUID  address_identifier = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD bytes              = 0;
    if (WSAIoctl(acceptor->listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_identifier, sizeof(accept_identifier),
                 &acceptor->accept_ex, sizeof(acceptor->accept_ex), &bytes, nullptr, nullptr) != 0 ||
        acceptor->accept_ex == nullptr) {
        ces_engine_report(L"SIO_GET_EXTENSION_FUNCTION_POINTER(AcceptEx)", WSAGetLastError());
        ++acceptor->network_errors;
        return false;
    }
    bytes = 0;
    if (WSAIoctl(acceptor->listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &address_identifier,
                 sizeof(address_identifier), &acceptor->get_accept_addresses, sizeof(acceptor->get_accept_addresses),
                 &bytes, nullptr, nullptr) != 0 ||
        acceptor->get_accept_addresses == nullptr) {
        ces_engine_report(L"SIO_GET_EXTENSION_FUNCTION_POINTER(GetAcceptExSockaddrs)", WSAGetLastError());
        ++acceptor->network_errors;
        return false;
    }
    acceptor->operation_count = ces_accept_operation_count(worker_count);
    acceptor->operations      = static_cast<ces_engine_accept_operation*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ces_engine_accept_operation) * acceptor->operation_count));
    resources->operations.reset(acceptor->operations);
    resources->operation_sockets.reset(new (std::nothrow) ces_socket_owner[acceptor->operation_count]);
    if (acceptor->operations == nullptr || !resources->operation_sockets) {
        ces_engine_report(L"AcceptEx operation allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    for (std::uint32_t index = 0; index < acceptor->operation_count; ++index) {
        acceptor->operations[index].owner       = acceptor;
        acceptor->operations[index].socket      = INVALID_SOCKET;
        acceptor->operations[index].accept_port = acceptor->port;
        acceptor->operations[index].state       = ces_accept_state::idle;
        acceptor->operations[index].index       = index;
    }
    acceptor->thread = CreateThread(nullptr, 0, ces_engine_acceptor_thread, acceptor, 0, nullptr);
    resources->thread.reset(acceptor->thread);
    if (acceptor->thread == nullptr) {
        ces_engine_report(L"CreateThread(acceptor)", static_cast<int>(GetLastError()));
        return false;
    }
    return true;
}

static void ces_engine_acceptor_destroy(ces_engine_acceptor* acceptor) noexcept {
    if (acceptor->thread != nullptr) {
        WaitForSingleObject(acceptor->thread, INFINITE);
        acceptor->resources->thread.reset();
        acceptor->thread = nullptr;
    }
    acceptor->resources->listener.reset();
    acceptor->listener = INVALID_SOCKET;
    if (acceptor->operations != nullptr) {
        if (acceptor->resources->operation_sockets) {
            for (std::uint32_t index = 0; index < acceptor->operation_count; ++index) {
                ces_engine_accept_socket_close(&acceptor->operations[index]);
            }
        }
        acceptor->resources->operation_sockets.reset();
        acceptor->resources->operations.reset();
        acceptor->operations = nullptr;
    }
    acceptor->resources->port.reset();
    acceptor->port = nullptr;
}

static ces_exit_code ces_engine_run_tcp(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                        const ces_options*                  options,
                                        std::atomic<bool>*                  stop_requested) noexcept {
    std::uint32_t worker_count = options->worker_count;
    if (worker_count == 0) {
        worker_count =
            std::clamp(static_cast<std::uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), 1U, CES_MAX_WORKERS);
    }
    ces_engine_worker* workers = static_cast<ces_engine_worker*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ces_engine_worker) * worker_count));
    ces_heap_owner                                 workers_owner{ workers };
    std::unique_ptr<ces_engine_worker_resources[]> worker_resources{ new (std::nothrow)
                                                                         ces_engine_worker_resources[worker_count] };
    if (workers == nullptr || !worker_resources) {
        ces_engine_report(L"worker array allocation", ERROR_NOT_ENOUGH_MEMORY);
        return ces_exit_code::network;
    }
    std::atomic<bool> failed{ false };
    std::uint32_t     initialized = 0;
    for (; initialized < worker_count; ++initialized) {
        if (!ces_engine_worker_initialize(&workers[initialized], rio, options, &failed, initialized, worker_count,
                                          &worker_resources[initialized])) {
            failed.store(true, std::memory_order_release);
            ces_engine_worker_destroy(&workers[initialized]);
            break;
        }
    }

    ces_engine_acceptor_resources acceptor_resources{};
    ces_engine_acceptor           acceptor{};
    acceptor.resources    = &acceptor_resources;
    acceptor.listener     = INVALID_SOCKET;
    bool acceptor_started = false;
    if (!failed.load(std::memory_order_acquire)) {
        acceptor_started = ces_engine_acceptor_initialize(&acceptor, rio, options, workers, worker_count, &failed,
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
        if (PostQueuedCompletionStatus(acceptor.port, 0, ces_engine_stop_key, nullptr) == FALSE) {
            ces_engine_fail_fast(L"PostQueuedCompletionStatus(acceptor stop)", static_cast<int>(GetLastError()));
        }
    }
    ces_engine_acceptor_destroy(&acceptor);
    for (std::uint32_t index = 0; index < initialized; ++index) {
        if (PostQueuedCompletionStatus(workers[index].port, 0, ces_engine_admission_closed_key, nullptr) == FALSE ||
            PostQueuedCompletionStatus(workers[index].port, 0, ces_engine_stop_key, nullptr) == FALSE) {
            ces_engine_fail_fast(L"PostQueuedCompletionStatus(worker shutdown)", static_cast<int>(GetLastError()));
        }
    }
    ces_engine_statistics statistics{};
    statistics.network_errors = acceptor.network_errors;
    statistics.rejected       = acceptor.rejected;
    for (std::uint32_t index = 0; index < initialized; ++index) {
        ces_engine_worker_destroy(&workers[index]);
        ces_statistics_add(&statistics, &workers[index].statistics);
    }
    if (options->stats) {
        ces_engine_print_final_statistics(ces_protocol::tcp, &statistics, GetTickCount64() - start, worker_count, 0);
    }
    return failed.load(std::memory_order_acquire) ? ces_exit_code::network : ces_exit_code::success;
}

static void ces_engine_udp_arm(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                               RIO_CQ                              queue,
                               OVERLAPPED*                         overlapped,
                               bool*                               armed) noexcept {
    if (*armed) {
        ces_engine_fail_fast(L"duplicate UDP RIONotify", ERROR_INVALID_STATE);
    }
    std::memset(overlapped, 0, sizeof(*overlapped));
    const int status = rio->RIONotify(queue);
    ces_require_rio_notify_success(status, L"RIONotify(UDP)");
    if (!ces_notification_mark_rearmed(armed)) {
        ces_engine_fail_fast(L"UDP notification rearm transition", ERROR_INVALID_STATE);
    }
}

static ces_exit_code ces_engine_run_udp(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                        const ces_options*                  options,
                                        std::atomic<bool>*                  stop_requested) noexcept {
    const std::uint32_t stride      = options->rio_buffer_bytes + CES_UDP_ADDRESS_BYTES;
    std::size_t         arena_bytes = 0;
    if (options->udp_depth > options->cq_capacity / 2U) {
        ces_engine_report(L"UDP CQ capacity", ERROR_INSUFFICIENT_BUFFER);
        return ces_exit_code::usage;
    }
    if (!ces_checked_arena_bytes(options->udp_depth, stride, options->memory_bytes, &arena_bytes) ||
        arena_bytes > std::numeric_limits<DWORD>::max()) {
        ces_engine_report(L"UDP registered arena", ERROR_NOT_ENOUGH_MEMORY);
        return ces_exit_code::usage;
    }

    ces_socket_owner        socket_owner{ ces_engine_registered_socket(SOCK_DGRAM, IPPROTO_UDP) };
    SOCKET                  socket_value = socket_owner.get();
    ces_handle_owner        port_owner{ CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1) };
    HANDLE                  port = port_owner.get();
    ces_virtual_arena_owner arena_owner{ VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) };
    char*                   memory = static_cast<char*>(arena_owner.get());
    ces_engine_udp_slot*    slots  = static_cast<ces_engine_udp_slot*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ces_engine_udp_slot) * options->udp_depth));
    ces_heap_owner             slots_owner{ slots };
    ces_rio_registration_owner registration_owner{};
    ces_rio_cq_owner           completion_queue_owner{};
    RIO_BUFFERID               registration     = RIO_INVALID_BUFFERID;
    RIO_CQ                     completion_queue = RIO_INVALID_CQ;
    RIO_RQ                     request_queue    = RIO_INVALID_RQ;
    OVERLAPPED                 notification_overlapped{};
    bool                       armed       = false;
    std::uint32_t              outstanding = 0;
    bool                       failed      = false;
    ces_engine_statistics      statistics{};

    if (socket_value == INVALID_SOCKET || port == nullptr || memory == nullptr || slots == nullptr) {
        ces_engine_report(L"UDP runtime allocation", WSAGetLastError());
        if (socket_value == INVALID_SOCKET) {
            ++statistics.network_errors;
        }
        failed = true;
    }
    if (!failed && !ces_engine_configure_socket(socket_value, options, false)) {
        ++statistics.network_errors;
        failed = true;
    }
    if (!failed) {
        SOCKADDR_IN address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port        = htons(options->port);
        if (bind(socket_value, reinterpret_cast<const SOCKADDR*>(&address), sizeof(address)) != 0) {
            ces_engine_report(L"bind(UDP)", WSAGetLastError());
            ++statistics.network_errors;
            failed = true;
        }
    }
    if (!failed) {
        registration = rio->RIORegisterBuffer(memory, static_cast<DWORD>(arena_bytes));
        registration_owner.reset(rio, registration);
        RIO_NOTIFICATION_COMPLETION notification{};
        notification.Type               = RIO_IOCP_COMPLETION;
        notification.Iocp.IocpHandle    = port;
        notification.Iocp.CompletionKey = slots;
        notification.Iocp.Overlapped    = &notification_overlapped;
        completion_queue                = rio->RIOCreateCompletionQueue(options->cq_capacity, &notification);
        completion_queue_owner.reset(rio, completion_queue);
        if (registration == RIO_INVALID_BUFFERID || completion_queue == RIO_INVALID_CQ) {
            ces_engine_report(L"UDP RIO buffer/CQ creation", WSAGetLastError());
            ++statistics.network_errors;
            failed = true;
        }
    }
    if (!failed) {
        request_queue = rio->RIOCreateRequestQueue(socket_value, options->udp_depth, 1, options->udp_depth, 1,
                                                   completion_queue, completion_queue, slots);
        if (request_queue == RIO_INVALID_RQ) {
            ces_engine_report(L"RIOCreateRequestQueue(UDP)", WSAGetLastError());
            ++statistics.network_errors;
            failed = true;
        }
    }
    if (!failed) {
        for (std::uint32_t index = 0; index < options->udp_depth; ++index) {
            slots[index].payload = RIO_BUF{ registration, index * stride, options->rio_buffer_bytes };
            slots[index].remote_address =
                RIO_BUF{ registration, index * stride + options->rio_buffer_bytes, CES_UDP_ADDRESS_BYTES };
            slots[index].operation   = ces_engine_operation::receive;
            slots[index].outstanding = true;
            if (rio->RIOReceiveEx(request_queue, &slots[index].payload, 1, nullptr, &slots[index].remote_address,
                                  nullptr, nullptr, 0, &slots[index]) == FALSE) {
                ces_engine_report(L"RIOReceiveEx(UDP)", WSAGetLastError());
                ++statistics.network_errors;
                slots[index].outstanding = false;
                failed                   = true;
                break;
            }
            ++outstanding;
        }
        if (ces_notify_should_arm(armed, outstanding)) {
            ces_engine_udp_arm(rio, completion_queue, &notification_overlapped, &armed);
        }
    }

    const ULONGLONG start   = GetTickCount64();
    bool            closing = failed;
    if (closing && socket_value != INVALID_SOCKET) {
        ces_engine_owned_socket_close(&socket_owner, &socket_value);
    }
    std::array<RIORESULT, ces_engine_batch_size> results{};
    while (!closing || outstanding != 0) {
        if (!closing && (stop_requested->load(std::memory_order_acquire) ||
                         (options->run_seconds != 0 && GetTickCount64() - start >= options->run_seconds * 1000ULL))) {
            closing = true;
            ces_engine_owned_socket_close(&socket_owner, &socket_value);
        }
        DWORD       transferred = 0;
        ULONG_PTR   key         = 0;
        OVERLAPPED* overlapped  = nullptr;
        const BOOL  ok          = GetQueuedCompletionStatus(port, &transferred, &key, &overlapped, 100);
        const DWORD error       = ok == FALSE ? GetLastError() : ERROR_SUCCESS;
        if (overlapped == &notification_overlapped) {
            if (ok == FALSE) {
                ces_engine_fail_fast(L"GetQueuedCompletionStatus(UDP notification)", static_cast<int>(error));
            }
            if (!ces_notification_packet_matches(key, overlapped,
                                                 static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(slots)),
                                                 &notification_overlapped)) {
                ces_engine_fail_fast(L"UDP RIO notification key", ERROR_INVALID_DATA);
            }
            if (!ces_notification_mark_delivered(&armed)) {
                ces_engine_fail_fast(L"UDP notification delivery transition", ERROR_INVALID_STATE);
            }
            for (std::uint32_t batch = 0; batch < ces_engine_max_drain_batches; ++batch) {
                const ULONG count = ces_require_valid_dequeue_count(
                    rio->RIODequeueCompletion(completion_queue, results.data(), static_cast<ULONG>(results.size())),
                    L"RIODequeueCompletion(UDP)");
                if (count == 0) {
                    break;
                }
                for (ULONG result_index = 0; result_index < count; ++result_index) {
                    ces_engine_udp_slot* slot = reinterpret_cast<ces_engine_udp_slot*>(
                        static_cast<std::uintptr_t>(results[result_index].RequestContext));
                    if (slot == nullptr || !slot->outstanding || outstanding == 0) {
                        ces_engine_fail_fast(L"UDP completion invariant", ERROR_INVALID_DATA);
                    }
                    slot->outstanding = false;
                    --outstanding;
                    ces_statistics_record_completion(&statistics, slot->operation, results[result_index].Status,
                                                     results[result_index].BytesTransferred, closing);
                    if (closing) {
                        continue;
                    }
                    if (results[result_index].Status != ERROR_SUCCESS) {
                        if (results[result_index].Status == WSAECONNRESET) {
                            slot->payload.Length = options->rio_buffer_bytes;
                            slot->operation      = ces_engine_operation::receive;
                        } else {
                            ces_engine_report(L"UDP RIO completion", static_cast<int>(results[result_index].Status));
                            failed  = true;
                            closing = true;
                            ces_engine_owned_socket_close(&socket_owner, &socket_value);
                            continue;
                        }
                    } else if (slot->operation == ces_engine_operation::receive) {
                        slot->payload.Length = results[result_index].BytesTransferred;
                        slot->operation      = ces_engine_operation::send;
                    } else {
                        slot->payload.Length = options->rio_buffer_bytes;
                        slot->operation      = ces_engine_operation::receive;
                    }

                    BOOL posted = FALSE;
                    if (slot->operation == ces_engine_operation::send) {
                        posted = rio->RIOSendEx(request_queue, &slot->payload, 1, nullptr, &slot->remote_address,
                                                nullptr, nullptr, 0, slot);
                    } else {
                        posted = rio->RIOReceiveEx(request_queue, &slot->payload, 1, nullptr, &slot->remote_address,
                                                   nullptr, nullptr, 0, slot);
                    }
                    if (posted == FALSE) {
                        ces_engine_report(L"UDP RIO repost", WSAGetLastError());
                        ++statistics.network_errors;
                        failed  = true;
                        closing = true;
                        ces_engine_owned_socket_close(&socket_owner, &socket_value);
                        continue;
                    }
                    slot->outstanding = true;
                    ++outstanding;
                }
            }
            if (ces_notify_should_arm(armed, outstanding)) {
                ces_engine_udp_arm(rio, completion_queue, &notification_overlapped, &armed);
            }
        } else if (ok == FALSE && error != WAIT_TIMEOUT) {
            ces_engine_fail_fast(L"GetQueuedCompletionStatus(UDP)", static_cast<int>(error));
        } else if (!(ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr)) {
            ces_engine_fail_fast(L"unexpected UDP IOCP packet", ERROR_INVALID_DATA);
        }
    }

    if (failed && socket_value != INVALID_SOCKET) {
        ces_engine_owned_socket_close(&socket_owner, &socket_value);
    }
    // Teardown contract: after this point no new RIO request is posted, every published operation
    // has already retired, and only then is the completion queue closed. RIOCloseCompletionQueue
    // invalidates the queue and silently drops completions that would have been added afterwards,
    // so outstanding must be zero here and no accounting may depend on a later completion. A
    // pending RIONotify delivery is deliberately not awaited: the notification OVERLAPPED stays
    // valid until the IOCP handle is closed at the end of this function, which is the last object
    // that can reference it.
    if (outstanding != 0) {
        ces_engine_fail_fast(L"UDP cleanup with outstanding operations", ERROR_IO_INCOMPLETE);
    }
    if (options->stats) {
        ces_engine_print_final_statistics(ces_protocol::udp, &statistics, GetTickCount64() - start, 1U, outstanding);
    }
    completion_queue_owner.reset();
    completion_queue = RIO_INVALID_CQ;
    registration_owner.reset();
    registration = RIO_INVALID_BUFFERID;
    arena_owner.reset();
    memory = nullptr;
    slots_owner.reset();
    slots = nullptr;
    ces_engine_owned_socket_close(&socket_owner, &socket_value);
    port_owner.reset();
    port = nullptr;
    return failed ? ces_exit_code::network : ces_exit_code::success;
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
