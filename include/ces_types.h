#pragma once

#include "ces_compiler_contract.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

inline constexpr std::uint32_t CES_MAX_WORKERS = 64U;

enum class ces_protocol : std::uint8_t { none = 0, tcp = 1, udp = 2 };

enum class ces_exit_code : int { success = 0, usage = 1, network = 2, echo_failure = 3, internal = 4 };

struct ces_options {
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

bool          ces_parse_options(int             argc,
                                wchar_t* const* argv,
                                ces_options*    options,
                                wchar_t*        error,
                                std::size_t     error_capacity) noexcept;
bool          ces_checked_product(std::size_t left, std::size_t right, std::size_t* product) noexcept;
bool          ces_checked_arena_bytes(std::size_t   slots,
                                      std::size_t   stride,
                                      std::uint64_t memory_limit,
                                      std::size_t*  bytes) noexcept;
std::uint32_t ces_tcp_connection_capacity(std::uint32_t cq_capacity, std::uint64_t memory_slots) noexcept;
bool          ces_advance_offset(std::size_t total, std::size_t transferred, std::size_t* offset) noexcept;
bool          ces_notification_mark_delivered(bool* armed) noexcept;
bool          ces_notification_mark_rearmed(bool* armed) noexcept;

// Documented RIONotify outcomes: ERROR_SUCCESS arms the queue, WSAEALREADY means a previous
// RIONotify has not completed yet (a state-machine invariant failure, never a recovery branch),
// and everything else is a hard error.
enum class ces_rio_notify_outcome : std::uint8_t { armed = 0, duplicate_arm = 1, invalid = 2 };

ces_rio_notify_outcome ces_rio_notify_outcome_of(int status) noexcept;

// Lazy-arm policy: a worker arms the completion queue only when work is outstanding and no
// notification is already pending, so "armed" always means exactly one RIONotify is in flight.
bool          ces_notify_should_arm(bool armed, std::uint32_t outstanding) noexcept;
ces_exit_code ces_run_server(const ces_options* options, std::atomic<bool>* stop_requested) noexcept;
