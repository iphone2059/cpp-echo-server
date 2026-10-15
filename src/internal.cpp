module;
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#if !defined(CES_PURE_ENGINE_ONLY)
// WinSock2 must precede the Windows networking headers.
// clang-format off
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
// clang-format on
#endif

export module ces.internal;

import ces.types;
import ces.engine;
#if !defined(CES_PURE_ENGINE_ONLY)
import ces.contract;
import ces.native;
#endif

export struct ces_udp_slot_state {
    ces_engine_operation operation;
    std::uint32_t        length;
    bool                 outstanding;
};

export struct ces_udp_engine {
    ces_udp_slot_state*   slots;
    std::uint32_t         depth;
    std::uint32_t         payload_bytes;
    std::uint32_t         outstanding;
    ces_udp_phase         phase;
    bool                  closing;
    bool                  failed;
    ces_engine_statistics statistics;
};

export struct ces_accept_slot {
    ces_accept_state state;
};
export enum class ces_accept_action_kind : std::uint8_t { withdraw, repost, handoff, stop, fatal };

export struct ces_accept_action {
    ces_accept_action_kind kind;
    ces_failure            failure;
};

export struct ces_acceptor_core {
    ces_accept_slot* slots;
    std::uint32_t    operation_count;
    std::uint32_t    next_worker;
    std::uint64_t    network_errors;
    std::uint64_t    rejected;
    bool             stopping;
};

export bool       ces_udp_initialize(ces_udp_engine*     engine,
                                     ces_udp_slot_state* slots,
                                     std::uint32_t       depth,
                                     std::uint32_t       payload_bytes) noexcept;
export bool       ces_udp_mark_posted(ces_udp_engine* engine, std::uint32_t index) noexcept;
export ces_action ces_udp_on_event(ces_udp_engine* engine, const ces_event& event) noexcept;
export void       ces_udp_begin_drain(ces_udp_engine* engine) noexcept;

export ces_handoff_disposition ces_acceptor_handoff_disposition(bool stopping,
                                                                bool failed,
                                                                bool listener_valid,
                                                                bool accepted_valid) noexcept;
export bool                    ces_accept_error_is_recoverable(std::uint32_t error) noexcept;
export bool                    ces_acceptor_has_live(const ces_acceptor_core* core) noexcept;
export bool                    ces_acceptor_mark_posted(ces_accept_slot* slot) noexcept;
export bool                    ces_acceptor_retire_post(ces_accept_slot* slot) noexcept;
export bool                    ces_acceptor_mark_transit(ces_accept_slot* slot) noexcept;
export bool                    ces_acceptor_defer_repost(ces_accept_slot* slot) noexcept;
export bool                    ces_acceptor_ack(ces_accept_slot* slot) noexcept;
export void                    ces_acceptor_stop(ces_acceptor_core* core) noexcept;
export ces_accept_action       ces_acceptor_on_completion(ces_acceptor_core* core,
                                                          std::uint32_t      index,
                                                          bool               completed,
                                                          std::uint32_t      error,
                                                          bool               failed,
                                                          bool               listener_valid,
                                                          bool               accepted_valid) noexcept;
export std::uint32_t           ces_accept_operation_count(std::uint32_t worker_count) noexcept;

// Static Runtime dispatch is shared by production and the Native-free contract driver.
export template <class Transport>
ces_action ces_runtime_tcp_dispatch_event(ces_tcp_engine*  engine,
                                          const ces_event& event,
                                          std::uint64_t    now,
                                          Transport&       transport) noexcept {
    const ces_action action = ces_tcp_on_event(engine, event, now);
    transport.execute(action);
    return action;
}
export struct ces_native_worker;
export struct ces_native_acceptor;
export class ces_engine_worker_resources;
export class ces_engine_acceptor_resources;
export struct ces_engine_accept_operation;
export struct _RIO_EXTENSION_FUNCTION_TABLE;

export struct ces_engine_worker {
    std::unique_ptr<ces_native_worker> transport;
    std::unique_ptr<ces_tcp_engine>    core;
    std::atomic<std::uint32_t>         admission_credit{ 0 };
    const ces_options*                 options      = nullptr;
    std::atomic<bool>*                 failed       = nullptr;
    std::uint32_t                      worker_index = 0;
    bool                               ready        = false;
    std::unique_ptr<ces_connection[]>  connection_states;
    std::unique_ptr<std::uint32_t[]>   free_index_storage;
    std::unique_ptr<ces_timer_node[]>  timer_node_storage;
    std::unique_ptr<std::uint32_t[]>   timer_position_storage;
    std::uint32_t*                     free_indices    = nullptr;
    ces_timer_node*                    timer_nodes     = nullptr;
    std::uint32_t*                     timer_positions = nullptr;
    ces_engine_worker() noexcept;
    ~ces_engine_worker() noexcept;
};

export struct ces_engine_acceptor {
    std::unique_ptr<ces_native_acceptor> transport;
    ces_acceptor_core                    core{};
    ces_engine_worker*                   workers        = nullptr;
    const ces_options*                   options        = nullptr;
    std::atomic<bool>*                   failed         = nullptr;
    std::uint32_t                        worker_count   = 0;
    std::uint64_t                        drain_deadline = 0;
    std::unique_ptr<ces_accept_slot[]>   operation_states;
    ces_engine_acceptor() noexcept;
    ~ces_engine_acceptor() noexcept;
};

// Worker Runtime is the only bridge between Native provider ownership and pure decisions.
export bool ces_engine_worker_initialize(ces_engine_worker*                   worker,
                                         const _RIO_EXTENSION_FUNCTION_TABLE* rio,
                                         const ces_options*                   options,
                                         std::atomic<bool>*                   failed,
                                         std::uint32_t                        worker_index,
                                         std::uint32_t                        worker_count,
                                         ces_engine_worker_resources*         resources) noexcept;
export void ces_engine_worker_join(ces_engine_worker* worker) noexcept;
export void ces_engine_worker_require_release(const ces_engine_worker* worker) noexcept;
export void ces_engine_worker_destroy(ces_engine_worker* worker) noexcept;
export bool ces_engine_acceptor_initialize(ces_engine_acceptor*                 acceptor,
                                           const _RIO_EXTENSION_FUNCTION_TABLE* rio,
                                           const ces_options*                   options,
                                           ces_engine_worker*                   workers,
                                           std::uint32_t                        worker_count,
                                           std::atomic<bool>*                   failed,
                                           ces_engine_acceptor_resources*       resources) noexcept;
export void ces_engine_acceptor_join(ces_engine_acceptor* acceptor) noexcept;
export void ces_engine_acceptor_require_release(const ces_engine_acceptor* acceptor) noexcept;
export void ces_engine_acceptor_destroy(ces_engine_acceptor* acceptor) noexcept;

// The Worker Runtime publishes its own final ledger for the UDP loop. Declaring it here keeps the
// pure/Native runtime free of an import of the coordinator module, which imports this one.
export void          ces_engine_print_final_statistics(ces_protocol                 protocol,
                                                       const ces_engine_statistics* statistics,
                                                       std::uint64_t                elapsed_milliseconds,
                                                       std::uint32_t                worker_count,
                                                       std::uint32_t                terminal_count) noexcept;
export ces_exit_code ces_engine_run_udp(const _RIO_EXTENSION_FUNCTION_TABLE* rio,
                                        const ces_options*                   options,
                                        std::atomic<bool>*                   stop_requested) noexcept;

export bool               ces_worker_try_reserve_admission(ces_engine_worker* worker) noexcept;
export void               ces_worker_release_admission_credit(ces_engine_worker* worker) noexcept;
export ces_engine_worker* ces_acceptor_select_worker(ces_engine_acceptor* acceptor) noexcept;

export bool ces_engine_accept_error_is_recoverable(std::uint32_t error) noexcept;
export bool ces_engine_post_accept(ces_engine_acceptor* acceptor, ces_engine_accept_operation* operation) noexcept;
export unsigned long __stdcall ces_engine_acceptor_thread(void* parameter) noexcept;
export ces_handoff_disposition ces_engine_require_handoff_disposition(bool stopping,
                                                                      bool failed,
                                                                      bool listener_valid,
                                                                      bool accepted_valid) noexcept;

ces_handoff_disposition ces_acceptor_handoff_disposition(bool stopping,
                                                         bool failed,
                                                         bool listener_valid,
                                                         bool accepted_valid) noexcept {
    if (stopping || failed || !listener_valid) {
        return ces_handoff_disposition::withdraw;
    }
    return accepted_valid ? ces_handoff_disposition::handoff : ces_handoff_disposition::fatal;
}

std::uint32_t ces_accept_operation_count(std::uint32_t worker_count) noexcept {
    constexpr std::uint32_t minimum    = 8U;
    constexpr std::uint32_t maximum    = 128U;
    constexpr std::uint32_t per_worker = 2U;
    return worker_count >= maximum / per_worker ? maximum : std::max(minimum, worker_count * per_worker);
}

bool ces_accept_error_is_recoverable(std::uint32_t error) noexcept {
    return error == 64U || error == 10054U || error == 10053U;
}

bool ces_acceptor_has_live(const ces_acceptor_core* core) noexcept {
    for (std::uint32_t index = 0; index < core->operation_count; ++index) {
        if (core->slots[index].state != ces_accept_state::idle) {
            return true;
        }
    }
    return false;
}

bool ces_acceptor_mark_posted(ces_accept_slot* slot) noexcept {
    if (slot->state != ces_accept_state::idle) {
        return false;
    }
    slot->state = ces_accept_state::posted;
    return true;
}

bool ces_acceptor_retire_post(ces_accept_slot* slot) noexcept {
    if (slot->state != ces_accept_state::posted) {
        return false;
    }
    slot->state = ces_accept_state::idle;
    return true;
}

bool ces_acceptor_mark_transit(ces_accept_slot* slot) noexcept {
    if (slot->state != ces_accept_state::idle) {
        return false;
    }
    slot->state = ces_accept_state::transit;
    return true;
}

bool ces_acceptor_defer_repost(ces_accept_slot* slot) noexcept {
    if (slot->state != ces_accept_state::idle) {
        return false;
    }
    slot->state = ces_accept_state::deferred;
    return true;
}

bool ces_acceptor_ack(ces_accept_slot* slot) noexcept {
    if (slot->state != ces_accept_state::transit && slot->state != ces_accept_state::deferred) {
        return false;
    }
    slot->state = ces_accept_state::idle;
    return true;
}

void ces_acceptor_stop(ces_acceptor_core* core) noexcept {
    core->stopping = true;
}

ces_accept_action ces_acceptor_on_completion(ces_acceptor_core* core,
                                             std::uint32_t      index,
                                             bool               completed,
                                             std::uint32_t      error,
                                             bool               failed,
                                             bool               listener_valid,
                                             bool               accepted_valid) noexcept {
    if (index >= core->operation_count || !ces_acceptor_retire_post(&core->slots[index])) {
        return {
            ces_accept_action_kind::fatal, { L"AcceptEx completion state", 5023 }
        };
    }
    if (!completed) {
        if (core->stopping || failed) {
            return { ces_accept_action_kind::withdraw, {} };
        }
        ++core->network_errors;
        return ces_accept_error_is_recoverable(error) ?
                   ces_accept_action{
                       ces_accept_action_kind::repost, {}
        } :
                   ces_accept_action{ ces_accept_action_kind::stop,
                                      { L"AcceptEx completion", static_cast<int>(error) } };
    }
    const ces_handoff_disposition disposition =
        ces_acceptor_handoff_disposition(core->stopping, failed, listener_valid, accepted_valid);
    if (disposition == ces_handoff_disposition::fatal) {
        return {
            ces_accept_action_kind::fatal, { L"AcceptEx handoff invariant", 10038 }
        };
    }
    return { disposition == ces_handoff_disposition::withdraw ? ces_accept_action_kind::withdraw :
                                                                ces_accept_action_kind::handoff,
             {} };
}

bool ces_udp_initialize(ces_udp_engine*     engine,
                        ces_udp_slot_state* slots,
                        std::uint32_t       depth,
                        std::uint32_t       payload_bytes) noexcept {
    if (engine == nullptr || slots == nullptr || depth == 0) {
        return false;
    }
    engine->slots         = slots;
    engine->depth         = depth;
    engine->payload_bytes = payload_bytes;
    engine->phase         = ces_udp_phase::running;
    for (std::uint32_t index = 0; index < depth; ++index) {
        slots[index] = { ces_engine_operation::receive, payload_bytes, false };
    }
    return true;
}

bool ces_udp_mark_posted(ces_udp_engine* engine, std::uint32_t index) noexcept {
    if (engine->closing || index >= engine->depth || engine->slots[index].outstanding ||
        engine->outstanding >= engine->depth) {
        return false;
    }
    engine->slots[index].outstanding = true;
    ++engine->outstanding;
    return true;
}

void ces_udp_begin_drain(ces_udp_engine* engine) noexcept {
    engine->closing = true;
    engine->phase   = ces_udp_phase::draining;
}

ces_action ces_udp_on_event(ces_udp_engine* engine, const ces_event& event) noexcept {
    const std::uint32_t index = event.completion.index;
    if (event.kind == ces_event_kind::stop) {
        ces_udp_begin_drain(engine);
        return { ces_action_kind::close, 0, 0, 0, {} };
    }
    if (index >= engine->depth) {
        return {
            ces_action_kind::fatal, index, 0, 0, { L"UDP completion invariant", 13 }
        };
    }
    ces_udp_slot_state* slot = &engine->slots[index];
    if (event.kind == ces_event_kind::post_failed) {
        ++engine->statistics.network_errors;
        engine->failed = true;
        ces_udp_begin_drain(engine);
        return { ces_action_kind::close, index, 0, 0, event.failure };
    }
    if (event.kind != ces_event_kind::completion || !slot->outstanding || engine->outstanding == 0 ||
        slot->operation != event.completion.operation) {
        return {
            ces_action_kind::fatal, index, 0, 0, { L"UDP completion invariant", 13 }
        };
    }
    slot->outstanding = false;
    --engine->outstanding;
    ces_statistics_record_completion(&engine->statistics, slot->operation, event.completion.status,
                                     event.completion.bytes, engine->closing);
    if (engine->closing) {
        return { ces_action_kind::none, index, 0, 0, {} };
    }
    if (event.completion.status != 0 && event.completion.status != 10054) {
        engine->failed = true;
        ces_udp_begin_drain(engine);
        return {
            ces_action_kind::close, index, 0, 0, { L"UDP RIO completion", event.completion.status }
        };
    }
    if (event.completion.status == 10054 || slot->operation == ces_engine_operation::send) {
        slot->operation = ces_engine_operation::receive;
        slot->length    = engine->payload_bytes;
    } else {
        slot->operation = ces_engine_operation::send;
        slot->length    = event.completion.bytes;
    }
    return { slot->operation == ces_engine_operation::send ? ces_action_kind::send : ces_action_kind::receive,
             index,
             0,
             slot->length,
             {} };
}

#if !defined(CES_PURE_ENGINE_ONLY)
static void ces_engine_arm(ces_engine_worker* worker) noexcept {
    // Ownership check first: a still-armed notification may own the dedicated OVERLAPPED, so the
    // wrapper refuses before it touches that storage. The shared transaction keeps its own check as a
    // second line of defence.
    if (worker->transport->notification_armed) {
        ces_engine_fail_fast(L"duplicate worker RIONotify", ERROR_INVALID_STATE);
    }
    std::memset(&worker->transport->notification_overlapped, 0, sizeof(worker->transport->notification_overlapped));
    ces_notification_arm(worker->transport->rio, worker->transport->completion_queue,
                         &worker->transport->notification_armed, &worker->transport->notify_arms, L"RIONotify(worker)");
}

static void ces_engine_maybe_arm(ces_engine_worker* worker) noexcept {
    if (!ces_notify_should_arm(worker->transport->notification_armed, worker->transport->rio_outstanding)) {
        return;
    }
    ces_engine_arm(worker);
}

static void ces_engine_execute(ces_engine_worker* worker, ces_action action) noexcept {
    for (;;) {
        if (action.kind == ces_action_kind::none) {
            return;
        }
        if (action.kind == ces_action_kind::fatal) {
            ces_engine_fail_fast(action.failure.stage, action.failure.native_error);
        }
        if (action.index >= worker->transport->slot_count) {
            ces_engine_fail_fast(L"TCP action index", ERROR_INVALID_DATA);
        }
        ces_engine_connection* connection = &worker->transport->connections[action.index];
        if (action.kind == ces_action_kind::close || action.kind == ces_action_kind::release) {
            if (action.kind == ces_action_kind::release && connection->outstanding != 0) {
                ces_engine_fail_fast(L"TCP connection release precondition", ERROR_IO_INCOMPLETE);
            }
            ces_engine_connection_socket_close(connection);
            if (action.kind == ces_action_kind::release) {
                if (connection->outstanding != 0) {
                    ces_engine_fail_fast(L"TCP connection release precondition", ERROR_IO_INCOMPLETE);
                }
                connection->request_queue = RIO_INVALID_RQ;
                ces_worker_release_admission_credit(worker);
            }
            return;
        }
        ces_failure failure{};
        if (ces_native_tcp_post(worker->transport.get(), connection, action, &failure)) {
            ces_engine_maybe_arm(worker);
            return;
        }
        ces_engine_report(failure.stage, failure.native_error);
        action = ces_tcp_on_event(
            worker->core.get(),
            {
                ces_event_kind::post_failed, { action.index, connection->request.operation, 0, 0 },
                 failure
        },
            GetTickCount64());
    }
}

class ces_tcp_native_action_transport {
  public:
    explicit ces_tcp_native_action_transport(ces_engine_worker* worker) noexcept : worker_(worker) {}

    void execute(const ces_action& action) noexcept { ces_engine_execute(worker_, action); }

  private:
    ces_engine_worker* worker_;
};

static void ces_engine_process_result(ces_engine_worker* worker, const RIORESULT& result) noexcept {
    const ces_completion            completion = ces_native_tcp_completion(worker->transport.get(), result);
    ces_tcp_native_action_transport transport{ worker };
    (void) ces_runtime_tcp_dispatch_event(worker->core.get(), { ces_event_kind::completion, completion, {} },
                                          GetTickCount64(), transport);
}

static void ces_engine_drain_worker(
    ces_engine_worker* worker,
    std::uint32_t      maximum_batches = ces_engine_max_drain_batches,
    std::uint32_t      maximum_results = std::numeric_limits<std::uint32_t>::max()) noexcept {
    std::array<RIORESULT, ces_engine_batch_size> results{};
    for (std::uint32_t batch = 0; batch < maximum_batches && maximum_results != 0; ++batch) {
        const ULONG requested = std::min<ULONG>(static_cast<ULONG>(results.size()), maximum_results);
        const ULONG count =
            ces_require_valid_dequeue_count(worker->transport->rio->RIODequeueCompletion(
                                                worker->transport->completion_queue, results.data(), requested),
                                            L"RIODequeueCompletion(worker)");
        if (count > requested) {
            ces_engine_fail_fast(L"worker RIO dequeue extent", ERROR_INVALID_DATA);
        }
        if (count == 0) {
            return;
        }
        maximum_results -= count;
        for (ULONG index = 0; index < count; ++index) {
            ces_engine_process_result(worker, results[index]);
        }
    }
}

static void ces_engine_ack_accept(ces_engine_accept_operation* operation) noexcept {
    const ULONG_PTR key = static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(operation));
    if (!operation->control_pending) {
        ces_engine_fail_fast(L"AcceptEx handoff acknowledgement", ERROR_INVALID_STATE);
    }
    if (PostQueuedCompletionStatus(operation->accept_port, 0, key, nullptr) == FALSE) {
        ces_engine_fail_fast(L"PostQueuedCompletionStatus(accept ack)", static_cast<int>(GetLastError()));
    }
}

static void ces_engine_take_socket(ces_engine_worker* worker, ces_engine_accept_operation* operation) noexcept {
    ces_socket_owner accepted_owner{ operation->owner->resources->operation_sockets[operation->index].release() };
    const SOCKET     accepted = accepted_owner.get();
    operation->socket         = INVALID_SOCKET;
    if (worker->core->stopping) {
        ces_worker_release_admission_credit(worker);
        ces_engine_ack_accept(operation);
        return;
    }
    if (worker->core->free_count == 0) {
        // The acceptor reserved a credit before publishing this handoff, so the worker must still hold
        // a free slot: reaching here means the credit and the free pool have drifted apart. That is an
        // invariant failure rather than capacity pressure, and reporting it as a rejection would both
        // hide the drift and corrupt the rejected count.
        ces_worker_release_admission_credit(worker);
        ces_engine_ack_accept(operation);
        ces_engine_fail_fast(L"admission credit/free pool mismatch", ERROR_INVALID_STATE);
    }
    std::uint32_t index = 0;
    if (!ces_tcp_take_slot(worker->core.get(), &index)) {
        ces_engine_fail_fast(L"admission credit/free pool mismatch", ERROR_INVALID_STATE);
    }
    ces_engine_connection* connection = &worker->transport->connections[index];
    connection->owner                 = worker->transport.get();
    worker->transport->resources->connection_sockets[index].reset(accepted_owner.release());
    connection->socket             = accepted;
    connection->index              = index;
    connection->outstanding        = 0;
    connection->request.connection = connection;
    connection->request.operation  = ces_engine_operation::receive;
    connection->buffer.BufferId    = worker->transport->resources->registrations[index].get();
    connection->buffer.Offset      = 0;
    connection->buffer.Length      = worker->transport->stride;
    connection->request_queue      = worker->transport->rio->RIOCreateRequestQueue(
        accepted, 1, 1, 1, 1, worker->transport->completion_queue, worker->transport->completion_queue, connection);
    if (connection->request_queue == RIO_INVALID_RQ) {
        ces_engine_report(L"RIOCreateRequestQueue(TCP)", WSAGetLastError());
        ++worker->core->statistics.network_errors;
        if (!ces_tcp_return_slot(worker->core.get(), index)) {
            ces_engine_fail_fast(L"TCP free list", ERROR_INVALID_DATA);
        }
        ces_worker_release_admission_credit(worker);
        ces_engine_connection_socket_close(connection);
        ces_engine_ack_accept(operation);
        return;
    }
    ces_engine_execute(worker, ces_tcp_admit(worker->core.get(), index, GetTickCount64()));
    ces_engine_ack_accept(operation);
}

static void ces_engine_stop_worker(ces_engine_worker* worker) noexcept {
    (void) ces_tcp_on_event(worker->core.get(), { ces_event_kind::stop, {}, {} }, GetTickCount64());
    for (std::uint32_t index = 0; index < worker->transport->slot_count; ++index) {
        if (ces_connection_is_active(&worker->core->connections[index])) {
            ces_engine_execute(worker, ces_tcp_close(worker->core.get(), index, GetTickCount64()));
        }
    }
}

static DWORD WINAPI ces_engine_worker_thread(void* parameter) noexcept {
    ces_engine_worker* worker = static_cast<ces_engine_worker*>(parameter);
    ces_engine_maybe_arm(worker);
    worker->core->phase = ces_worker_phase::running;
    worker->ready       = true;
    if (SetEvent(worker->transport->ready_event) == FALSE) {
        ces_engine_fail_fast(L"SetEvent(worker ready)", static_cast<int>(GetLastError()));
    }
    std::uint32_t expired_timer_packets = 0;
    for (;;) {
        DWORD       transferred       = 0;
        ULONG_PTR   key               = 0;
        OVERLAPPED* overlapped        = nullptr;
        const DWORD wait_milliseconds = ces_timer_wait_milliseconds(&worker->core->timers, GetTickCount64());
        const BOOL  ok =
            GetQueuedCompletionStatus(worker->transport->port, &transferred, &key, &overlapped, wait_milliseconds);
        const DWORD error = ok == FALSE ? GetLastError() : ERROR_SUCCESS;
        if (overlapped == &worker->transport->notification_overlapped) {
            if (ok == FALSE) {
                ces_engine_fail_fast(L"GetQueuedCompletionStatus(worker notification)", static_cast<int>(error));
            }
            if (!ces_notification_packet_matches(key, overlapped,
                                                 static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(worker)),
                                                 &worker->transport->notification_overlapped)) {
                ces_engine_fail_fast(L"worker RIO notification key", ERROR_INVALID_DATA);
            }
            if (!ces_notification_mark_delivered(&worker->transport->notification_armed)) {
                ces_engine_fail_fast(L"notification delivery transition", ERROR_INVALID_STATE);
            }
            ++worker->transport->notify_deliveries;
            ces_engine_drain_worker(worker);
            // A bounded drain may leave results queued; arming an already nonempty CQ notifies
            // immediately, so the rearm only has to happen while work is still outstanding.
            ces_engine_maybe_arm(worker);
        } else if (overlapped == nullptr && key == ces_engine_stop_key) {
            ces_engine_stop_worker(worker);
        } else if (overlapped == nullptr && key == ces_engine_admission_closed_key) {
            (void) ces_tcp_on_event(worker->core.get(), { ces_event_kind::admission_closed, {}, {} }, GetTickCount64());
        } else if (overlapped == nullptr && key > ces_engine_admission_closed_key) {
            ces_engine_accept_operation* operation =
                reinterpret_cast<ces_engine_accept_operation*>(static_cast<std::uintptr_t>(key));
            ces_engine_take_socket(worker, operation);
        } else if (ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr) {
            if (!worker->core->stopping && worker->transport->rio_outstanding != 0 &&
                !worker->transport->notification_armed) {
                ++worker->transport->notify_timeout_wakeups;
            }
        } else if (ok == FALSE && error != WAIT_TIMEOUT) {
            ces_engine_report(L"GetQueuedCompletionStatus(worker)", static_cast<int>(error));
            ++worker->core->statistics.network_errors;
            worker->failed->store(true, std::memory_order_release);
            ces_engine_stop_worker(worker);
        } else if (!(ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr)) {
            ces_engine_fail_fast(L"unexpected worker IOCP packet", ERROR_INVALID_DATA);
        }

        const ULONGLONG now = GetTickCount64();
        if (ces_timer_wait_milliseconds(&worker->core->timers, now) == 0) {
            // Include the bounded accept handoffs and both shutdown control packets as well as
            // the connections' RIO notification work before classifying an expired drain.
            const bool          queued_packet = ok != FALSE || overlapped != nullptr || key != 0;
            const std::uint32_t packet_budget =
                worker->transport->slot_count + ces_accept_operation_count(CES_MAX_WORKERS) + 2U;
            if (queued_packet && expired_timer_packets < packet_budget) {
                ++expired_timer_packets;
                continue;
            }
            // An empty IOCP does not prove an empty CQ. Each connection owns at most one RIO
            // operation, and even a one-result dequeue retires one of those queued records.
            ces_engine_drain_worker(worker, worker->transport->slot_count, worker->transport->slot_count);
            ces_engine_maybe_arm(worker);
        }
        expired_timer_packets       = 0;
        std::uint32_t expired_index = 0;
        while (ces_timer_pop_expired(&worker->core->timers, now, &expired_index)) {
            const ces_event event{
                ces_event_kind::timeout, { expired_index, ces_engine_operation::receive, 0, 0 },
                 {}
            };
            ces_engine_execute(worker, ces_tcp_on_event(worker->core.get(), event, now));
        }

        if (ces_tcp_may_exit(worker->core.get())) {
            break;
        }
    }
    // Teardown contract: the loop only returns once every connection has retired all of its RIO
    // work, so the completion queue can be closed without waiting for the last RIONotify delivery;
    // the notification OVERLAPPED stays valid until the IOCP handle is closed in
    // ces_engine_worker_destroy, which is the last object that can reference it. Forging an IOCP
    // packet here would claim a notification that RIO never delivered.
    // Release invariants are checked after the coordinator has joined every worker.
    worker->core->phase = ces_worker_phase::stopped;
    return worker->failed->load(std::memory_order_acquire) ? 1U : 0U;
}

bool ces_engine_worker_initialize(ces_engine_worker*                  worker,
                                  const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                  const ces_options*                  options,
                                  std::atomic<bool>*                  failed,
                                  std::uint32_t                       worker_index,
                                  std::uint32_t                       worker_count,
                                  ces_engine_worker_resources*        resources) noexcept {
    if (!worker->transport || !worker->core) {
        ces_engine_report(L"worker allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    worker->transport->resources        = resources;
    resources->lifetime                 = worker->transport.get();
    worker->transport->completion_queue = RIO_INVALID_CQ;
    worker->transport->rio              = rio;
    worker->options                     = options;
    worker->failed                      = failed;
    worker->worker_index                = worker_index;
    worker->transport->stride           = options->rio_buffer_bytes;
    worker->transport->port             = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    const DWORD port_error              = worker->transport->port == nullptr ? GetLastError() : ERROR_SUCCESS;
    resources->port.reset(worker->transport->port);
    worker->transport->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const DWORD event_error        = worker->transport->ready_event == nullptr ? GetLastError() : ERROR_SUCCESS;
    resources->ready_event.reset(worker->transport->ready_event);
    if (worker->transport->port == nullptr) {
        ces_engine_report(L"CreateIoCompletionPort(worker)", static_cast<int>(port_error));
        return false;
    }
    if (worker->transport->ready_event == nullptr) {
        ces_engine_report(L"CreateEventW(worker ready)", static_cast<int>(event_error));
        return false;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::uint64_t page = system_info.dwPageSize == 0U ? 4096ULL : system_info.dwPageSize;
    const std::uint64_t memory_share =
        ces_worker_memory_budget(options->memory_bytes, worker_count, worker_index, page);
    const std::uint64_t possible_slots = memory_share / worker->transport->stride;
    if (possible_slots == 0) {
        ces_engine_report(L"worker registered arena capacity", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    worker->transport->slot_count = ces_tcp_connection_capacity(options->cq_capacity, possible_slots);
    if (worker->transport->slot_count == 0) {
        ces_engine_report(L"worker RIO CQ capacity", ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    worker->admission_credit.store(worker->transport->slot_count, std::memory_order_relaxed);
    std::size_t arena_bytes = 0;
    if (!ces_checked_arena_bytes(worker->transport->slot_count, worker->transport->stride, memory_share,
                                 &arena_bytes) ||
        arena_bytes > std::numeric_limits<DWORD>::max()) {
        ces_engine_report(L"worker registered arena size", ERROR_ARITHMETIC_OVERFLOW);
        return false;
    }

    worker->transport->memory =
        static_cast<char*>(VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const DWORD arena_error = worker->transport->memory == nullptr ? GetLastError() : ERROR_SUCCESS;
    resources->arena.reset(worker->transport->memory);
    worker->transport->connections = static_cast<ces_engine_connection*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ces_engine_connection) * worker->transport->slot_count));
    resources->connections.reset(worker->transport->connections);
    worker->connection_states.reset(new (std::nothrow) ces_connection[worker->transport->slot_count]{});
    ces_connection* states = worker->connection_states.get();
    worker->free_index_storage.reset(new (std::nothrow) std::uint32_t[worker->transport->slot_count]);
    worker->free_indices = worker->free_index_storage.get();
    worker->timer_node_storage.reset(new (std::nothrow) ces_timer_node[worker->transport->slot_count]);
    worker->timer_nodes = worker->timer_node_storage.get();
    worker->timer_position_storage.reset(new (std::nothrow) std::uint32_t[worker->transport->slot_count]);
    worker->timer_positions = worker->timer_position_storage.get();
    resources->connection_sockets.reset(new (std::nothrow) ces_socket_owner[worker->transport->slot_count]);
    if (worker->transport->memory == nullptr) {
        ces_engine_report(L"VirtualAlloc(worker arena)", static_cast<int>(arena_error));
        return false;
    }
    if (worker->transport->connections == nullptr || worker->free_indices == nullptr ||
        worker->timer_nodes == nullptr || worker->timer_positions == nullptr || states == nullptr ||
        !resources->connection_sockets ||
        !ces_tcp_initialize(worker->core.get(), states, worker->free_indices, worker->timer_nodes,
                            worker->timer_positions, worker->transport->slot_count,
                            static_cast<std::uint64_t>(options->timeout_seconds) * 1000ULL)) {
        ces_engine_report(L"worker allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    resources->registrations.reset(new (std::nothrow) ces_rio_registration_owner[worker->transport->slot_count]);
    if (!resources->registrations) {
        ces_engine_report(L"worker registration owners", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    if (!ces_register_windows(rio, worker->transport->memory, worker->transport->stride, worker->transport->slot_count,
                              resources->registrations.get())) {
        ces_engine_report(L"RIORegisterBuffer(worker slots)", WSAGetLastError());
        return false;
    }

    RIO_NOTIFICATION_COMPLETION notification{};
    notification.Type               = RIO_IOCP_COMPLETION;
    notification.Iocp.IocpHandle    = worker->transport->port;
    notification.Iocp.CompletionKey = worker;
    notification.Iocp.Overlapped    = &worker->transport->notification_overlapped;
    worker->transport->completion_queue =
        rio->RIOCreateCompletionQueue(worker->transport->slot_count * 2U, &notification);
    resources->completion_queue.reset(rio, worker->transport->completion_queue);
    if (worker->transport->completion_queue == RIO_INVALID_CQ) {
        ces_engine_report(L"RIOCreateCompletionQueue(worker)", WSAGetLastError());
        return false;
    }
    for (std::uint32_t index = 0; index < worker->transport->slot_count; ++index) {
        worker->transport->connections[index].index         = index;
        worker->transport->connections[index].owner         = worker->transport.get();
        worker->transport->connections[index].socket        = INVALID_SOCKET;
        worker->transport->connections[index].request_queue = RIO_INVALID_RQ;
    }
    worker->transport->thread = CreateThread(nullptr, 0, ces_engine_worker_thread, worker, 0, nullptr);
    resources->thread.reset(worker->transport->thread);
    if (worker->transport->thread == nullptr) {
        ces_engine_report(L"CreateThread(worker)", static_cast<int>(GetLastError()));
        return false;
    }
    if (WaitForSingleObject(worker->transport->ready_event, INFINITE) != WAIT_OBJECT_0 || !worker->ready) {
        ces_engine_report(L"worker startup readiness", ERROR_INVALID_STATE);
        return false;
    }
    return true;
}

void ces_engine_worker_join(ces_engine_worker* worker) noexcept {
    if (worker->transport != nullptr && worker->transport->thread != nullptr &&
        WaitForSingleObject(worker->transport->thread, INFINITE) != WAIT_OBJECT_0) {
        ces_engine_fail_fast(L"WaitForSingleObject(server worker)", static_cast<int>(GetLastError()));
    }
}

void ces_engine_worker_require_release(const ces_engine_worker* worker) noexcept {
    if (worker->transport != nullptr) {
        ces_native_worker_require_release(worker->transport.get());
    }
    if ((worker->transport != nullptr && worker->transport->rio_outstanding != 0) ||
        (worker->core != nullptr && worker->core->outstanding != 0)) {
        ces_engine_fail_fast(L"worker release precondition", ERROR_IO_INCOMPLETE);
    }
    if (worker->transport != nullptr && worker->transport->connections != nullptr) {
        for (std::uint32_t index = 0; index < worker->transport->slot_count; ++index) {
            if (worker->transport->connections[index].outstanding != 0 ||
                (worker->core != nullptr && worker->core->connections != nullptr &&
                 worker->core->connections[index].outstanding != 0)) {
                ces_engine_fail_fast(L"worker release precondition", ERROR_IO_INCOMPLETE);
            }
        }
    }
    if (worker->transport != nullptr && worker->ready) {
        if (!worker->core->stopping || !worker->core->admission_closed || worker->core->active_count != 0 ||
            worker->core->free_count != worker->transport->slot_count ||
            worker->admission_credit.load(std::memory_order_relaxed) != worker->transport->slot_count ||
            worker->core->timers.size != 0) {
            ces_engine_fail_fast(L"worker release precondition", ERROR_INVALID_STATE);
        }
#    ifndef NDEBUG
        if (worker->transport->notify_timeout_wakeups != 0) {
            ces_engine_fail_fast(L"worker notification starvation", ERROR_INVALID_STATE);
        }
        if (worker->transport->notify_arms < worker->transport->notify_deliveries ||
            worker->transport->notify_arms - worker->transport->notify_deliveries > 1U) {
            ces_engine_fail_fast(L"worker notification accounting", ERROR_INVALID_STATE);
        }
#    endif
    }
}

void ces_engine_worker_destroy(ces_engine_worker* worker) noexcept {
    ces_engine_worker_join(worker);
    ces_engine_worker_require_release(worker);
    if (worker->transport == nullptr || worker->transport->resources == nullptr) {
        return;
    }
    if (worker->transport->thread != nullptr) {
        worker->transport->resources->thread.reset();
        worker->transport->thread = nullptr;
    }
    worker->transport->resources->completion_queue.reset();
    worker->transport->completion_queue = RIO_INVALID_CQ;
    worker->transport->resources->registrations.reset();
    worker->transport->resources->arena.reset();
    worker->transport->memory = nullptr;
    worker->transport->resources->connection_sockets.reset();
    worker->transport->resources->connections.reset();
    worker->transport->connections = nullptr;
    worker->connection_states.reset();
    if (worker->core != nullptr) {
        worker->core->connections = nullptr;
    }
    worker->free_index_storage.reset();
    worker->free_indices = nullptr;
    worker->timer_node_storage.reset();
    worker->timer_nodes = nullptr;
    worker->timer_position_storage.reset();
    worker->timer_positions = nullptr;
    worker->transport->resources->ready_event.reset();
    worker->transport->ready_event = nullptr;
    worker->transport->resources->port.reset();
    worker->transport->port                = nullptr;
    worker->transport->resources->lifetime = nullptr;
    worker->transport->resources           = nullptr;
    if (worker->core != nullptr) {
        worker->core->free_indices = nullptr;
        worker->core->timers       = {};
    }
}

bool ces_engine_accept_error_is_recoverable(std::uint32_t error) noexcept {
    return ces_accept_error_is_recoverable(error);
}

static bool ces_engine_defer_accept_repost(ces_engine_acceptor*         acceptor,
                                           ces_engine_accept_operation* operation) noexcept {
    // Keep queued reposts live until their control packet is consumed during shutdown.
    if (!ces_acceptor_defer_repost(&acceptor->core.slots[operation->index])) {
        ces_engine_fail_fast(L"AcceptEx defer state", ERROR_INVALID_STATE);
    }
    const ULONG_PTR key        = static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(operation));
    operation->control_pending = true;
    if (PostQueuedCompletionStatus(acceptor->transport->port, 0, key, nullptr) == FALSE) {
        ces_engine_fail_fast(L"PostQueuedCompletionStatus(deferred AcceptEx repost)", static_cast<int>(GetLastError()));
    }
    return true;
}

bool ces_engine_post_accept(ces_engine_acceptor* acceptor, ces_engine_accept_operation* operation) noexcept {
    if (acceptor->core.slots[operation->index].state != ces_accept_state::idle || operation->socket != INVALID_SOCKET ||
        operation->posted) {
        ces_engine_fail_fast(L"AcceptEx repost precondition", ERROR_INVALID_STATE);
    }
    for (std::uint32_t retry = 0; retry < ces_engine_max_inline_accept_retries; ++retry) {
        if (acceptor->core.stopping || acceptor->failed->load(std::memory_order_acquire) ||
            acceptor->transport->listener == INVALID_SOCKET) {
            return true;
        }
        std::memset(&operation->overlapped, 0, sizeof(operation->overlapped));
        operation->socket = ces_engine_registered_socket(SOCK_STREAM, IPPROTO_TCP);
        acceptor->transport->resources->operation_sockets[operation->index].reset(operation->socket);
        if (operation->socket == INVALID_SOCKET) {
            ces_engine_report(L"WSASocketW(accepted RIO socket)", WSAGetLastError());
            ++acceptor->core.network_errors;
            return false;
        }
        DWORD received = 0;
        if (!ces_acceptor_mark_posted(&acceptor->core.slots[operation->index])) {
            ces_engine_fail_fast(L"AcceptEx posted state", ERROR_INVALID_STATE);
        }
        operation->posted   = true;
        const BOOL accepted = acceptor->transport->accept_ex(
            acceptor->transport->listener, operation->socket, operation->addresses.data(), 0,
            sizeof(SOCKADDR_STORAGE) + 16U, sizeof(SOCKADDR_STORAGE) + 16U, &received, &operation->overlapped);
        if (accepted != FALSE) {
            return true;
        }
        const DWORD error = static_cast<DWORD>(WSAGetLastError());
        if (error == ERROR_IO_PENDING) {
            return true;
        }
        operation->posted = false;
        if (!ces_acceptor_retire_post(&acceptor->core.slots[operation->index])) {
            ces_engine_fail_fast(L"AcceptEx post rollback", ERROR_INVALID_STATE);
        }
        ces_engine_accept_socket_close(operation);
        ++acceptor->core.network_errors;
        if (ces_engine_accept_error_is_recoverable(error)) {
            continue;
        }
        ces_engine_report(L"AcceptEx", static_cast<int>(error));
        return false;
    }
    return ces_engine_defer_accept_repost(acceptor, operation);
}

static void ces_engine_stop_acceptor(ces_engine_acceptor* acceptor) noexcept {
    if (acceptor->core.stopping) {
        return;
    }
    ces_acceptor_stop(&acceptor->core);
    acceptor->drain_deadline = GetTickCount64() + ces_engine_drain_grace_ms;
    ces_engine_acceptor_listener_close(acceptor->transport.get());
    for (std::uint32_t index = 0; index < acceptor->transport->operation_count; ++index) {
        if (acceptor->core.slots[index].state == ces_accept_state::posted) {
            // Cancellation still owns the OVERLAPPED until its IOCP completion arrives.
            ces_engine_accept_socket_close(&acceptor->transport->operations[index]);
        }
    }
}

static bool ces_engine_acceptor_has_live(const ces_engine_acceptor* acceptor) noexcept {
    return ces_acceptor_has_live(&acceptor->core);
}

static ces_engine_accept_operation* ces_engine_accept_operation_from_address(ces_engine_acceptor* acceptor,
                                                                             std::uintptr_t       address) noexcept {
    const std::uintptr_t first       = reinterpret_cast<std::uintptr_t>(acceptor->transport->operations);
    const std::size_t    table_bytes = sizeof(ces_engine_accept_operation) * acceptor->transport->operation_count;
    if (acceptor->transport->operations == nullptr || address < first || address - first >= table_bytes ||
        (address - first) % sizeof(ces_engine_accept_operation) != 0) {
        ces_engine_fail_fast(L"AcceptEx completion identity", ERROR_INVALID_DATA);
    }
    ces_engine_accept_operation* operation = reinterpret_cast<ces_engine_accept_operation*>(address);
    if (operation->owner != acceptor->transport.get()) {
        ces_engine_fail_fast(L"AcceptEx completion identity", ERROR_INVALID_DATA);
    }
    return operation;
}

ces_handoff_disposition ces_engine_require_handoff_disposition(bool stopping,
                                                               bool failed,
                                                               bool listener_valid,
                                                               bool accepted_valid) noexcept {
    const auto disposition = ces_acceptor_handoff_disposition(stopping, failed, listener_valid, accepted_valid);
    if (disposition == ces_handoff_disposition::fatal) {
        ces_engine_fail_fast(L"AcceptEx handoff invariant", WSAENOTSOCK);
    }
    return disposition;
}

unsigned long __stdcall ces_engine_acceptor_thread(void* parameter) noexcept {
    ces_engine_acceptor* acceptor = static_cast<ces_engine_acceptor*>(parameter);
    for (std::uint32_t index = 0; index < acceptor->transport->operation_count; ++index) {
        if (!ces_engine_post_accept(acceptor, &acceptor->transport->operations[index])) {
            acceptor->failed->store(true, std::memory_order_release);
            ces_engine_stop_acceptor(acceptor);
            break;
        }
    }

    std::uint32_t expired_drain_packets = 0;
    while (!acceptor->core.stopping || ces_engine_acceptor_has_live(acceptor)) {
        if (!acceptor->core.stopping && acceptor->failed->load(std::memory_order_acquire)) {
            ces_engine_stop_acceptor(acceptor);
            if (!ces_engine_acceptor_has_live(acceptor)) {
                break;
            }
        }
        DWORD       transferred = 0;
        ULONG_PTR   key         = 0;
        OVERLAPPED* overlapped  = nullptr;
        const bool  drain_due   = acceptor->core.stopping && GetTickCount64() >= acceptor->drain_deadline;
        const BOOL  ok          = GetQueuedCompletionStatus(acceptor->transport->port, &transferred, &key, &overlapped,
                                                  drain_due ? 0U : 100U);
        const DWORD wait_error  = ok == FALSE ? GetLastError() : ERROR_SUCCESS;
        if (drain_due) {
            // Each live accept slot can own one completion or handoff acknowledgement. Give
            // already-queued packets one bounded zero-wait pass before rejecting missing work.
            if ((ok == FALSE && wait_error == WAIT_TIMEOUT && overlapped == nullptr) ||
                expired_drain_packets >= acceptor->transport->operation_count + 1U) {
                ces_engine_fail_fast(L"TCP acceptor drain", ERROR_TIMEOUT);
            }
            ++expired_drain_packets;
        } else {
            expired_drain_packets = 0;
        }
        if (overlapped == nullptr && key == ces_engine_stop_key) {
            ces_engine_stop_acceptor(acceptor);
            continue;
        }
        if (overlapped == nullptr && key > ces_engine_stop_key) {
            ces_engine_accept_operation* operation =
                ces_engine_accept_operation_from_address(acceptor, static_cast<std::uintptr_t>(key));
            if (acceptor->core.slots[operation->index].state != ces_accept_state::transit &&
                acceptor->core.slots[operation->index].state != ces_accept_state::deferred) {
                ces_engine_fail_fast(L"AcceptEx repost packet state", ERROR_INVALID_STATE);
            }
            if (!operation->control_pending) {
                ces_engine_fail_fast(L"AcceptEx repost packet state", ERROR_INVALID_STATE);
            }
            operation->control_pending = false;
            if (!ces_acceptor_ack(&acceptor->core.slots[operation->index])) {
                ces_engine_fail_fast(L"AcceptEx repost packet state", ERROR_INVALID_STATE);
            }
            if (!acceptor->core.stopping && !ces_engine_post_accept(acceptor, operation)) {
                acceptor->failed->store(true, std::memory_order_release);
                ces_engine_stop_acceptor(acceptor);
            }
            continue;
        }
        if (overlapped != nullptr) {
            ces_engine_accept_operation* operation =
                ces_engine_accept_operation_from_address(acceptor, reinterpret_cast<std::uintptr_t>(overlapped));
            if (acceptor->core.slots[operation->index].state != ces_accept_state::posted || !operation->posted) {
                ces_engine_fail_fast(L"AcceptEx completion state", ERROR_INVALID_STATE);
            }
            if (transferred != 0) {
                ces_engine_fail_fast(L"AcceptEx completion exceeds posted range", ERROR_INVALID_DATA);
            }
            operation->posted              = false;
            const ces_accept_action action = ces_acceptor_on_completion(
                &acceptor->core, operation->index, ok != FALSE, wait_error,
                acceptor->failed->load(std::memory_order_acquire), acceptor->transport->listener != INVALID_SOCKET,
                operation->socket != INVALID_SOCKET);
            if (action.kind == ces_accept_action_kind::fatal) {
                ces_engine_fail_fast(action.failure.stage, action.failure.native_error);
            }
            if (action.kind == ces_accept_action_kind::withdraw) {
                ces_engine_accept_socket_close(operation);
                continue;
            }
            if (action.kind == ces_accept_action_kind::repost) {
                ces_engine_accept_socket_close(operation);
                if (!ces_engine_post_accept(acceptor, operation)) {
                    acceptor->failed->store(true, std::memory_order_release);
                    ces_engine_stop_acceptor(acceptor);
                }
                continue;
            }
            if (action.kind == ces_accept_action_kind::stop) {
                ces_engine_accept_socket_close(operation);
                ces_engine_report(action.failure.stage, action.failure.native_error);
                acceptor->failed->store(true, std::memory_order_release);
                ces_engine_stop_acceptor(acceptor);
                continue;
            }
            if (setsockopt(operation->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                           reinterpret_cast<const char*>(&acceptor->transport->listener),
                           sizeof(acceptor->transport->listener)) != 0 ||
                !ces_engine_configure_socket(operation->socket, acceptor->options, true)) {
                ++acceptor->core.network_errors;
                acceptor->core.slots[operation->index].state = ces_accept_state::idle;
                ces_engine_accept_socket_close(operation);
                acceptor->failed->store(true, std::memory_order_release);
                ces_engine_stop_acceptor(acceptor);
                continue;
            }
            SOCKADDR* local_address  = nullptr;
            SOCKADDR* remote_address = nullptr;
            int       local_length   = 0;
            int       remote_length  = 0;
            acceptor->transport->get_accept_addresses(operation->addresses.data(), 0, sizeof(SOCKADDR_STORAGE) + 16U,
                                                      sizeof(SOCKADDR_STORAGE) + 16U, &local_address, &local_length,
                                                      &remote_address, &remote_length);
            if (local_address == nullptr || remote_address == nullptr || local_length <= 0 || remote_length <= 0) {
                ces_engine_report(L"GetAcceptExSockaddrs", WSAEINVAL);
                acceptor->core.slots[operation->index].state = ces_accept_state::idle;
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
                ++acceptor->core.rejected;
                acceptor->core.slots[operation->index].state = ces_accept_state::idle;
                ces_engine_accept_socket_close(operation);
                if (!ces_engine_post_accept(acceptor, operation)) {
                    acceptor->failed->store(true, std::memory_order_release);
                    ces_engine_stop_acceptor(acceptor);
                }
                continue;
            }
            if (!ces_acceptor_mark_transit(&acceptor->core.slots[operation->index])) {
                ces_engine_fail_fast(L"AcceptEx handoff state", ERROR_INVALID_STATE);
            }
            operation->control_pending  = true;
            const ULONG_PTR handoff_key = static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(operation));
            if (PostQueuedCompletionStatus(worker->transport->port, 0, handoff_key, nullptr) == FALSE) {
                ces_engine_fail_fast(L"PostQueuedCompletionStatus(accept handoff)", static_cast<int>(GetLastError()));
            }
            continue;
        }
        if (ok == FALSE && wait_error != WAIT_TIMEOUT) {
            ces_engine_report(L"GetQueuedCompletionStatus(acceptor)", static_cast<int>(wait_error));
            ++acceptor->core.network_errors;
            acceptor->failed->store(true, std::memory_order_release);
            ces_engine_stop_acceptor(acceptor);
        }
    }
    return acceptor->failed->load(std::memory_order_acquire) ? 1U : 0U;
}

bool ces_engine_acceptor_initialize(ces_engine_acceptor*                acceptor,
                                    const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                    const ces_options*                  options,
                                    ces_engine_worker*                  workers,
                                    std::uint32_t                       worker_count,
                                    std::atomic<bool>*                  failed,
                                    ces_engine_acceptor_resources*      resources) noexcept {
    if (!acceptor->transport) {
        ces_engine_report(L"AcceptEx operation allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    acceptor->transport->resources = resources;
    resources->lifetime            = acceptor->transport.get();
    acceptor->transport->rio       = rio;
    acceptor->options              = options;
    acceptor->workers              = workers;
    acceptor->worker_count         = worker_count;
    acceptor->failed               = failed;
    acceptor->transport->listener  = ces_engine_registered_socket(SOCK_STREAM, IPPROTO_TCP);
    const int listener_error = acceptor->transport->listener == INVALID_SOCKET ? WSAGetLastError() : ERROR_SUCCESS;
    resources->listener.reset(acceptor->transport->listener);
    acceptor->transport->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    const DWORD port_error    = acceptor->transport->port == nullptr ? GetLastError() : ERROR_SUCCESS;
    resources->port.reset(acceptor->transport->port);
    if (acceptor->transport->listener == INVALID_SOCKET || acceptor->transport->port == nullptr) {
        ces_engine_report(
            acceptor->transport->listener == INVALID_SOCKET ? L"WSASocketW(TCP listener)" :
                                                              L"CreateIoCompletionPort(acceptor)",
            acceptor->transport->listener == INVALID_SOCKET ? listener_error : static_cast<int>(port_error));
        ++acceptor->core.network_errors;
        return false;
    }
    if (!ces_engine_configure_socket(acceptor->transport->listener, options, true)) {
        ++acceptor->core.network_errors;
        return false;
    }
    SOCKADDR_IN address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port        = htons(options->port);
    if (bind(acceptor->transport->listener, reinterpret_cast<const SOCKADDR*>(&address), sizeof(address)) != 0 ||
        listen(acceptor->transport->listener, SOMAXCONN) != 0) {
        ces_engine_report(L"bind/listen", WSAGetLastError());
        ++acceptor->core.network_errors;
        return false;
    }
    if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(acceptor->transport->listener), acceptor->transport->port, 0,
                               1) != acceptor->transport->port) {
        ces_engine_report(L"CreateIoCompletionPort(listener association)", static_cast<int>(GetLastError()));
        ++acceptor->core.network_errors;
        return false;
    }
    GUID  accept_identifier  = WSAID_ACCEPTEX;
    GUID  address_identifier = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD bytes              = 0;
    if (WSAIoctl(acceptor->transport->listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_identifier,
                 sizeof(accept_identifier), &acceptor->transport->accept_ex, sizeof(acceptor->transport->accept_ex),
                 &bytes, nullptr, nullptr) != 0 ||
        acceptor->transport->accept_ex == nullptr) {
        ces_engine_report(L"SIO_GET_EXTENSION_FUNCTION_POINTER(AcceptEx)", WSAGetLastError());
        ++acceptor->core.network_errors;
        return false;
    }
    bytes = 0;
    if (WSAIoctl(acceptor->transport->listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &address_identifier,
                 sizeof(address_identifier), &acceptor->transport->get_accept_addresses,
                 sizeof(acceptor->transport->get_accept_addresses), &bytes, nullptr, nullptr) != 0 ||
        acceptor->transport->get_accept_addresses == nullptr) {
        ces_engine_report(L"SIO_GET_EXTENSION_FUNCTION_POINTER(GetAcceptExSockaddrs)", WSAGetLastError());
        ++acceptor->core.network_errors;
        return false;
    }
    acceptor->transport->operation_count = ces_accept_operation_count(worker_count);
    acceptor->transport->operations      = static_cast<ces_engine_accept_operation*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                       sizeof(ces_engine_accept_operation) * acceptor->transport->operation_count));
    resources->operations.reset(acceptor->transport->operations);
    acceptor->core.operation_count = acceptor->transport->operation_count;
    acceptor->operation_states.reset(new (std::nothrow) ces_accept_slot[acceptor->transport->operation_count]{});
    acceptor->core.slots = acceptor->operation_states.get();
    resources->operation_sockets.reset(new (std::nothrow) ces_socket_owner[acceptor->transport->operation_count]);
    if (acceptor->transport->operations == nullptr || acceptor->core.slots == nullptr ||
        !resources->operation_sockets) {
        ces_engine_report(L"AcceptEx operation allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    for (std::uint32_t index = 0; index < acceptor->transport->operation_count; ++index) {
        acceptor->transport->operations[index].owner       = acceptor->transport.get();
        acceptor->transport->operations[index].socket      = INVALID_SOCKET;
        acceptor->transport->operations[index].accept_port = acceptor->transport->port;
        acceptor->core.slots[index].state                  = ces_accept_state::idle;
        acceptor->transport->operations[index].index       = index;
    }
    acceptor->transport->thread = CreateThread(nullptr, 0, ces_engine_acceptor_thread, acceptor, 0, nullptr);
    resources->thread.reset(acceptor->transport->thread);
    if (acceptor->transport->thread == nullptr) {
        ces_engine_report(L"CreateThread(acceptor)", static_cast<int>(GetLastError()));
        return false;
    }
    return true;
}

void ces_engine_acceptor_join(ces_engine_acceptor* acceptor) noexcept {
    if (acceptor->transport != nullptr && acceptor->transport->thread != nullptr) {
        if (WaitForSingleObject(acceptor->transport->thread, INFINITE) != WAIT_OBJECT_0) {
            ces_engine_fail_fast(L"WaitForSingleObject(acceptor)", static_cast<int>(GetLastError()));
        }
    }
}

void ces_engine_acceptor_require_release(const ces_engine_acceptor* acceptor) noexcept {
    if (acceptor->transport != nullptr) {
        ces_native_acceptor_require_release(acceptor->transport.get());
    }
    if (acceptor->transport != nullptr && acceptor->transport->operations != nullptr) {
        for (std::uint32_t index = 0; index < acceptor->transport->operation_count; ++index) {
            if (acceptor->transport->operations[index].posted) {
                ces_engine_fail_fast(L"TCP acceptor terminal operations", ERROR_IO_INCOMPLETE);
            }
        }
    }
    if (acceptor->core.slots != nullptr && ces_engine_acceptor_has_live(acceptor)) {
        ces_engine_fail_fast(L"TCP acceptor terminal operations", ERROR_IO_INCOMPLETE);
    }
}

void ces_engine_acceptor_destroy(ces_engine_acceptor* acceptor) noexcept {
    ces_engine_acceptor_join(acceptor);
    ces_engine_acceptor_require_release(acceptor);
    if (acceptor->transport == nullptr || acceptor->transport->resources == nullptr) {
        return;
    }
    if (acceptor->transport != nullptr && acceptor->transport->thread != nullptr) {
        acceptor->transport->resources->thread.reset();
        acceptor->transport->thread = nullptr;
    }
    acceptor->transport->resources->listener.reset();
    acceptor->transport->listener = INVALID_SOCKET;
    if (acceptor->transport != nullptr && acceptor->transport->operations != nullptr) {
        if (acceptor->transport->resources->operation_sockets) {
            for (std::uint32_t index = 0; index < acceptor->transport->operation_count; ++index) {
                ces_engine_accept_socket_close(&acceptor->transport->operations[index]);
            }
        }
        acceptor->transport->resources->operation_sockets.reset();
        acceptor->transport->resources->operations.reset();
        acceptor->transport->operations = nullptr;
    }
    acceptor->operation_states.reset();
    acceptor->core.slots = nullptr;
    acceptor->transport->resources->port.reset();
    acceptor->transport->port                = nullptr;
    acceptor->transport->resources->lifetime = nullptr;
    acceptor->transport->resources           = nullptr;
}

static void ces_engine_udp_arm(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                               RIO_CQ                              queue,
                               OVERLAPPED*                         overlapped,
                               bool*                               armed,
                               std::uint64_t*                      arm_count) noexcept {
    // Ownership check first, then the OVERLAPPED reset: the shared transaction repeats the check.
    if (*armed) {
        ces_engine_fail_fast(L"duplicate UDP RIONotify", ERROR_INVALID_STATE);
    }
    std::memset(overlapped, 0, sizeof(*overlapped));
    ces_notification_arm(rio, queue, armed, arm_count, L"RIONotify(UDP)");
}

ces_exit_code ces_engine_run_udp(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                 const ces_options*                  options,
                                 std::atomic<bool>*                  stop_requested) noexcept {
    const std::uint32_t stride = options->rio_buffer_bytes + CES_UDP_ADDRESS_BYTES;
    SYSTEM_INFO         system_info{};
    GetSystemInfo(&system_info);
    const std::size_t page        = system_info.dwPageSize == 0U ? 4096U : system_info.dwPageSize;
    std::size_t       arena_bytes = 0;
    if (options->udp_depth > options->cq_capacity / 2U) {
        ces_engine_report(L"UDP CQ capacity", ERROR_INSUFFICIENT_BUFFER);
        return ces_exit_code::usage;
    }
    if (!ces_checked_page_rounded_arena_bytes(options->udp_depth, stride, options->memory_bytes, page, &arena_bytes) ||
        arena_bytes > std::numeric_limits<DWORD>::max()) {
        ces_engine_report(L"UDP registered arena", ERROR_NOT_ENOUGH_MEMORY);
        return ces_exit_code::usage;
    }

    ces_socket_owner        socket_owner{ ces_engine_registered_socket(SOCK_DGRAM, IPPROTO_UDP) };
    SOCKET                  socket_value = socket_owner.get();
    const int               socket_error = socket_value == INVALID_SOCKET ? WSAGetLastError() : ERROR_SUCCESS;
    ces_handle_owner        port_owner{ CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1) };
    HANDLE                  port       = port_owner.get();
    const DWORD             port_error = port == nullptr ? GetLastError() : ERROR_SUCCESS;
    ces_virtual_arena_owner arena_owner{ VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) };
    char*                   memory      = static_cast<char*>(arena_owner.get());
    const DWORD             arena_error = memory == nullptr ? GetLastError() : ERROR_SUCCESS;
    ces_engine_udp_slot*    slots       = static_cast<ces_engine_udp_slot*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ces_engine_udp_slot) * options->udp_depth));
    ces_heap_owner                                slots_owner{ slots };
    std::unique_ptr<ces_rio_registration_owner[]> slot_registrations{ new (
        std::nothrow) ces_rio_registration_owner[options->udp_depth] };
    ces_rio_cq_owner                              completion_queue_owner{};
    RIO_CQ                                        completion_queue = RIO_INVALID_CQ;
    RIO_RQ                                        request_queue    = RIO_INVALID_RQ;
    OVERLAPPED                                    notification_overlapped{};
    bool                                          armed       = false;
    std::uint32_t                                 outstanding = 0;
    ces_udp_engine                                core{};
    // Notification accounting for the UDP loop, mirroring the TCP worker: a bounded wait that
    // expires with RIO work outstanding and no notification armed means the queue was never armed.
    std::uint64_t                                 notify_arms            = 0;
    std::uint64_t                                 notify_deliveries      = 0;
    std::uint64_t                                 notify_timeout_wakeups = 0;
    ces_udp_slot_state*                           states                 = static_cast<ces_udp_slot_state*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ces_udp_slot_state) * options->udp_depth));
    ces_heap_owner states_owner{ states };
    if (states != nullptr) {
        (void) ces_udp_initialize(&core, states, options->udp_depth, options->rio_buffer_bytes);
    }

    if (socket_value == INVALID_SOCKET || port == nullptr || memory == nullptr || slots == nullptr ||
        !slot_registrations || states == nullptr) {
        if (socket_value == INVALID_SOCKET) {
            ces_engine_report(L"WSASocketW(UDP RIO)", socket_error);
            ++core.statistics.network_errors;
        } else if (port == nullptr) {
            ces_engine_report(L"CreateIoCompletionPort(UDP)", static_cast<int>(port_error));
        } else if (memory == nullptr) {
            ces_engine_report(L"VirtualAlloc(UDP arena)", static_cast<int>(arena_error));
        } else {
            ces_engine_report(L"UDP runtime allocation", ERROR_NOT_ENOUGH_MEMORY);
        }
        core.failed = true;
    }
    if (!core.failed && !ces_engine_configure_socket(socket_value, options, false)) {
        ++core.statistics.network_errors;
        core.failed = true;
    }
    if (!core.failed) {
        SOCKADDR_IN address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port        = htons(options->port);
        if (bind(socket_value, reinterpret_cast<const SOCKADDR*>(&address), sizeof(address)) != 0) {
            ces_engine_report(L"bind(UDP)", WSAGetLastError());
            ++core.statistics.network_errors;
            core.failed = true;
        }
    }
    if (!core.failed) {
        if (!ces_register_windows(rio, memory, stride, options->udp_depth, slot_registrations.get())) {
            ces_engine_report(L"UDP slot registrations", WSAGetLastError());
            ++core.statistics.network_errors;
            core.failed = true;
        }
    }
    if (!core.failed) {
        RIO_NOTIFICATION_COMPLETION notification{};
        notification.Type               = RIO_IOCP_COMPLETION;
        notification.Iocp.IocpHandle    = port;
        notification.Iocp.CompletionKey = slots;
        notification.Iocp.Overlapped    = &notification_overlapped;
        completion_queue                = rio->RIOCreateCompletionQueue(options->cq_capacity, &notification);
        completion_queue_owner.reset(rio, completion_queue);
        if (completion_queue == RIO_INVALID_CQ) {
            ces_engine_report(L"UDP RIO CQ creation", WSAGetLastError());
            ++core.statistics.network_errors;
            core.failed = true;
        }
    }
    if (!core.failed) {
        request_queue = rio->RIOCreateRequestQueue(socket_value, options->udp_depth, 1, options->udp_depth, 1,
                                                   completion_queue, completion_queue, slots);
        if (request_queue == RIO_INVALID_RQ) {
            ces_engine_report(L"RIOCreateRequestQueue(UDP)", WSAGetLastError());
            ++core.statistics.network_errors;
            core.failed = true;
        }
    }
    if (!core.failed) {
        for (std::uint32_t index = 0; index < options->udp_depth; ++index) {
            const RIO_BUFFERID registration = slot_registrations[index].get();
            slots[index].payload            = RIO_BUF{ registration, 0, options->rio_buffer_bytes };
            slots[index].remote_address     = RIO_BUF{ registration, options->rio_buffer_bytes, CES_UDP_ADDRESS_BYTES };
            const ces_action action{ ces_action_kind::receive, index, 0, options->rio_buffer_bytes, {} };
            if (!ces_native_udp_post(rio, request_queue, &slots[index], action, &outstanding)) {
                const ces_failure failure{ L"RIOReceiveEx(UDP)", WSAGetLastError() };
                ces_engine_report(failure.stage, failure.native_error);
                (void) ces_udp_on_event(
                    &core, {
                               ces_event_kind::post_failed, { index, ces_engine_operation::receive, 0, 0 },
                                failure
                });
                break;
            }
            if (!ces_udp_mark_posted(&core, index)) {
                ces_engine_fail_fast(L"UDP post invariant", ERROR_INVALID_STATE);
            }
        }
        if (ces_notify_should_arm(armed, outstanding)) {
            ces_engine_udp_arm(rio, completion_queue, &notification_overlapped, &armed, &notify_arms);
        }
    }

    const ULONGLONG start = GetTickCount64();
    core.closing          = core.failed;
    if (core.closing) {
        ces_udp_begin_drain(&core);
    }
    ULONGLONG drain_deadline = core.closing ? start + ces_engine_drain_grace_ms : 0;
    if (core.closing && socket_value != INVALID_SOCKET) {
        ces_engine_owned_socket_close(&socket_owner, &socket_value);
    }
    std::array<RIORESULT, ces_engine_batch_size> results{};
    const auto                                   drain_results = [&](std::uint32_t maximum_batches,
                                   std::uint32_t maximum_results = std::numeric_limits<std::uint32_t>::max()) noexcept {
        for (std::uint32_t batch = 0; batch < maximum_batches && maximum_results != 0; ++batch) {
            const ULONG requested = std::min<ULONG>(static_cast<ULONG>(results.size()), maximum_results);
            const ULONG count = ces_require_valid_dequeue_count(
                rio->RIODequeueCompletion(completion_queue, results.data(), requested), L"RIODequeueCompletion(UDP)");
            if (count > requested) {
                ces_engine_fail_fast(L"UDP RIO dequeue extent", ERROR_INVALID_DATA);
            }
            if (count == 0) {
                break;
            }
            maximum_results -= count;
            for (ULONG result_index = 0; result_index < count; ++result_index) {
                const ces_completion completion =
                    ces_native_udp_completion(slots, options->udp_depth, &outstanding, results[result_index]);
                const ces_action action = ces_udp_on_event(&core, { ces_event_kind::completion, completion, {} });
                if (action.kind == ces_action_kind::fatal) {
                    ces_engine_fail_fast(action.failure.stage, action.failure.native_error);
                }
                if (action.kind == ces_action_kind::none) {
                    continue;
                }
                if (action.kind == ces_action_kind::close) {
                    ces_engine_report(action.failure.stage, action.failure.native_error);
                    drain_deadline = GetTickCount64() + ces_engine_drain_grace_ms;
                    ces_engine_owned_socket_close(&socket_owner, &socket_value);
                    continue;
                }
                if (!ces_native_udp_post(rio, request_queue, &slots[action.index], action, &outstanding)) {
                    const ces_failure failure{ L"UDP RIO repost", WSAGetLastError() };
                    ces_engine_report(failure.stage, failure.native_error);
                    (void) ces_udp_on_event(
                        &core,
                        {
                            ces_event_kind::post_failed, { action.index, completion.operation, 0, 0 },
                             failure
                    });
                    drain_deadline = GetTickCount64() + ces_engine_drain_grace_ms;
                    ces_engine_owned_socket_close(&socket_owner, &socket_value);
                    continue;
                }
                if (!ces_udp_mark_posted(&core, action.index)) {
                    ces_engine_fail_fast(L"UDP post invariant", ERROR_INVALID_STATE);
                }
            }
        }
        if (ces_notify_should_arm(armed, outstanding)) {
            ces_engine_udp_arm(rio, completion_queue, &notification_overlapped, &armed, &notify_arms);
        }
    };
    std::uint32_t expired_drain_packets = 0;
    while (!core.closing || outstanding != 0) {
        if (!core.closing &&
            (stop_requested->load(std::memory_order_acquire) ||
             (options->run_seconds != 0 && GetTickCount64() - start >= options->run_seconds * 1000ULL))) {
            (void) ces_udp_on_event(&core, { ces_event_kind::stop, {}, {} });
            drain_deadline = GetTickCount64() + ces_engine_drain_grace_ms;
            ces_engine_owned_socket_close(&socket_owner, &socket_value);
        }
        DWORD       transferred = 0;
        ULONG_PTR   key         = 0;
        OVERLAPPED* overlapped  = nullptr;
        const bool  drain_due   = core.closing && GetTickCount64() >= drain_deadline;
        const BOOL  ok    = GetQueuedCompletionStatus(port, &transferred, &key, &overlapped, drain_due ? 0U : 100U);
        const DWORD error = ok == FALSE ? GetLastError() : ERROR_SUCCESS;
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
            ++notify_deliveries;
            drain_results(ces_engine_max_drain_batches);
        } else if (ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr) {
            // A bounded wait that expires while RIO work is outstanding and no notification is armed
            // means the queue was never armed for that work: debug builds treat it as starvation.
            if (outstanding != 0 && !armed && !core.closing) {
                ++notify_timeout_wakeups;
            }
        } else if (ok == FALSE && error != WAIT_TIMEOUT) {
            ces_engine_fail_fast(L"GetQueuedCompletionStatus(UDP)", static_cast<int>(error));
        } else if (!(ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr)) {
            ces_engine_fail_fast(L"unexpected UDP IOCP packet", ERROR_INVALID_DATA);
        }
        if (core.closing && outstanding != 0 && GetTickCount64() >= drain_deadline) {
            const bool queued_packet = ok != FALSE || overlapped != nullptr || key != 0;
            if (queued_packet && expired_drain_packets < options->udp_depth + 1U) {
                ++expired_drain_packets;
                continue;
            }
            // CQ completion ownership can retire before its notification reaches the IOCP.
            // One nonempty dequeue per slot covers the worst case of short returned batches.
            drain_results(options->udp_depth, options->udp_depth);
            if (outstanding != 0) {
                ces_engine_fail_fast(L"UDP shutdown drain", ERROR_TIMEOUT);
            }
        } else {
            expired_drain_packets = 0;
        }
    }

    if (core.failed && socket_value != INVALID_SOCKET) {
        ces_engine_owned_socket_close(&socket_owner, &socket_value);
    }
    // Teardown contract: after this point no new RIO request is posted, every published operation
    // has already retired, and only then is the completion queue closed. RIOCloseCompletionQueue
    // invalidates the queue and silently drops completions that would have been added afterwards,
    // so outstanding must be zero here and no accounting may depend on a later completion. A
    // pending RIONotify delivery is deliberately not awaited: the notification OVERLAPPED stays
    // valid until the IOCP handle is closed at the end of this function, which is the last object
    // that can reference it.
#    ifndef NDEBUG
    if (notify_timeout_wakeups != 0) {
        ces_engine_fail_fast(L"UDP notification starvation", ERROR_INVALID_STATE);
    }
    if (notify_arms < notify_deliveries || notify_arms - notify_deliveries > 1U) {
        ces_engine_fail_fast(L"UDP notification accounting", ERROR_INVALID_STATE);
    }
#    endif
    core.phase = ces_udp_phase::stopped;
    if (outstanding != 0 || !ces_udp_may_release(core.phase, core.outstanding)) {
        ces_engine_fail_fast(L"UDP cleanup with outstanding operations", ERROR_IO_INCOMPLETE);
    }
    for (std::uint32_t index = 0; slots != nullptr && index < options->udp_depth; ++index) {
        if (slots[index].outstanding || (states != nullptr && states[index].outstanding)) {
            ces_engine_fail_fast(L"UDP cleanup with outstanding operations", ERROR_IO_INCOMPLETE);
        }
    }
    if (options->stats) {
        ces_engine_print_final_statistics(ces_protocol::udp, &core.statistics, GetTickCount64() - start, 1U,
                                          outstanding);
    }
    completion_queue_owner.reset();
    completion_queue = RIO_INVALID_CQ;
    slot_registrations.reset();
    arena_owner.reset();
    memory = nullptr;
    states_owner.reset();
    core.slots = nullptr;
    slots_owner.reset();
    slots = nullptr;
    ces_engine_owned_socket_close(&socket_owner, &socket_value);
    port_owner.reset();
    port = nullptr;
    // Opt-in observability: the UDP loop reports its own counters as worker 0, and a diagnostics
    // failure never changes the production result.
    const ces_notify_diagnostic_snapshot udp_snapshot{ 0U, notify_arms, notify_deliveries, notify_timeout_wakeups };
    ces_write_diagnostics(&udp_snapshot, 1U, ces_protocol::udp);
    return core.failed ? ces_exit_code::network : ces_exit_code::success;
}

bool ces_worker_try_reserve_admission(ces_engine_worker* worker) noexcept {
    if (worker == nullptr) {
        return false;
    }
    // A load-then-handoff would overbook when several AcceptEx completions arrive back to back, so
    // the reservation must be a compare-exchange that consumes exactly one credit.
    std::uint32_t credit = worker->admission_credit.load(std::memory_order_relaxed);
    while (credit != 0U) {
        if (worker->admission_credit.compare_exchange_weak(credit, credit - 1U, std::memory_order_acquire,
                                                           std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void ces_worker_release_admission_credit(ces_engine_worker* worker) noexcept {
    if (worker == nullptr) {
        return;
    }
    const std::uint32_t previous = worker->admission_credit.fetch_add(1U, std::memory_order_release);
    if (worker->core == nullptr || previous >= worker->core->slot_count) {
        // Returning more credit than the worker ever owned means a reservation was released twice;
        // that would let the acceptor overbook the worker later.
        ces_engine_fail_fast(L"worker admission credit", ERROR_INVALID_STATE);
    }
}

ces_engine_worker* ces_acceptor_select_worker(ces_engine_acceptor* acceptor) noexcept {
    if (acceptor == nullptr || acceptor->workers == nullptr || acceptor->worker_count == 0U) {
        return nullptr;
    }
    for (std::uint32_t offset = 0; offset < acceptor->worker_count; ++offset) {
        const std::uint32_t index = (acceptor->core.next_worker + offset) % acceptor->worker_count;
        if (ces_worker_try_reserve_admission(&acceptor->workers[index])) {
            acceptor->core.next_worker = (index + 1U) % acceptor->worker_count;
            return &acceptor->workers[index];
        }
    }
    return nullptr;
}

ces_engine_worker::ces_engine_worker() noexcept :
    transport(new (std::nothrow) ces_native_worker{}),
    core(new (std::nothrow) ces_tcp_engine{}) {}

ces_engine_worker::~ces_engine_worker() noexcept {
    ces_engine_worker_require_release(this);
    if (transport != nullptr && transport->resources != nullptr) {
        ces_engine_worker_destroy(this);
    }
}

ces_engine_acceptor::ces_engine_acceptor() noexcept : transport(new (std::nothrow) ces_native_acceptor{}) {}

ces_engine_acceptor::~ces_engine_acceptor() noexcept {
    ces_engine_acceptor_require_release(this);
    if (transport != nullptr && transport->resources != nullptr) {
        ces_engine_acceptor_destroy(this);
    }
}
#endif
