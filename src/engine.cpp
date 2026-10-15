module;
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

export module ces.engine;

import ces.types;

// The aggregate owns only pure connection intent, the free pool and deadlines.
// Its storage is allocated once by the cold Native initialization boundary.
// Connection slots and the movable timer heap are the pure scheduling state of this engine module.
export struct ces_connection {
    ces_connection_state state;
    ces_engine_operation operation;
    std::uint32_t        index;
    std::uint32_t        outstanding;
    std::uint32_t        echo_bytes;
    std::uint32_t        send_offset;
    std::uint64_t        deadline;
};

export bool ces_connection_is_active(const ces_connection* connection) noexcept;

export bool ces_timer_initialize(ces_timer_heap* heap,
                                 ces_timer_node* nodes,
                                 std::uint32_t*  positions,
                                 std::uint32_t   capacity) noexcept;
export bool ces_timer_insert_or_update(ces_timer_heap* heap,
                                       std::uint32_t   connection_index,
                                       std::uint64_t   deadline) noexcept;
export bool ces_timer_remove(ces_timer_heap* heap, std::uint32_t connection_index) noexcept;
export bool ces_timer_pop_expired(ces_timer_heap* heap, std::uint64_t now, std::uint32_t* connection_index) noexcept;
export std::uint32_t ces_timer_wait_milliseconds(const ces_timer_heap* heap, std::uint64_t now) noexcept;

export struct ces_tcp_engine {
    ces_connection*       connections;
    std::uint32_t*        free_indices;
    ces_timer_heap        timers;
    std::uint32_t         slot_count;
    std::uint32_t         free_count;
    std::uint32_t         active_count;
    std::uint32_t         outstanding;
    std::uint64_t         timeout_milliseconds;
    ces_engine_statistics statistics;
    ces_worker_phase      phase;
    bool                  stopping;
    bool                  admission_closed;
};

export bool       ces_tcp_initialize(ces_tcp_engine* engine,
                                     ces_connection* connections,
                                     std::uint32_t*  free_indices,
                                     ces_timer_node* timers,
                                     std::uint32_t*  positions,
                                     std::uint32_t   count,
                                     std::uint64_t   timeout_milliseconds) noexcept;
export bool       ces_tcp_take_slot(ces_tcp_engine* engine, std::uint32_t* index) noexcept;
export bool       ces_tcp_return_slot(ces_tcp_engine* engine, std::uint32_t index) noexcept;
export ces_action ces_tcp_admit(ces_tcp_engine* engine, std::uint32_t index, std::uint64_t now) noexcept;
export ces_action ces_tcp_on_event(ces_tcp_engine* engine, const ces_event& event, std::uint64_t now) noexcept;
export ces_action ces_tcp_close(ces_tcp_engine* engine, std::uint32_t index, std::uint64_t now) noexcept;
export ces_worker_lifecycle ces_tcp_lifecycle(const ces_tcp_engine* engine) noexcept;
export bool                 ces_tcp_may_exit(const ces_tcp_engine* engine) noexcept;

static constexpr std::uint32_t ces_timer_absent = UINT32_MAX;

static bool ces_timer_less(const ces_timer_node& left, const ces_timer_node& right) noexcept {
    return left.deadline < right.deadline ||
           (left.deadline == right.deadline && left.connection_index < right.connection_index);
}

static void ces_timer_swap(ces_timer_heap* heap, std::uint32_t left, std::uint32_t right) noexcept {
    std::swap(heap->nodes[left], heap->nodes[right]);
    heap->positions[heap->nodes[left].connection_index]  = left;
    heap->positions[heap->nodes[right].connection_index] = right;
}

static void ces_timer_sift_up(ces_timer_heap* heap, std::uint32_t position) noexcept {
    while (position != 0) {
        const std::uint32_t parent = (position - 1U) / 2U;
        if (!ces_timer_less(heap->nodes[position], heap->nodes[parent])) {
            break;
        }
        ces_timer_swap(heap, position, parent);
        position = parent;
    }
}

static void ces_timer_sift_down(ces_timer_heap* heap, std::uint32_t position) noexcept {
    for (;;) {
        const std::uint32_t left = position * 2U + 1U;
        if (left >= heap->size) {
            return;
        }
        const std::uint32_t right    = left + 1U;
        std::uint32_t       smallest = left;
        if (right < heap->size && ces_timer_less(heap->nodes[right], heap->nodes[left])) {
            smallest = right;
        }
        if (!ces_timer_less(heap->nodes[smallest], heap->nodes[position])) {
            return;
        }
        ces_timer_swap(heap, position, smallest);
        position = smallest;
    }
}

bool ces_timer_initialize(ces_timer_heap* heap,
                          ces_timer_node* nodes,
                          std::uint32_t*  positions,
                          std::uint32_t   capacity) noexcept {
    if (heap == nullptr || nodes == nullptr || positions == nullptr || capacity == 0) {
        return false;
    }
    heap->nodes     = nodes;
    heap->positions = positions;
    heap->size      = 0;
    heap->capacity  = capacity;
    std::fill_n(positions, capacity, ces_timer_absent);
    return true;
}

bool ces_timer_insert_or_update(ces_timer_heap* heap, std::uint32_t connection_index, std::uint64_t deadline) noexcept {
    if (heap == nullptr || connection_index >= heap->capacity) {
        return false;
    }
    const std::uint32_t present = heap->positions[connection_index];
    if (present != ces_timer_absent) {
        const std::uint64_t previous  = heap->nodes[present].deadline;
        heap->nodes[present].deadline = deadline;
        if (deadline < previous) {
            ces_timer_sift_up(heap, present);
        } else {
            ces_timer_sift_down(heap, present);
        }
        return true;
    }
    if (heap->size == heap->capacity) {
        return false;
    }
    const std::uint32_t position      = heap->size++;
    heap->nodes[position]             = ces_timer_node{ deadline, connection_index };
    heap->positions[connection_index] = position;
    ces_timer_sift_up(heap, position);
    return true;
}

bool ces_timer_remove(ces_timer_heap* heap, std::uint32_t connection_index) noexcept {
    if (heap == nullptr || connection_index >= heap->capacity) {
        return false;
    }
    const std::uint32_t position = heap->positions[connection_index];
    if (position == ces_timer_absent) {
        return false;
    }
    heap->positions[connection_index] = ces_timer_absent;
    --heap->size;
    if (position == heap->size) {
        return true;
    }
    heap->nodes[position]                                   = heap->nodes[heap->size];
    heap->positions[heap->nodes[position].connection_index] = position;
    if (position != 0 && ces_timer_less(heap->nodes[position], heap->nodes[(position - 1U) / 2U])) {
        ces_timer_sift_up(heap, position);
    } else {
        ces_timer_sift_down(heap, position);
    }
    return true;
}

bool ces_timer_pop_expired(ces_timer_heap* heap, std::uint64_t now, std::uint32_t* connection_index) noexcept {
    if (heap == nullptr || connection_index == nullptr || heap->size == 0 || heap->nodes[0].deadline > now) {
        return false;
    }
    *connection_index = heap->nodes[0].connection_index;
    return ces_timer_remove(heap, *connection_index);
}

std::uint32_t ces_timer_wait_milliseconds(const ces_timer_heap* heap, std::uint64_t now) noexcept {
    if (heap == nullptr || heap->size == 0) {
        return UINT32_MAX;
    }
    const std::uint64_t deadline = heap->nodes[0].deadline;
    if (deadline <= now) {
        return 0;
    }
    const std::uint64_t remaining = deadline - now;
    const std::uint64_t maximum   = static_cast<std::uint64_t>(UINT32_MAX) - 1ULL;
    return static_cast<std::uint32_t>(std::min(remaining, maximum));
}

bool ces_connection_is_active(const ces_connection* connection) noexcept {
    return connection->state == ces_connection_state::receiving || connection->state == ces_connection_state::sending ||
           connection->state == ces_connection_state::closing;
}

static ces_action ces_tcp_none(std::uint32_t index) noexcept {
    return { ces_action_kind::none, index, 0, 0, {} };
}

static ces_action ces_tcp_fatal(std::uint32_t index, const wchar_t* stage, int error = 13) noexcept {
    return {
        ces_action_kind::fatal, index, 0, 0, { stage, error }
    };
}

static ces_action ces_tcp_schedule(ces_tcp_engine* engine, ces_connection* connection, std::uint64_t now) noexcept {
    connection->deadline = ces_saturating_add(now, engine->timeout_milliseconds);
    if (!ces_timer_insert_or_update(&engine->timers, connection->index, connection->deadline)) {
        return ces_tcp_fatal(connection->index, L"server timer insert/update");
    }
    connection->outstanding = 1;
    ++engine->outstanding;
    if (connection->state == ces_connection_state::receiving) {
        connection->operation = ces_engine_operation::receive;
        return { ces_action_kind::receive, connection->index, 0, 0, {} };
    }
    connection->operation = ces_engine_operation::send;
    return { ces_action_kind::send,
             connection->index,
             connection->send_offset,
             connection->echo_bytes - connection->send_offset,
             {} };
}

static ces_action ces_tcp_release(ces_tcp_engine* engine, ces_connection* connection) noexcept {
    if (connection->outstanding != 0 || engine->active_count == 0 || engine->free_count >= engine->slot_count) {
        return ces_tcp_fatal(connection->index, L"TCP connection release precondition", 996);
    }
    (void) ces_timer_remove(&engine->timers, connection->index);
    connection->state                          = ces_connection_state::idle;
    connection->echo_bytes                     = 0;
    connection->send_offset                    = 0;
    connection->deadline                       = 0;
    engine->free_indices[engine->free_count++] = connection->index;
    --engine->active_count;
    return { ces_action_kind::release, connection->index, 0, 0, {} };
}

bool ces_tcp_initialize(ces_tcp_engine* engine,
                        ces_connection* connections,
                        std::uint32_t*  free_indices,
                        ces_timer_node* timers,
                        std::uint32_t*  positions,
                        std::uint32_t   count,
                        std::uint64_t   timeout_milliseconds) noexcept {
    if (engine == nullptr || connections == nullptr || free_indices == nullptr ||
        !ces_timer_initialize(&engine->timers, timers, positions, count)) {
        return false;
    }
    engine->connections          = connections;
    engine->free_indices         = free_indices;
    engine->slot_count           = count;
    engine->free_count           = count;
    engine->timeout_milliseconds = timeout_milliseconds;
    engine->phase                = ces_worker_phase::starting;
    for (std::uint32_t index = 0; index < count; ++index) {
        connections[index]  = { ces_connection_state::idle, ces_engine_operation::receive, index, 0, 0, 0, 0 };
        free_indices[index] = count - index - 1U;
    }
    return true;
}

bool ces_tcp_take_slot(ces_tcp_engine* engine, std::uint32_t* index) noexcept {
    if (engine->stopping || engine->free_count == 0 || index == nullptr) {
        return false;
    }
    *index = engine->free_indices[--engine->free_count];
    return true;
}

bool ces_tcp_return_slot(ces_tcp_engine* engine, std::uint32_t index) noexcept {
    if (index < engine->slot_count && engine->free_count < engine->slot_count) {
        engine->connections[index].state           = ces_connection_state::idle;
        engine->free_indices[engine->free_count++] = index;
        return true;
    }
    return false;
}

ces_action ces_tcp_admit(ces_tcp_engine* engine, std::uint32_t index, std::uint64_t now) noexcept {
    if (index >= engine->slot_count || engine->connections[index].state != ces_connection_state::idle) {
        return ces_tcp_fatal(index, L"TCP admission state");
    }
    ces_connection* connection = &engine->connections[index];
    connection->state          = ces_connection_state::receiving;
    connection->echo_bytes     = 0;
    connection->send_offset    = 0;
    ++engine->active_count;
    ++engine->statistics.accepted;
    return ces_tcp_schedule(engine, connection, now);
}

ces_action ces_tcp_close(ces_tcp_engine* engine, std::uint32_t index, std::uint64_t now) noexcept {
    if (index >= engine->slot_count) {
        return ces_tcp_fatal(index, L"TCP close index");
    }
    ces_connection* connection = &engine->connections[index];
    if (!ces_connection_is_active(connection) || connection->state == ces_connection_state::closing) {
        return ces_tcp_none(index);
    }
    connection->state = ces_connection_state::closing;
    (void) ces_timer_remove(&engine->timers, index);
    if (connection->outstanding == 0) {
        return ces_tcp_release(engine, connection);
    }
    connection->deadline = ces_saturating_add(now, CES_DRAIN_GRACE_MS);
    if (!ces_timer_insert_or_update(&engine->timers, index, connection->deadline)) {
        return ces_tcp_fatal(index, L"ces_timer_insert_or_update(drain)");
    }
    return { ces_action_kind::close, index, 0, 0, {} };
}

ces_action ces_tcp_on_event(ces_tcp_engine* engine, const ces_event& event, std::uint64_t now) noexcept {
    if (event.kind == ces_event_kind::stop) {
        engine->stopping = true;
        engine->phase    = engine->admission_closed ? ces_worker_phase::draining : ces_worker_phase::quiescing;
        return ces_tcp_none(0);
    }
    if (event.kind == ces_event_kind::admission_closed) {
        engine->admission_closed = true;
        engine->phase            = engine->stopping ? ces_worker_phase::draining : ces_worker_phase::admission_closed;
        return ces_tcp_none(0);
    }
    const std::uint32_t index = event.completion.index;
    if (index >= engine->slot_count) {
        return ces_tcp_fatal(index, L"TCP event index");
    }
    ces_connection* connection = &engine->connections[index];
    if (event.kind == ces_event_kind::timeout) {
        if (connection->state == ces_connection_state::closing && connection->outstanding != 0) {
            return ces_tcp_fatal(index, L"TCP connection drain", 1460);
        }
        return ces_tcp_close(engine, index, now);
    }
    if (!ces_connection_is_active(connection) || connection->outstanding != 1 || engine->outstanding == 0) {
        return ces_tcp_fatal(index, L"worker RIO outstanding count");
    }
    if (event.kind == ces_event_kind::post_failed) {
        --connection->outstanding;
        --engine->outstanding;
        ++engine->statistics.network_errors;
        return ces_tcp_close(engine, index, now);
    }
    if (event.kind != ces_event_kind::completion || connection->operation != event.completion.operation) {
        return ces_tcp_fatal(index, L"TCP completion operation");
    }
    --connection->outstanding;
    --engine->outstanding;
    ces_statistics_record_completion(&engine->statistics, event.completion.operation, event.completion.status,
                                     event.completion.bytes, connection->state == ces_connection_state::closing);
    if (connection->state == ces_connection_state::closing) {
        return ces_tcp_release(engine, connection);
    }
    if (event.completion.status != 0 ||
        (event.completion.operation == ces_engine_operation::receive && event.completion.bytes == 0)) {
        return ces_tcp_close(engine, index, now);
    }
    if (event.completion.operation == ces_engine_operation::receive) {
        connection->echo_bytes  = event.completion.bytes;
        connection->send_offset = 0;
        connection->state       = ces_connection_state::sending;
    } else {
        std::size_t offset = connection->send_offset;
        if (!ces_advance_offset(connection->echo_bytes, event.completion.bytes, &offset)) {
            ++engine->statistics.network_errors;
            return ces_tcp_close(engine, index, now);
        }
        connection->send_offset = static_cast<std::uint32_t>(offset);
        if (connection->send_offset == connection->echo_bytes) {
            connection->state = ces_connection_state::receiving;
        }
    }
    return ces_tcp_schedule(engine, connection, now);
}

ces_worker_lifecycle ces_tcp_lifecycle(const ces_tcp_engine* engine) noexcept {
    return { engine->phase, engine->active_count, 0, engine->outstanding };
}

bool ces_tcp_may_exit(const ces_tcp_engine* engine) noexcept {
    const ces_worker_lifecycle lifecycle = ces_tcp_lifecycle(engine);
    return engine->stopping && engine->admission_closed && ces_worker_may_exit(&lifecycle) && engine->timers.size == 0;
}
