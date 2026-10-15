module;
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

export module ces.types;

export inline constexpr std::uint32_t CES_WIN32_TARGET                = 0x0A00U;
export inline constexpr std::uint32_t CES_ERROR_CAPACITY              = 256U;
export inline constexpr std::uint32_t CES_DEFAULT_PORT                = 7U;
export inline constexpr std::uint32_t CES_DEFAULT_TCP_TIMEOUT_SECONDS = 300U;
export inline constexpr std::uint32_t CES_DEFAULT_UDP_DEPTH           = 256U;
export inline constexpr std::uint32_t CES_DEFAULT_RIO_BUFFER_BYTES    = 16384U;
export inline constexpr std::uint32_t CES_DEFAULT_CQ_CAPACITY         = 4096U;
export inline constexpr std::uint64_t CES_DEFAULT_MEMORY_BYTES        = 1073741824ULL;
export inline constexpr std::uint32_t CES_MAXIMUM_UDP_PAYLOAD_BYTES   = 65507U;

static_assert(CHAR_BIT == 8);
static_assert(sizeof(wchar_t) == 2, "Windows requires 16-bit wchar_t");
#if !defined(_MSC_VER)
#    error cpp-echo-server requires MSVC
#endif
#if !defined(_MSVC_LANG) || _MSVC_LANG < 202302L
#    error cpp-echo-server requires MSVC latest C++ language mode
#endif

export inline constexpr std::uint32_t CES_MAX_WORKERS = 64U;

export enum class ces_protocol : std::uint8_t { none = 0, tcp = 1, udp = 2 };

export enum class ces_exit_code : int { success = 0, usage = 1, network = 2, echo_failure = 3, internal = 4 };

export struct ces_options {
    ces_protocol  protocol;
    std::uint16_t port;
    std::uint32_t timeout_seconds;
    std::uint32_t run_seconds;
    std::uint32_t socket_buffer_bytes;
    std::uint32_t udp_depth;
    std::uint32_t worker_count;
    std::uint32_t rio_buffer_bytes;
    std::uint32_t cq_capacity;
    std::uint64_t memory_bytes;
    bool          quiet;
    bool          stats;
    bool          help;
};

// Pure shared vocabulary. No Windows header or native resource belongs to this module.
export inline constexpr std::uint32_t CES_UDP_ADDRESS_BYTES        = 144U;
export inline constexpr std::uint32_t CES_COMPLETION_BATCH_SIZE    = 256U;
export inline constexpr std::uint32_t CES_COMPLETION_DRAIN_BATCHES = 64U;
export inline constexpr std::uint64_t CES_DRAIN_GRACE_MS           = 5000ULL;
export enum class ces_worker_phase : std::uint8_t { starting, running, quiescing, admission_closed, draining, stopped };
export enum class ces_udp_phase : std::uint8_t { running, draining, stopped };
export enum class ces_engine_operation : std::uint8_t { receive, send };
export enum class ces_connection_state : std::uint8_t { idle, receiving, sending, closing, released };
export enum class ces_accept_state : std::uint8_t { idle, posted, transit, deferred };
export enum class ces_handoff_disposition : std::uint8_t { withdraw, fatal, handoff };
export enum class ces_rio_notify_outcome : std::uint8_t { armed, duplicate_arm, invalid };
export enum class ces_event_kind : std::uint8_t { completion, post_failed, stop, admission_closed, timeout };
export enum class ces_action_kind : std::uint8_t { none, receive, send, close, release, fatal };

export struct ces_failure {
    const wchar_t* stage;
    int            native_error;
};

export struct ces_completion {
    std::uint32_t        index;
    ces_engine_operation operation;
    std::int32_t         status;
    std::uint32_t        bytes;
};

export struct ces_event {
    ces_event_kind kind;
    ces_completion completion;
    ces_failure    failure;
};

export struct ces_action {
    ces_action_kind kind;
    std::uint32_t   index;
    std::uint32_t   offset;
    std::uint32_t   length;
    ces_failure     failure;
};

export struct ces_worker_lifecycle {
    ces_worker_phase phase;
    std::uint32_t    active_connections;
    std::uint32_t    pending_handoffs;
    std::uint32_t    rio_outstanding;
};

export struct ces_engine_statistics {
    std::uint64_t accepted;
    std::uint64_t completions;
    std::uint64_t receives;
    std::uint64_t sends;
    std::uint64_t bytes;
    std::uint64_t received_bytes;
    std::uint64_t sent_bytes;
    std::uint64_t network_errors;
    std::uint64_t rejected;
};

export struct ces_timer_node {
    std::uint64_t deadline;
    std::uint32_t connection_index;
};

export struct ces_timer_heap {
    ces_timer_node* nodes;
    std::uint32_t*  positions;
    std::uint32_t   size;
    std::uint32_t   capacity;
};

export struct ces_notify_diagnostic_snapshot {
    std::uint32_t worker;
    std::uint64_t arms;
    std::uint64_t deliveries;
    std::uint64_t timeout_wakeups;
};

static_assert(std::is_trivial_v<ces_worker_lifecycle>);
static_assert(std::is_standard_layout_v<ces_worker_lifecycle>);
static_assert(std::is_trivially_copyable_v<ces_worker_lifecycle>);
static_assert(std::is_trivial_v<ces_engine_statistics>);
static_assert(std::is_standard_layout_v<ces_engine_statistics>);
static_assert(std::is_trivially_copyable_v<ces_engine_statistics>);
static_assert(std::is_trivial_v<ces_timer_node>);
static_assert(std::is_standard_layout_v<ces_timer_node>);
static_assert(std::is_trivially_copyable_v<ces_timer_node>);
static_assert(std::is_trivial_v<ces_timer_heap>);
static_assert(std::is_standard_layout_v<ces_timer_heap>);
static_assert(std::is_trivially_copyable_v<ces_timer_heap>);

export bool                   ces_notification_mark_delivered(bool* armed) noexcept;
export bool                   ces_notification_mark_rearmed(bool* armed) noexcept;
export ces_rio_notify_outcome ces_rio_notify_outcome_of(int status) noexcept;
export bool                   ces_notify_should_arm(bool armed, std::uint32_t outstanding) noexcept;
export bool                   ces_worker_may_exit(const ces_worker_lifecycle* lifecycle) noexcept;
export bool                   ces_udp_may_release(ces_udp_phase phase, std::uint32_t outstanding) noexcept;
export void          ces_statistics_add(ces_engine_statistics* total, const ces_engine_statistics* value) noexcept;
export void          ces_statistics_record_completion(ces_engine_statistics* statistics,
                                                      ces_engine_operation   operation,
                                                      std::int32_t           status,
                                                      std::uint32_t          bytes_transferred,
                                                      bool                   closing) noexcept;
export std::uint64_t ces_saturating_add(std::uint64_t left, std::uint64_t right) noexcept;
export bool          ces_advance_offset(std::size_t total, std::size_t transferred, std::size_t* offset) noexcept;

bool ces_notification_mark_delivered(bool* armed) noexcept {
    if (armed == nullptr || !*armed) {
        return false;
    }
    *armed = false;
    return true;
}

bool ces_notification_mark_rearmed(bool* armed) noexcept {
    if (armed == nullptr || *armed) {
        return false;
    }
    *armed = true;
    return true;
}

ces_rio_notify_outcome ces_rio_notify_outcome_of(int status) noexcept {
    if (status == 0) {
        return ces_rio_notify_outcome::armed;
    }
    if (status == 10037) {
        return ces_rio_notify_outcome::duplicate_arm;
    }
    return ces_rio_notify_outcome::invalid;
}

bool ces_notify_should_arm(bool armed, std::uint32_t outstanding) noexcept {
    return !armed && outstanding != 0U;
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
                                      std::int32_t           status,
                                      std::uint32_t          bytes_transferred,
                                      bool                   closing) noexcept {
    ++statistics->completions;
    if (status != 0) {
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

bool ces_worker_may_exit(const ces_worker_lifecycle* lifecycle) noexcept {
    return lifecycle != nullptr && lifecycle->phase >= ces_worker_phase::admission_closed &&
           lifecycle->active_connections == 0 && lifecycle->pending_handoffs == 0 && lifecycle->rio_outstanding == 0;
}

bool ces_udp_may_release(ces_udp_phase phase, std::uint32_t outstanding) noexcept {
    return phase == ces_udp_phase::stopped && outstanding == 0;
}

std::uint64_t ces_saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max() :
                                                                      left + right;
}

bool ces_advance_offset(std::size_t total, std::size_t transferred, std::size_t* offset) noexcept {
    if (offset == nullptr || transferred == 0 || *offset > total || transferred > total - *offset) {
        return false;
    }
    *offset += transferred;
    return true;
}
