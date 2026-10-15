module;
// WinSock2 must precede the Windows networking headers.
// clang-format off
#include <WinSock2.h>
#include <Windows.h>
// clang-format on
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <type_traits>

export module ces.test.support;

import ces.types;
import ces.internal;
import ces.engine;

// WinSock2.h must be included before the Windows networking headers below.
// clang-format off
// clang-format on

// Deterministic model of the documented worker arming sequence. It drives the production policy
// helpers (ces_notify_should_arm and the notification mark transitions) so the contract itself is
// what gets exercised, and it counts the observations that expose a missed arm.

export struct ces_notify_status_case {
    int                    status;
    ces_rio_notify_outcome expected;
    const char*            name;
};

export struct ces_notify_model {
    bool          armed{ false };
    std::uint32_t outstanding{ 0 };
    std::uint64_t arms{ 0 };
    std::uint64_t deliveries{ 0 };
    // Deliveries observed with no pending arm. Production fails fast on these, so the model records
    // them as violations instead of tolerating them as an empty delivery.
    std::uint64_t violations{ 0 };
    std::uint64_t timeout_wakeups_while_outstanding{ 0 };
    bool          stopped{ false };
};

export inline void ces_notify_model_post(ces_notify_model* model, std::uint32_t count) noexcept {
    model->outstanding += count;
    if (ces_notify_should_arm(model->armed, model->outstanding) && ces_notification_mark_rearmed(&model->armed)) {
        ++model->arms;
    }
}

export inline void ces_notify_model_deliver(ces_notify_model* model, std::uint32_t retired) noexcept {
    if (!ces_notification_mark_delivered(&model->armed)) {
        ++model->violations;
        return;
    }
    ++model->deliveries;
    model->outstanding -= retired <= model->outstanding ? retired : model->outstanding;
    if (!model->stopped && ces_notify_should_arm(model->armed, model->outstanding) &&
        ces_notification_mark_rearmed(&model->armed)) {
        ++model->arms;
    }
}

// Work retired by RIODequeueCompletion alone. The notification that woke the worker may still be
// pending in the port queue, so this drain path does not consume a notification.
export inline void ces_notify_model_retire(ces_notify_model* model, std::uint32_t retired) noexcept {
    model->outstanding -= retired <= model->outstanding ? retired : model->outstanding;
}

export inline void ces_notify_model_timeout(ces_notify_model* model) noexcept {
    if (model->outstanding != 0 && !model->armed) {
        ++model->timeout_wakeups_while_outstanding;
    }
}

// Teardown contract: the completion queue may be closed once no RIO operation is outstanding and the
// arm state was never violated. A notification that is still in flight (arms - deliveries == 1) is
// allowed to remain unconsumed; production never waits for it and never forges it.
export inline bool ces_notify_model_may_close(const ces_notify_model* model) noexcept {
    return model->outstanding == 0 && model->violations == 0 && model->arms >= model->deliveries &&
           model->arms - model->deliveries <= 1U;
}

// Shared expectation ledger for the contract, engine and pure-engine executables. Each executable
// links its own module instance, so the counters keep the per-suite totals the suites always printed.
export int ces_test_failures        = 0;
export int ces_engine_test_failures = 0;

export void ces_test_expect(bool condition, const char* name) noexcept {
    if (condition) {
        std::printf("PASS %s\n", name);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", name);
    ++ces_test_failures;
}

export void ces_engine_test_expect(bool condition, const char* name) noexcept {
    if (condition) {
        std::printf("PASS %s\n", name);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", name);
    ++ces_engine_test_failures;
}

export class ces_contract_action_transport {
  public:
    ces_action    last{};
    std::uint32_t executions = 0;

    void execute(const ces_action& action) noexcept {
        last = action;
        ++executions;
    }
};

export void ces_engine_test_tcp_event_contract() noexcept {
    std::array<ces_connection, 2> connections{};
    std::array<std::uint32_t, 2>  free_indices{};
    std::array<ces_timer_node, 2> timers{};
    std::array<std::uint32_t, 2>  positions{};
    ces_tcp_engine                engine{};
    ces_contract_action_transport transport{};
    ces_engine_test_expect(
        ces_tcp_initialize(&engine, connections.data(), free_indices.data(), timers.data(), positions.data(), 2, 1000),
        "pure TCP storage initializes without Native");
    std::uint32_t index = 99;
    ces_engine_test_expect(ces_tcp_take_slot(&engine, &index) && index == 0,
                           "pure TCP free pool uses the reference lowest slot order");
    ces_action action = ces_tcp_admit(&engine, index, 10);
    ces_engine_test_expect(action.kind == ces_action_kind::receive && engine.active_count == 1 &&
                               engine.outstanding == 1 && connections[0].deadline == 1010,
                           "pure TCP admission emits one receive intent and deadline");
    action =
        ces_runtime_tcp_dispatch_event(&engine,
                                       {
                                           ces_event_kind::completion, { 0, ces_engine_operation::receive, 0, 8 },
                                            {}
    },
                                       20, transport);
    ces_engine_test_expect(
        action.kind == ces_action_kind::send && action.offset == 0 && action.length == 8 && engine.outstanding == 1,
        "pure TCP receive completion emits the entire echo range");
    action =
        ces_runtime_tcp_dispatch_event(&engine,
                                       {
                                           ces_event_kind::completion, { 0, ces_engine_operation::send, 0, 3 },
                                            {}
    },
                                       30, transport);
    ces_engine_test_expect(action.kind == ces_action_kind::send && action.offset == 3 && action.length == 5,
                           "pure TCP partial send emits only the remaining tail");
    action =
        ces_runtime_tcp_dispatch_event(&engine,
                                       {
                                           ces_event_kind::completion, { 0, ces_engine_operation::send, 0, 5 },
                                            {}
    },
                                       40, transport);
    ces_engine_test_expect(action.kind == ces_action_kind::receive &&
                               connections[0].state == ces_connection_state::receiving &&
                               engine.statistics.sent_bytes == 8 && engine.statistics.bytes == 8,
                           "pure TCP completed echo returns to receive with the shared byte ledger");
    (void) ces_tcp_on_event(&engine, { ces_event_kind::stop, {}, {} }, 50);
    action = ces_tcp_close(&engine, 0, 50);
    ces_engine_test_expect(action.kind == ces_action_kind::close && engine.outstanding == 1 && engine.active_count == 1,
                           "pure TCP cancellation retains the published operation");
    (void) ces_tcp_on_event(&engine, { ces_event_kind::admission_closed, {}, {} }, 50);
    ces_engine_test_expect(!ces_tcp_may_exit(&engine),
                           "pure TCP admission barrier cannot bypass cancellation completion");
    action = ces_runtime_tcp_dispatch_event(
        &engine,
        {
            ces_event_kind::completion, { 0, ces_engine_operation::receive, 995, 0 },
             {}
    },
        60, transport);
    ces_engine_test_expect(action.kind == ces_action_kind::release && ces_tcp_may_exit(&engine) &&
                               engine.free_count == 2 && engine.statistics.network_errors == 0 &&
                               engine.statistics.completions == 4,
                           "pure TCP only cancelled completion releases the slot and closes the lifecycle");
    action = ces_runtime_tcp_dispatch_event(
        &engine,
        {
            ces_event_kind::completion, { 0, ces_engine_operation::receive, 995, 0 },
             {}
    },
        70, transport);
    ces_engine_test_expect(action.kind == ces_action_kind::fatal && engine.statistics.completions == 4,
                           "pure TCP duplicate completion cannot decrement or enter the statistics ledger");
    ces_engine_test_expect(
        transport.executions == 5 && transport.last.kind == ces_action_kind::fatal,
        "production static Runtime dispatcher delivers the same Core action to a replacement Transport");
    ces_tcp_engine admission_first{};
    ces_engine_test_expect(ces_tcp_initialize(&admission_first, connections.data(), free_indices.data(), timers.data(),
                                              positions.data(), 2, 1000),
                           "pure TCP empty admission-first worker initializes");
    (void) ces_tcp_on_event(&admission_first, { ces_event_kind::admission_closed, {}, {} }, 10);
    (void) ces_tcp_on_event(&admission_first, { ces_event_kind::stop, {}, {} }, 10);
    ces_engine_test_expect(
        ces_tcp_may_exit(&admission_first),
        "pure TCP admission-closed then stop preserves the exit phase used by production coordinator");
}

export void ces_engine_test_udp_event_contract() noexcept {
    std::array<ces_udp_slot_state, 2> slots{};
    ces_udp_engine                    engine{};
    ces_engine_test_expect(ces_udp_initialize(&engine, slots.data(), 2, 65507) && ces_udp_mark_posted(&engine, 0) &&
                               ces_udp_mark_posted(&engine, 1) && !ces_udp_mark_posted(&engine, 1),
                           "pure UDP fixed depth and posted ownership reject duplicate posts");
    ces_action action =
        ces_udp_on_event(&engine, {
                                      ces_event_kind::completion, { 0, ces_engine_operation::receive, 0, 0 },
                                       {}
    });
    ces_engine_test_expect(
        action.kind == ces_action_kind::send && action.length == 0 && ces_udp_mark_posted(&engine, 0),
        "pure UDP zero length datagram still emits a zero length echo");
    action = ces_udp_on_event(&engine, {
                                           ces_event_kind::completion, { 0, ces_engine_operation::send, 0, 0 },
                                            {}
    });
    ces_engine_test_expect(
        action.kind == ces_action_kind::receive && action.length == 65507 && ces_udp_mark_posted(&engine, 0),
        "pure UDP echo completion restores the full receive window");
    action =
        ces_udp_on_event(&engine, {
                                      ces_event_kind::completion, { 1, ces_engine_operation::receive, 10054, 0 },
                                       {}
    });
    ces_engine_test_expect(action.kind == ces_action_kind::receive && action.length == 65507 &&
                               ces_udp_mark_posted(&engine, 1) && engine.statistics.network_errors == 1,
                           "pure UDP reset records one error and rearms receive");
    action = ces_udp_on_event(&engine, { ces_event_kind::stop, {}, {} });
    ces_engine_test_expect(action.kind == ces_action_kind::close && engine.outstanding == 2,
                           "pure UDP stop retains both provider operations");
    (void) ces_udp_on_event(&engine,
                            {
                                ces_event_kind::completion, { 0, ces_engine_operation::receive, 995, 0 },
                                 {}
    });
    (void) ces_udp_on_event(&engine,
                            {
                                ces_event_kind::completion, { 1, ces_engine_operation::receive, 995, 0 },
                                 {}
    });
    engine.phase = ces_udp_phase::stopped;
    ces_engine_test_expect(ces_udp_may_release(engine.phase, engine.outstanding) &&
                               engine.statistics.network_errors == 1 && engine.statistics.completions == 5 &&
                               engine.statistics.receives == 1 && engine.statistics.sends == 1,
                           "pure UDP cancelled completions retire ownership without inflating network errors");
}

export void ces_engine_test_acceptor_state_contract() noexcept {
    ces_accept_slot   slot{};
    ces_acceptor_core core{ &slot, 1, 0, 0, 0, false };
    ces_engine_test_expect(
        ces_acceptor_mark_posted(&slot) && !ces_acceptor_mark_posted(&slot) && ces_acceptor_has_live(&core),
        "pure Acceptor posted state owns the completion exactly once");
    core.stopping = true;
    ces_engine_test_expect(
        ces_acceptor_has_live(&core) && ces_acceptor_retire_post(&slot) && !ces_acceptor_has_live(&core),
        "pure Acceptor stop leaves posted ownership live until completion retires");
    ces_engine_test_expect(ces_acceptor_mark_transit(&slot) && ces_acceptor_has_live(&core) &&
                               ces_acceptor_ack(&slot) && !ces_acceptor_has_live(&core) &&
                               ces_acceptor_defer_repost(&slot) && ces_acceptor_ack(&slot) && !ces_acceptor_ack(&slot),
                           "pure Acceptor handoff and deferred acknowledgements each retire one live slot");
    core.stopping = false;
    (void) ces_acceptor_mark_posted(&slot);
    ces_accept_action action = ces_acceptor_on_completion(&core, 0, true, 0, false, true, true);
    ces_engine_test_expect(action.kind == ces_accept_action_kind::handoff && core.network_errors == 0,
                           "pure Acceptor healthy completion admits a valid accepted socket");
    (void) ces_acceptor_mark_posted(&slot);
    action = ces_acceptor_on_completion(&core, 0, true, 0, false, true, false);
    ces_engine_test_expect(action.kind == ces_accept_action_kind::fatal && action.failure.native_error == 10038,
                           "pure Acceptor policy A classifies a healthy invalid socket as an invariant failure");
    (void) ces_acceptor_mark_posted(&slot);
    action = ces_acceptor_on_completion(&core, 0, false, 13, false, true, true);
    ces_engine_test_expect(
        action.kind == ces_accept_action_kind::stop && action.failure.native_error == 13 && core.network_errors == 1,
        "pure Acceptor native transport failure stops with one network error");
    (void) ces_acceptor_mark_posted(&slot);
    ces_acceptor_stop(&core);
    action = ces_acceptor_on_completion(&core, 0, false, 995, false, false, false);
    ces_engine_test_expect(
        action.kind == ces_accept_action_kind::withdraw && core.network_errors == 1 && !ces_acceptor_has_live(&core),
        "pure Acceptor cancellation retires posted state without a network error");
}
