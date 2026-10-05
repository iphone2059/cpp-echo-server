#include "ces_engine_internal.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>

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

[[noreturn]] void ces_engine_fail_fast(const wchar_t* stage, int error) noexcept {
    std::fwprintf(stderr, L"%ls failed: native_error=%d\n", stage, error);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(ces_exit_code::internal));
    __assume(0);
}

void ces_require_rio_notify_success(int status, const wchar_t* stage) noexcept {
    if (status != ERROR_SUCCESS) {
        ces_engine_fail_fast(stage, status);
    }
}

ULONG ces_require_valid_dequeue_count(ULONG count, const wchar_t* stage) noexcept {
    if (count == RIO_CORRUPT_CQ) {
        ces_engine_fail_fast(stage, ERROR_INVALID_DATA);
    }
    return count;
}

void ces_statistics_add(ces_engine_statistics* total, const ces_engine_statistics* value) noexcept {
    if (total == nullptr || value == nullptr) {
        return;
    }
    total->accepted += value->accepted;
    total->completions += value->completions;
    total->receives += value->receives;
    total->sends += value->sends;
    total->bytes += value->bytes;
    total->received_bytes += value->received_bytes;
    total->sent_bytes += value->sent_bytes;
    total->network_errors += value->network_errors;
    total->rejected += value->rejected;
}

void ces_statistics_record_completion(ces_engine_statistics* statistics,
                                      ces_engine_operation   operation,
                                      LONG                   status,
                                      ULONG                  bytes_transferred,
                                      bool                   closing) noexcept {
    ++statistics->completions;
    if (status != ERROR_SUCCESS) {
        if (!closing) {
            ++statistics->network_errors;
        }
        return;
    }
    if (operation == ces_engine_operation::receive) {
        ++statistics->receives;
        statistics->received_bytes += bytes_transferred;
    } else {
        ++statistics->sends;
        statistics->sent_bytes += bytes_transferred;
        statistics->bytes += bytes_transferred;
    }
}

std::uint32_t ces_accept_operation_count(std::uint32_t worker_count) noexcept {
    constexpr std::uint32_t minimum    = 8U;
    constexpr std::uint32_t maximum    = 128U;
    constexpr std::uint32_t per_worker = 2U;
    return worker_count >= maximum / per_worker ? maximum : std::max(minimum, worker_count * per_worker);
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
    if (previous >= worker->slot_count) {
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
        const std::uint32_t index = (acceptor->next_worker + offset) % acceptor->worker_count;
        if (ces_worker_try_reserve_admission(&acceptor->workers[index])) {
            acceptor->next_worker = (index + 1U) % acceptor->worker_count;
            return &acceptor->workers[index];
        }
    }
    return nullptr;
}

ces_socket_owner::ces_socket_owner() noexcept : value_(INVALID_SOCKET) {}

ces_socket_owner::ces_socket_owner(SOCKET value) noexcept : value_(value) {}

ces_socket_owner::~ces_socket_owner() noexcept {
    reset();
}

ces_socket_owner::ces_socket_owner(ces_socket_owner&& other) noexcept : value_(other.release()) {}

ces_socket_owner& ces_socket_owner::operator=(ces_socket_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

SOCKET ces_socket_owner::get() const noexcept {
    return value_;
}

SOCKET ces_socket_owner::release() noexcept {
    const SOCKET value = value_;
    value_             = INVALID_SOCKET;
    return value;
}

void ces_socket_owner::reset(SOCKET value) noexcept {
    if (value_ != INVALID_SOCKET) {
        closesocket(value_);
    }
    value_ = value;
}

ces_handle_owner::ces_handle_owner() noexcept : value_(nullptr) {}

ces_handle_owner::ces_handle_owner(HANDLE value) noexcept : value_(value) {}

ces_handle_owner::~ces_handle_owner() noexcept {
    reset();
}

ces_handle_owner::ces_handle_owner(ces_handle_owner&& other) noexcept : value_(other.release()) {}

ces_handle_owner& ces_handle_owner::operator=(ces_handle_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

HANDLE ces_handle_owner::get() const noexcept {
    return value_;
}

HANDLE ces_handle_owner::release() noexcept {
    const HANDLE value = value_;
    value_             = nullptr;
    return value;
}

void ces_handle_owner::reset(HANDLE value) noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
        CloseHandle(value_);
    }
    value_ = value;
}

ces_virtual_arena_owner::ces_virtual_arena_owner() noexcept : value_(nullptr) {}

ces_virtual_arena_owner::ces_virtual_arena_owner(void* value) noexcept : value_(value) {}

ces_virtual_arena_owner::~ces_virtual_arena_owner() noexcept {
    reset();
}

ces_virtual_arena_owner::ces_virtual_arena_owner(ces_virtual_arena_owner&& other) noexcept : value_(other.release()) {}

ces_virtual_arena_owner& ces_virtual_arena_owner::operator=(ces_virtual_arena_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

void* ces_virtual_arena_owner::get() const noexcept {
    return value_;
}

void* ces_virtual_arena_owner::release() noexcept {
    void* value = value_;
    value_      = nullptr;
    return value;
}

void ces_virtual_arena_owner::reset(void* value) noexcept {
    if (value_ != nullptr) {
        VirtualFree(value_, 0, MEM_RELEASE);
    }
    value_ = value;
}

ces_heap_owner::ces_heap_owner() noexcept : value_(nullptr) {}

ces_heap_owner::ces_heap_owner(void* value) noexcept : value_(value) {}

ces_heap_owner::~ces_heap_owner() noexcept {
    reset();
}

ces_heap_owner::ces_heap_owner(ces_heap_owner&& other) noexcept : value_(other.release()) {}

ces_heap_owner& ces_heap_owner::operator=(ces_heap_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

void* ces_heap_owner::get() const noexcept {
    return value_;
}

void* ces_heap_owner::release() noexcept {
    void* value = value_;
    value_      = nullptr;
    return value;
}

void ces_heap_owner::reset(void* value) noexcept {
    if (value_ != nullptr) {
        HeapFree(GetProcessHeap(), 0, value_);
    }
    value_ = value;
}

ces_rio_registration_owner::ces_rio_registration_owner() noexcept : rio_(nullptr), value_(RIO_INVALID_BUFFERID) {}

ces_rio_registration_owner::ces_rio_registration_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                                       RIO_BUFFERID                        value) noexcept :
    rio_(rio),
    value_(value) {}

ces_rio_registration_owner::~ces_rio_registration_owner() noexcept {
    reset();
}

ces_rio_registration_owner::ces_rio_registration_owner(ces_rio_registration_owner&& other) noexcept :
    rio_(other.rio_),
    value_(other.release()) {
    other.rio_ = nullptr;
}

ces_rio_registration_owner& ces_rio_registration_owner::operator=(ces_rio_registration_owner&& other) noexcept {
    if (this != &other) {
        reset();
        rio_       = other.rio_;
        value_     = other.release();
        other.rio_ = nullptr;
    }
    return *this;
}

RIO_BUFFERID ces_rio_registration_owner::get() const noexcept {
    return value_;
}

RIO_BUFFERID ces_rio_registration_owner::release() noexcept {
    const RIO_BUFFERID value = value_;
    value_                   = RIO_INVALID_BUFFERID;
    return value;
}

void ces_rio_registration_owner::reset(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_BUFFERID value) noexcept {
    if (value_ != RIO_INVALID_BUFFERID && rio_ != nullptr) {
        rio_->RIODeregisterBuffer(value_);
    }
    rio_   = rio;
    value_ = value;
}

ces_rio_cq_owner::ces_rio_cq_owner() noexcept : rio_(nullptr), value_(RIO_INVALID_CQ) {}

ces_rio_cq_owner::ces_rio_cq_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_CQ value) noexcept :
    rio_(rio),
    value_(value) {}

ces_rio_cq_owner::~ces_rio_cq_owner() noexcept {
    reset();
}

ces_rio_cq_owner::ces_rio_cq_owner(ces_rio_cq_owner&& other) noexcept : rio_(other.rio_), value_(other.release()) {
    other.rio_ = nullptr;
}

ces_rio_cq_owner& ces_rio_cq_owner::operator=(ces_rio_cq_owner&& other) noexcept {
    if (this != &other) {
        reset();
        rio_       = other.rio_;
        value_     = other.release();
        other.rio_ = nullptr;
    }
    return *this;
}

RIO_CQ ces_rio_cq_owner::get() const noexcept {
    return value_;
}

RIO_CQ ces_rio_cq_owner::release() noexcept {
    const RIO_CQ value = value_;
    value_             = RIO_INVALID_CQ;
    return value;
}

void ces_rio_cq_owner::reset(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_CQ value) noexcept {
    if (value_ != RIO_INVALID_CQ && rio_ != nullptr) {
        rio_->RIOCloseCompletionQueue(value_);
    }
    rio_   = rio;
    value_ = value;
}

bool ces_worker_may_exit(const ces_worker_lifecycle* lifecycle) noexcept {
    return lifecycle != nullptr && lifecycle->phase >= ces_worker_phase::admission_closed &&
           lifecycle->active_connections == 0 && lifecycle->pending_handoffs == 0;
}

bool ces_udp_may_release(ces_udp_phase phase, std::uint32_t outstanding) noexcept {
    return phase == ces_udp_phase::stopped && outstanding == 0;
}

bool ces_notification_packet_matches(ULONG_PTR         key,
                                     const OVERLAPPED* overlapped,
                                     ULONG_PTR         expected_key,
                                     const OVERLAPPED* expected_overlapped) noexcept {
    return key == expected_key && overlapped == expected_overlapped;
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

bool ces_timer_insert_or_update(ces_timer_heap* heap, std::uint32_t connection_index, ULONGLONG deadline) noexcept {
    if (heap == nullptr || connection_index >= heap->capacity) {
        return false;
    }
    const std::uint32_t present = heap->positions[connection_index];
    if (present != ces_timer_absent) {
        const ULONGLONG previous      = heap->nodes[present].deadline;
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

bool ces_timer_pop_expired(ces_timer_heap* heap, ULONGLONG now, std::uint32_t* connection_index) noexcept {
    if (heap == nullptr || connection_index == nullptr || heap->size == 0 || heap->nodes[0].deadline > now) {
        return false;
    }
    *connection_index = heap->nodes[0].connection_index;
    return ces_timer_remove(heap, *connection_index);
}

DWORD ces_timer_wait_milliseconds(const ces_timer_heap* heap, ULONGLONG now) noexcept {
    if (heap == nullptr || heap->size == 0) {
        return INFINITE;
    }
    const ULONGLONG deadline = heap->nodes[0].deadline;
    if (deadline <= now) {
        return 0;
    }
    const ULONGLONG remaining = deadline - now;
    const ULONGLONG maximum   = static_cast<ULONGLONG>(INFINITE) - 1ULL;
    return static_cast<DWORD>(std::min(remaining, maximum));
}
