#pragma once

#include "ces_types.h"

// WinSock2.h must be included before the Windows networking headers below.
// clang-format off
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
// clang-format on

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

enum class ces_worker_phase : std::uint8_t {
    starting         = 0,
    running          = 1,
    quiescing        = 2,
    admission_closed = 3,
    draining         = 4,
    stopped          = 5
};

enum class ces_udp_phase : std::uint8_t { running = 0, draining = 1, stopped = 2 };

struct ces_worker_lifecycle {
    ces_worker_phase phase;
    std::uint32_t    active_connections;
    std::uint32_t    pending_handoffs;
    bool             notification_armed;
};

struct ces_engine_statistics {
    std::uint64_t accepted;
    std::uint64_t completions;
    std::uint64_t receives;
    std::uint64_t sends;
    std::uint64_t bytes;
};

struct ces_timer_node {
    ULONGLONG     deadline;
    std::uint32_t connection_index;
};

struct ces_timer_heap {
    ces_timer_node* nodes;
    std::uint32_t*  positions;
    std::uint32_t   size;
    std::uint32_t   capacity;
};

enum class ces_engine_operation : std::uint8_t { receive, send };

enum class ces_accept_state : LONG { idle = 0, posted = 1, transit = 2 };

struct ces_engine_worker;
struct ces_engine_connection;
struct ces_engine_acceptor;
class ces_engine_worker_resources;
class ces_engine_acceptor_resources;

struct ces_engine_request {
    ces_engine_connection* connection;
    ces_engine_operation   operation;
};

struct ces_engine_accept_operation {
    OVERLAPPED                                              overlapped;
    ces_engine_acceptor*                                    owner;
    SOCKET                                                  socket;
    HANDLE                                                  accept_port;
    ces_accept_state                                        state;
    std::uint32_t                                           index;
    std::array<char, (sizeof(SOCKADDR_STORAGE) + 16U) * 2U> addresses;
};

struct ces_engine_connection {
    ces_engine_worker* owner;
    SOCKET             socket;
    RIO_RQ             request_queue;
    RIO_BUF            buffer;
    ces_engine_request request;
    std::size_t        echo_bytes;
    std::size_t        send_offset;
    std::uint32_t      index;
    std::uint32_t      outstanding;
    ULONGLONG          deadline;
    bool               active;
    bool               closing;
};

struct ces_engine_worker {
    ces_engine_worker_resources*        resources;
    const RIO_EXTENSION_FUNCTION_TABLE* rio;
    const ces_options*                  options;
    std::atomic<bool>*                  failed;
    HANDLE                              port;
    HANDLE                              thread;
    HANDLE                              ready_event;
    OVERLAPPED                          notification_overlapped;
    RIO_CQ                              completion_queue;
    RIO_BUFFERID                        registration;
    char*                               memory;
    ces_engine_connection*              connections;
    std::uint32_t*                      free_indices;
    ces_timer_node*                     timer_nodes;
    std::uint32_t*                      timer_positions;
    ces_timer_heap                      timers;
    std::uint32_t                       slot_count;
    std::uint32_t                       free_count;
    std::uint32_t                       active_count;
    std::uint32_t                       stride;
    std::uint32_t                       worker_index;
    std::uint64_t                       accepted_count;
    std::uint64_t                       completion_count;
    std::uint64_t                       receive_count;
    std::uint64_t                       send_count;
    std::uint64_t                       echoed_bytes;
    bool                                notification_armed;
    bool                                stopping;
    bool                                admission_closed;
    bool                                ready;
};

struct ces_engine_acceptor {
    ces_engine_acceptor_resources*      resources;
    const RIO_EXTENSION_FUNCTION_TABLE* rio;
    const ces_options*                  options;
    ces_engine_worker*                  workers;
    std::uint32_t                       worker_count;
    std::atomic<bool>*                  failed;
    HANDLE                              port;
    HANDLE                              thread;
    SOCKET                              listener;
    LPFN_ACCEPTEX                       accept_ex;
    LPFN_GETACCEPTEXSOCKADDRS           get_accept_addresses;
    ces_engine_accept_operation*        operations;
    std::uint32_t                       operation_count;
    std::uint32_t                       next_worker;
    bool                                stopping;
};

struct ces_engine_udp_slot {
    RIO_BUF              payload;
    RIO_BUF              remote_address;
    ces_engine_operation operation;
    bool                 outstanding;
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

class ces_socket_owner {
  public:
    ces_socket_owner() noexcept;
    explicit ces_socket_owner(SOCKET value) noexcept;
    ~ces_socket_owner() noexcept;
    ces_socket_owner(const ces_socket_owner&)            = delete;
    ces_socket_owner& operator=(const ces_socket_owner&) = delete;
    ces_socket_owner(ces_socket_owner&& other) noexcept;
    ces_socket_owner& operator=(ces_socket_owner&& other) noexcept;
    SOCKET            get() const noexcept;
    SOCKET            release() noexcept;
    void              reset(SOCKET value = INVALID_SOCKET) noexcept;

  private:
    SOCKET value_;
};

class ces_handle_owner {
  public:
    ces_handle_owner() noexcept;
    explicit ces_handle_owner(HANDLE value) noexcept;
    ~ces_handle_owner() noexcept;
    ces_handle_owner(const ces_handle_owner&)            = delete;
    ces_handle_owner& operator=(const ces_handle_owner&) = delete;
    ces_handle_owner(ces_handle_owner&& other) noexcept;
    ces_handle_owner& operator=(ces_handle_owner&& other) noexcept;
    HANDLE            get() const noexcept;
    HANDLE            release() noexcept;
    void              reset(HANDLE value = nullptr) noexcept;

  private:
    HANDLE value_;
};

class ces_virtual_arena_owner {
  public:
    ces_virtual_arena_owner() noexcept;
    explicit ces_virtual_arena_owner(void* value) noexcept;
    ~ces_virtual_arena_owner() noexcept;
    ces_virtual_arena_owner(const ces_virtual_arena_owner&)            = delete;
    ces_virtual_arena_owner& operator=(const ces_virtual_arena_owner&) = delete;
    ces_virtual_arena_owner(ces_virtual_arena_owner&& other) noexcept;
    ces_virtual_arena_owner& operator=(ces_virtual_arena_owner&& other) noexcept;
    void*                    get() const noexcept;
    void*                    release() noexcept;
    void                     reset(void* value = nullptr) noexcept;

  private:
    void* value_;
};

class ces_heap_owner {
  public:
    ces_heap_owner() noexcept;
    explicit ces_heap_owner(void* value) noexcept;
    ~ces_heap_owner() noexcept;
    ces_heap_owner(const ces_heap_owner&)            = delete;
    ces_heap_owner& operator=(const ces_heap_owner&) = delete;
    ces_heap_owner(ces_heap_owner&& other) noexcept;
    ces_heap_owner& operator=(ces_heap_owner&& other) noexcept;
    void*           get() const noexcept;
    void*           release() noexcept;
    void            reset(void* value = nullptr) noexcept;

  private:
    void* value_;
};

class ces_rio_registration_owner {
  public:
    ces_rio_registration_owner() noexcept;
    ces_rio_registration_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_BUFFERID value) noexcept;
    ~ces_rio_registration_owner() noexcept;
    ces_rio_registration_owner(const ces_rio_registration_owner&)            = delete;
    ces_rio_registration_owner& operator=(const ces_rio_registration_owner&) = delete;
    ces_rio_registration_owner(ces_rio_registration_owner&& other) noexcept;
    ces_rio_registration_owner& operator=(ces_rio_registration_owner&& other) noexcept;
    RIO_BUFFERID                get() const noexcept;
    RIO_BUFFERID                release() noexcept;
    void reset(const RIO_EXTENSION_FUNCTION_TABLE* rio = nullptr, RIO_BUFFERID value = RIO_INVALID_BUFFERID) noexcept;

  private:
    const RIO_EXTENSION_FUNCTION_TABLE* rio_;
    RIO_BUFFERID                        value_;
};

class ces_rio_cq_owner {
  public:
    ces_rio_cq_owner() noexcept;
    ces_rio_cq_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_CQ value) noexcept;
    ~ces_rio_cq_owner() noexcept;
    ces_rio_cq_owner(const ces_rio_cq_owner&)            = delete;
    ces_rio_cq_owner& operator=(const ces_rio_cq_owner&) = delete;
    ces_rio_cq_owner(ces_rio_cq_owner&& other) noexcept;
    ces_rio_cq_owner& operator=(ces_rio_cq_owner&& other) noexcept;
    RIO_CQ            get() const noexcept;
    RIO_CQ            release() noexcept;
    void              reset(const RIO_EXTENSION_FUNCTION_TABLE* rio = nullptr, RIO_CQ value = RIO_INVALID_CQ) noexcept;

  private:
    const RIO_EXTENSION_FUNCTION_TABLE* rio_;
    RIO_CQ                              value_;
};

class ces_engine_worker_resources {
  public:
    ces_handle_owner                    port;
    ces_handle_owner                    thread;
    ces_handle_owner                    ready_event;
    ces_virtual_arena_owner             arena;
    ces_rio_registration_owner          registration;
    ces_rio_cq_owner                    completion_queue;
    ces_heap_owner                      connections;
    ces_heap_owner                      free_indices;
    ces_heap_owner                      timer_nodes;
    ces_heap_owner                      timer_positions;
    std::unique_ptr<ces_socket_owner[]> connection_sockets;
};

class ces_engine_acceptor_resources {
  public:
    ces_socket_owner                    listener;
    ces_handle_owner                    port;
    ces_handle_owner                    thread;
    ces_heap_owner                      operations;
    std::unique_ptr<ces_socket_owner[]> operation_sockets;
};

[[noreturn]] void ces_engine_fail_fast(const wchar_t* stage, int error) noexcept;
void              ces_require_rio_notify_success(int status, const wchar_t* stage) noexcept;
ULONG             ces_require_valid_dequeue_count(ULONG count, const wchar_t* stage) noexcept;
void              ces_statistics_add(ces_engine_statistics* total, const ces_engine_statistics* value) noexcept;
bool              ces_worker_may_exit(const ces_worker_lifecycle* lifecycle) noexcept;
bool              ces_udp_may_release(ces_udp_phase phase, std::uint32_t outstanding) noexcept;
bool              ces_notification_packet_matches(ULONG_PTR         key,
                                                  const OVERLAPPED* overlapped,
                                                  ULONG_PTR         expected_key,
                                                  const OVERLAPPED* expected_overlapped) noexcept;
bool              ces_timer_initialize(ces_timer_heap* heap,
                                       ces_timer_node* nodes,
                                       std::uint32_t*  positions,
                                       std::uint32_t   capacity) noexcept;
bool  ces_timer_insert_or_update(ces_timer_heap* heap, std::uint32_t connection_index, ULONGLONG deadline) noexcept;
bool  ces_timer_remove(ces_timer_heap* heap, std::uint32_t connection_index) noexcept;
bool  ces_timer_pop_expired(ces_timer_heap* heap, ULONGLONG now, std::uint32_t* connection_index) noexcept;
DWORD ces_timer_wait_milliseconds(const ces_timer_heap* heap, ULONGLONG now) noexcept;
