module;
// WinSock2 must precede the Windows networking headers.
// clang-format off
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
// clang-format on
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

export module ces.native;

import ces.types;
import ces.contract;

// WinSock2 must precede Windows networking headers.
// clang-format off
// clang-format on

export inline constexpr ULONG_PTR     ces_engine_stop_key                  = 1U;
export inline constexpr ULONG_PTR     ces_engine_admission_closed_key      = 2U;
export inline constexpr ULONG         ces_engine_batch_size                = CES_COMPLETION_BATCH_SIZE;
export inline constexpr std::uint32_t ces_engine_max_drain_batches         = CES_COMPLETION_DRAIN_BATCHES;
export inline constexpr std::uint32_t ces_engine_max_inline_accept_retries = 4U;
export inline constexpr ULONGLONG     ces_engine_drain_grace_ms            = CES_DRAIN_GRACE_MS;
static_assert(sizeof(SOCKADDR_STORAGE) + 16U == CES_UDP_ADDRESS_BYTES);

export class ces_engine_winsock {
  public:
    bool started = false;
    int  start() noexcept;
    ~ces_engine_winsock() noexcept;
};
export struct ces_native_worker;
export struct ces_engine_connection;
export struct ces_native_acceptor;
export class ces_engine_worker_resources;
export class ces_engine_acceptor_resources;

export struct ces_engine_request {
    ces_engine_connection* connection;
    ces_engine_operation   operation;
    /// The generation this stable request context belongs to. The context lives inside the
    /// connection record, so its address never changes between generations and the tag travels with
    /// it, letting the retire path reject a completion that belongs to a generation the slot has
    /// already left.
    std::uint32_t          generation = 0;
};

export struct ces_engine_accept_operation {
    OVERLAPPED                                              overlapped;
    ces_native_acceptor*                                    owner;
    SOCKET                                                  socket;
    HANDLE                                                  accept_port;
    bool                                                    posted;
    bool                                                    control_pending;
    std::uint32_t                                           index;
    std::array<char, (sizeof(SOCKADDR_STORAGE) + 16U) * 2U> addresses;
};

export struct ces_engine_connection {
    ces_native_worker* owner;
    SOCKET             socket;
    RIO_RQ             request_queue;
    RIO_BUF            buffer;
    ces_engine_request request;
    std::uint32_t      index;
    /// Identity of the generation currently admitted into this slot. It changes only where a
    /// handoff adopts the slot, once the free pool proved nothing of the previous one is in flight.
    std::uint32_t      generation = 0;
    std::uint32_t      outstanding;
};

export struct ces_native_worker {
    ~ces_native_worker() noexcept;
    // Native storage is constructed once; it contains no business state or admission credit.
    ces_engine_worker_resources*        resources   = nullptr;
    const RIO_EXTENSION_FUNCTION_TABLE* rio         = nullptr;
    HANDLE                              port        = nullptr;
    HANDLE                              thread      = nullptr;
    HANDLE                              ready_event = nullptr;
    OVERLAPPED                          notification_overlapped{};
    RIO_CQ                              completion_queue       = RIO_INVALID_CQ;
    char*                               memory                 = nullptr;
    ces_engine_connection*              connections            = nullptr;
    std::uint32_t                       slot_count             = 0;
    // RIO posts that have not completed yet; the arm policy keys off this counter and the release
    // precondition requires it to be zero before the completion queue is closed.
    std::uint32_t                       rio_outstanding        = 0;
    // Notification accounting. A bounded wait that expires while RIO work is outstanding and no
    // notification is armed means the queue was never armed for that work, so a non-zero starvation
    // count fails in every build instead of silently degrading to timeout polling.
    std::uint64_t                       notify_arms            = 0;
    std::uint64_t                       notify_deliveries      = 0;
    std::uint64_t                       notify_timeout_wakeups = 0;
    std::uint32_t                       stride                 = 0;
    bool                                notification_armed     = false;
    // The accept table this worker can receive handoffs from, published by the acceptor before its
    // thread starts. A handoff control packet carries an accept operation address, and the worker
    // range-, alignment- and owner-checks it against these before it dereferences anything.
    const ces_engine_accept_operation*  accept_operations      = nullptr;
    std::uint32_t                       accept_operation_count = 0;
    const void*                         accept_owner           = nullptr;
};

export struct ces_native_acceptor {
    ~ces_native_acceptor() noexcept;
    ces_engine_acceptor_resources*      resources            = nullptr;
    const RIO_EXTENSION_FUNCTION_TABLE* rio                  = nullptr;
    HANDLE                              port                 = nullptr;
    HANDLE                              thread               = nullptr;
    SOCKET                              listener             = INVALID_SOCKET;
    LPFN_ACCEPTEX                       accept_ex            = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS           get_accept_addresses = nullptr;
    ces_engine_accept_operation*        operations           = nullptr;
    std::uint32_t                       operation_count      = 0;
};

export struct ces_engine_udp_slot {
    RIO_BUF              payload;
    RIO_BUF              remote_address;
    ces_engine_operation operation;
    bool                 outstanding;
};

export class ces_socket_owner {
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

export class ces_handle_owner {
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

export class ces_virtual_arena_owner {
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

export class ces_heap_owner {
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

export class ces_rio_registration_owner {
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

export class ces_rio_cq_owner {
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

export class ces_engine_worker_resources {
  public:
    ces_native_worker* lifetime = nullptr;
    ~ces_engine_worker_resources() noexcept;
    ces_handle_owner                              port;
    ces_handle_owner                              thread;
    ces_handle_owner                              ready_event;
    ces_virtual_arena_owner                       arena;
    std::unique_ptr<ces_rio_registration_owner[]> registrations;
    ces_rio_cq_owner                              completion_queue;
    ces_heap_owner                                connections;
    std::unique_ptr<ces_socket_owner[]>           connection_sockets;
};

export class ces_engine_acceptor_resources {
  public:
    ces_native_acceptor* lifetime = nullptr;
    ~ces_engine_acceptor_resources() noexcept;
    ces_socket_owner                    listener;
    ces_handle_owner                    port;
    ces_handle_owner                    thread;
    ces_heap_owner                      operations;
    std::unique_ptr<ces_socket_owner[]> operation_sockets;
};

export [[noreturn]] void ces_engine_fail_fast(const wchar_t* stage, int error) noexcept;
export void              ces_require_rio_notify_success(int status, const wchar_t* stage) noexcept;

// One RIONotify transaction: precondition, provider call, status classification, armed transition
// and arm accounting. TCP workers and the UDP loop share it, and a fake provider can drive it through
// RIO_EXTENSION_FUNCTION_TABLE::RIONotify without a socket, a port or a completion queue.
export void ces_notification_arm(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                 RIO_CQ                              completion_queue,
                                 bool*                               armed,
                                 std::uint64_t*                      arm_count,
                                 const wchar_t*                      stage) noexcept;

export void   ces_write_diagnostics(const ces_notify_diagnostic_snapshot* values,
                                    std::uint32_t                         count,
                                    ces_protocol                          protocol) noexcept;
export ULONG  ces_require_valid_dequeue_count(ULONG count, const wchar_t* stage) noexcept;
export bool   ces_register_windows(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                   char*                               memory,
                                   std::size_t                         window_bytes,
                                   std::uint32_t                       count,
                                   ces_rio_registration_owner*         registrations) noexcept;
export bool   ces_prepare_send_window(RIO_BUF* buffer, std::size_t total, std::size_t offset) noexcept;
export bool   ces_notification_packet_matches(ULONG_PTR         key,
                                              const OVERLAPPED* overlapped,
                                              ULONG_PTR         expected_key,
                                              const OVERLAPPED* expected_overlapped) noexcept;
export void   ces_engine_report(const wchar_t* stage, int error) noexcept;
export bool   ces_engine_load_rio(RIO_EXTENSION_FUNCTION_TABLE* table) noexcept;
export SOCKET ces_engine_registered_socket(int type, int protocol) noexcept;
export bool   ces_engine_configure_socket(SOCKET socket_value, const ces_options* options, bool tcp) noexcept;
export void   ces_engine_acceptor_listener_close(ces_native_acceptor* acceptor) noexcept;
export void   ces_engine_connection_socket_close(ces_engine_connection* connection) noexcept;
export void   ces_engine_accept_socket_close(ces_engine_accept_operation* operation) noexcept;
export void   ces_engine_owned_socket_close(ces_socket_owner* owner, SOCKET* mirror) noexcept;

export bool           ces_native_tcp_post(ces_native_worker*     worker,
                                          ces_engine_connection* connection,
                                          const ces_action&      action,
                                          ces_failure*           failure) noexcept;
export ces_completion ces_native_tcp_completion(ces_native_worker* worker, const RIORESULT& result) noexcept;

export bool           ces_native_udp_post(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                          RIO_RQ                              queue,
                                          ces_engine_udp_slot*                slot,
                                          const ces_action&                   action,
                                          std::uint32_t*                      outstanding) noexcept;
export ces_completion ces_native_udp_completion(ces_engine_udp_slot* slots,
                                                std::uint32_t        depth,
                                                std::uint32_t*       outstanding,
                                                const RIORESULT&     result) noexcept;

export void ces_native_worker_require_release(const ces_native_worker* worker) noexcept;
export void ces_native_acceptor_require_release(const ces_native_acceptor* acceptor) noexcept;

void ces_native_worker_require_release(const ces_native_worker* worker) noexcept {
    if (worker->thread != nullptr && WaitForSingleObject(worker->thread, 0) != WAIT_OBJECT_0) {
        ces_engine_fail_fast(L"worker release precondition", ERROR_IO_INCOMPLETE);
    }
    if (worker->rio_outstanding != 0) {
        ces_engine_fail_fast(L"worker release precondition", ERROR_IO_INCOMPLETE);
    }
    for (std::uint32_t index = 0; worker->connections != nullptr && index < worker->slot_count; ++index) {
        if (worker->connections[index].outstanding != 0) {
            ces_engine_fail_fast(L"worker release precondition", ERROR_IO_INCOMPLETE);
        }
    }
}

void ces_native_acceptor_require_release(const ces_native_acceptor* acceptor) noexcept {
    if (acceptor->thread != nullptr && WaitForSingleObject(acceptor->thread, 0) != WAIT_OBJECT_0) {
        ces_engine_fail_fast(L"TCP acceptor terminal operations", ERROR_IO_INCOMPLETE);
    }
    for (std::uint32_t index = 0; acceptor->operations != nullptr && index < acceptor->operation_count; ++index) {
        if (acceptor->operations[index].posted || acceptor->operations[index].control_pending) {
            ces_engine_fail_fast(L"TCP acceptor terminal operations", ERROR_IO_INCOMPLETE);
        }
    }
}

ces_engine_worker_resources::~ces_engine_worker_resources() noexcept {
    if (lifetime != nullptr) {
        ces_native_worker_require_release(lifetime);
        thread.reset();
        lifetime->thread = nullptr;
        completion_queue.reset();
        lifetime->completion_queue = RIO_INVALID_CQ;
        registrations.reset();
        arena.reset();
        lifetime->memory = nullptr;
        connection_sockets.reset();
        connections.reset();
        lifetime->connections = nullptr;
        ready_event.reset();
        lifetime->ready_event = nullptr;
        port.reset();
        lifetime->port      = nullptr;
        lifetime->resources = nullptr;
        lifetime            = nullptr;
    }
}

ces_engine_acceptor_resources::~ces_engine_acceptor_resources() noexcept {
    if (lifetime != nullptr) {
        ces_native_acceptor_require_release(lifetime);
        thread.reset();
        lifetime->thread = nullptr;
        listener.reset();
        lifetime->listener = INVALID_SOCKET;
        operation_sockets.reset();
        operations.reset();
        lifetime->operations = nullptr;
        port.reset();
        lifetime->port      = nullptr;
        lifetime->resources = nullptr;
        lifetime            = nullptr;
    }
}

ces_native_worker::~ces_native_worker() noexcept {
    ces_native_worker_require_release(this);
    if (completion_queue != RIO_INVALID_CQ || port != nullptr || resources != nullptr) {
        ces_engine_fail_fast(L"worker release precondition", ERROR_IO_INCOMPLETE);
    }
}

ces_native_acceptor::~ces_native_acceptor() noexcept {
    ces_native_acceptor_require_release(this);
    if (port != nullptr || resources != nullptr) {
        ces_engine_fail_fast(L"TCP acceptor terminal operations", ERROR_IO_INCOMPLETE);
    }
}

[[noreturn]] void ces_engine_fail_fast(const wchar_t* stage, int error) noexcept {
    char text[160]{};
    (void) WideCharToMultiByte(CP_UTF8, 0, stage, -1, text, sizeof(text), nullptr, nullptr);
    std::fprintf(stderr, "%s failed: native_error=%d\n", text, error);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(ces_exit_code::internal));
    __assume(0);
}

void ces_write_diagnostics(const ces_notify_diagnostic_snapshot* values,
                           std::uint32_t                         count,
                           ces_protocol                          protocol) noexcept {
    if (values == nullptr) {
        return;
    }
    const DWORD required = GetEnvironmentVariableW(L"CES_DIAG_FILE", nullptr, 0);
    if (required == 0) {
        return;
    }
    wchar_t path[512]{};
    if (static_cast<std::size_t>(required) > sizeof(path) / sizeof(path[0])) {
        return;
    }
    if (GetEnvironmentVariableW(L"CES_DIAG_FILE", path, static_cast<DWORD>(sizeof(path) / sizeof(path[0]))) == 0) {
        return;
    }
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const char* protocol_name = protocol == ces_protocol::tcp ? "tcp" : "udp";
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint64_t arms       = values[index].arms;
        const std::uint64_t deliveries = values[index].deliveries;
        const std::uint64_t gap        = arms >= deliveries ? arms - deliveries : 0U;
        char                line[192]{};
        const int           length =
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "role=server protocol=%s worker=%u notify_arms=%llu notify_deliveries=%llu notify_gap=%llu "
                        "timeout_wakeups_while_outstanding=%llu\n",
                        protocol_name, values[index].worker, static_cast<unsigned long long>(arms),
                        static_cast<unsigned long long>(deliveries), static_cast<unsigned long long>(gap),
                        static_cast<unsigned long long>(values[index].timeout_wakeups));
        if (length <= 0) {
            continue;
        }
        DWORD written = 0;
        if (WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr) == FALSE ||
            written != static_cast<DWORD>(length)) {
            break;
        }
    }
    (void) CloseHandle(file);
}

void ces_notification_arm(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                          RIO_CQ                              completion_queue,
                          bool*                               armed,
                          std::uint64_t*                      arm_count,
                          const wchar_t*                      stage) noexcept {
    if (rio == nullptr || rio->RIONotify == nullptr || armed == nullptr || arm_count == nullptr || stage == nullptr) {
        ces_engine_fail_fast(L"notification arm arguments", ERROR_INVALID_PARAMETER);
    }
    if (*armed) {
        ces_engine_fail_fast(L"duplicate notification arm", ERROR_INVALID_STATE);
    }
    const int status = rio->RIONotify(completion_queue);
    ces_require_rio_notify_success(status, stage);
    if (!ces_notification_mark_rearmed(armed)) {
        ces_engine_fail_fast(L"notification rearm transition", ERROR_INVALID_STATE);
    }
    ++*arm_count;
}

void ces_require_rio_notify_success(int status, const wchar_t* stage) noexcept {
    const ces_rio_notify_outcome outcome = ces_rio_notify_outcome_of(status);
    if (outcome == ces_rio_notify_outcome::armed) {
        return;
    }
    if (outcome == ces_rio_notify_outcome::duplicate_arm) {
        // A correct state machine never arms a queue whose previous notification is still pending,
        // so report this distinctly instead of folding it into a generic RIONotify failure.
        ces_engine_fail_fast(L"RIONotify duplicate arm", status);
    }
    ces_engine_fail_fast(stage, status);
}

ULONG ces_require_valid_dequeue_count(ULONG count, const wchar_t* stage) noexcept {
    if (count == RIO_CORRUPT_CQ) {
        ces_engine_fail_fast(stage, ERROR_INVALID_DATA);
    }
    return count;
}

bool ces_notification_packet_matches(ULONG_PTR         key,
                                     const OVERLAPPED* overlapped,
                                     ULONG_PTR         expected_key,
                                     const OVERLAPPED* expected_overlapped) noexcept {
    return key == expected_key && overlapped == expected_overlapped;
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
        // A failed release means Windows still holds the whole registered region: that is a release
        // boundary invariant, so it fails fast instead of being ignored.
        if (VirtualFree(value_, 0, MEM_RELEASE) == 0) {
            ces_engine_fail_fast(L"VirtualFree(arena)", static_cast<int>(GetLastError()));
        }
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

bool ces_register_windows(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                          char*                               memory,
                          std::size_t                         window_bytes,
                          std::uint32_t                       count,
                          ces_rio_registration_owner*         registrations) noexcept {
    if (rio == nullptr || rio->RIORegisterBuffer == nullptr || rio->RIODeregisterBuffer == nullptr ||
        memory == nullptr || registrations == nullptr || count == 0 || window_bytes == 0 ||
        window_bytes > std::numeric_limits<DWORD>::max() ||
        count > std::numeric_limits<std::size_t>::max() / window_bytes) {
        return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const RIO_BUFFERID id = rio->RIORegisterBuffer(memory + static_cast<std::size_t>(index) * window_bytes,
                                                       static_cast<DWORD>(window_bytes));
        if (id == RIO_INVALID_BUFFERID) {
            const int error = WSAGetLastError();
            for (std::uint32_t prior = 0; prior < index; ++prior) {
                registrations[prior].reset();
            }
            WSASetLastError(error);
            return false;
        }
        registrations[index].reset(rio, id);
    }
    return true;
}

bool ces_prepare_send_window(RIO_BUF* buffer, std::size_t total, std::size_t offset) noexcept {
    if (buffer == nullptr || buffer->BufferId == RIO_INVALID_BUFFERID || total > std::numeric_limits<ULONG>::max() ||
        offset >= total) {
        return false;
    }
    buffer->Offset = static_cast<ULONG>(offset);
    buffer->Length = static_cast<ULONG>(total - offset);
    return true;
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

void ces_engine_acceptor_listener_close(ces_native_acceptor* acceptor) noexcept {
    acceptor->resources->listener.reset();
    acceptor->listener = INVALID_SOCKET;
}

void ces_engine_connection_socket_close(ces_engine_connection* connection) noexcept {
    connection->owner->resources->connection_sockets[connection->index].reset();
    connection->socket = INVALID_SOCKET;
}

void ces_engine_accept_socket_close(ces_engine_accept_operation* operation) noexcept {
    operation->owner->resources->operation_sockets[operation->index].reset();
    operation->socket = INVALID_SOCKET;
}

void ces_engine_owned_socket_close(ces_socket_owner* owner, SOCKET* mirror) noexcept {
    owner->reset();
    *mirror = INVALID_SOCKET;
}

void ces_engine_report(const wchar_t* stage, int error) noexcept {
    char text[160]{};
    (void) WideCharToMultiByte(CP_UTF8, 0, stage, -1, text, sizeof(text), nullptr, nullptr);
    std::fprintf(stderr, "%s failed: native_error=%d\n", text, error);
}

bool ces_engine_load_rio(RIO_EXTENSION_FUNCTION_TABLE* table) noexcept {
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

SOCKET ces_engine_registered_socket(int type, int protocol) noexcept {
    return WSASocketW(AF_INET, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_REGISTERED_IO);
}

bool ces_engine_configure_socket(SOCKET socket_value, const ces_options* options, bool tcp) noexcept {
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

int ces_engine_winsock::start() noexcept {
    WSADATA   data{};
    const int status = WSAStartup(MAKEWORD(2, 2), &data);
    started          = status == 0;
    return status;
}

ces_engine_winsock::~ces_engine_winsock() noexcept {
    if (started) {
        WSACleanup();
    }
}

bool ces_native_tcp_post(ces_native_worker*     worker,
                         ces_engine_connection* connection,
                         const ces_action&      action,
                         ces_failure*           failure) noexcept {
    if (connection->outstanding != 0) {
        ces_engine_fail_fast(L"worker duplicate RIO post", ERROR_INVALID_STATE);
    }
    connection->request.operation =
        action.kind == ces_action_kind::send ? ces_engine_operation::send : ces_engine_operation::receive;
    connection->buffer.Offset = action.kind == ces_action_kind::send ? action.offset : 0;
    connection->buffer.Length = action.kind == ces_action_kind::send ? action.length : worker->stride;
    if (connection->buffer.Offset > worker->stride ||
        connection->buffer.Length > worker->stride - connection->buffer.Offset) {
        ces_engine_fail_fast(L"server send window", ERROR_INVALID_DATA);
    }
    const BOOL posted =
        action.kind == ces_action_kind::send ?
            worker->rio->RIOSend(connection->request_queue, &connection->buffer, 1, 0, &connection->request) :
            worker->rio->RIOReceive(connection->request_queue, &connection->buffer, 1, 0, &connection->request);
    if (posted == FALSE) {
        *failure = { action.kind == ces_action_kind::send ? L"RIOSend" : L"RIOReceive", WSAGetLastError() };
        return false;
    }
    ++connection->outstanding;
    ++worker->rio_outstanding;
    return true;
}

ces_completion ces_native_tcp_completion(ces_native_worker* worker, const RIORESULT& result) noexcept {
    const std::uintptr_t address     = static_cast<std::uintptr_t>(result.RequestContext);
    const std::uintptr_t first       = reinterpret_cast<std::uintptr_t>(worker->connections);
    const std::size_t    table_bytes = sizeof(ces_engine_connection) * worker->slot_count;
    if (worker->connections == nullptr || address < first || address - first >= table_bytes ||
        (address - first) % sizeof(ces_engine_connection) != offsetof(ces_engine_connection, request)) {
        ces_engine_fail_fast(L"worker RIO RequestContext", ERROR_INVALID_DATA);
    }
    ces_engine_connection*    connection = &worker->connections[(address - first) / sizeof(ces_engine_connection)];
    const ces_engine_request* request    = reinterpret_cast<const ces_engine_request*>(address);
    if (request->connection != connection || connection->owner != worker ||
        (request->operation != ces_engine_operation::receive && request->operation != ces_engine_operation::send)) {
        ces_engine_fail_fast(L"worker RIO RequestContext", ERROR_INVALID_DATA);
    }
    if (request->generation != connection->generation) {
        ces_engine_fail_fast(L"worker completion generation", ERROR_IO_INCOMPLETE);
    }
    if (connection->outstanding != 1 || worker->rio_outstanding == 0) {
        ces_engine_fail_fast(L"worker RIO outstanding count", ERROR_INVALID_DATA);
    }
    if (result.BytesTransferred > connection->buffer.Length) {
        ces_engine_fail_fast(L"worker completion exceeds posted range", ERROR_INVALID_DATA);
    }
    --connection->outstanding;
    --worker->rio_outstanding;
    return { connection->index, request->operation, result.Status, result.BytesTransferred };
}

bool ces_native_udp_post(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                         RIO_RQ                              queue,
                         ces_engine_udp_slot*                slot,
                         const ces_action&                   action,
                         std::uint32_t*                      outstanding) noexcept {
    if (slot->outstanding) {
        ces_engine_fail_fast(L"UDP duplicate post", ERROR_INVALID_STATE);
    }
    slot->operation = action.kind == ces_action_kind::send ? ces_engine_operation::send : ces_engine_operation::receive;
    slot->payload.Length = action.length;
    const BOOL posted =
        slot->operation == ces_engine_operation::send ?
            rio->RIOSendEx(queue, &slot->payload, 1, nullptr, &slot->remote_address, nullptr, nullptr, 0, slot) :
            rio->RIOReceiveEx(queue, &slot->payload, 1, nullptr, &slot->remote_address, nullptr, nullptr, 0, slot);
    if (posted == FALSE) {
        return false;
    }
    slot->outstanding = true;
    ++*outstanding;
    return true;
}

ces_completion ces_native_udp_completion(ces_engine_udp_slot* slots,
                                         std::uint32_t        depth,
                                         std::uint32_t*       outstanding,
                                         const RIORESULT&     result) noexcept {
    const std::uintptr_t address     = static_cast<std::uintptr_t>(result.RequestContext);
    const std::uintptr_t first       = reinterpret_cast<std::uintptr_t>(slots);
    const std::size_t    table_bytes = sizeof(ces_engine_udp_slot) * depth;
    if (slots == nullptr || address < first || address - first >= table_bytes ||
        (address - first) % sizeof(ces_engine_udp_slot) != 0) {
        ces_engine_fail_fast(L"UDP completion identity", ERROR_INVALID_DATA);
    }
    ces_engine_udp_slot* slot = reinterpret_cast<ces_engine_udp_slot*>(address);
    if (!slot->outstanding || *outstanding == 0) {
        ces_engine_fail_fast(L"UDP completion invariant", ERROR_INVALID_DATA);
    }
    if (result.BytesTransferred > slot->payload.Length) {
        ces_engine_fail_fast(L"UDP completion exceeds posted range", ERROR_INVALID_DATA);
    }
    slot->outstanding = false;
    --*outstanding;
    return { static_cast<std::uint32_t>((address - first) / sizeof(ces_engine_udp_slot)), slot->operation,
             result.Status, result.BytesTransferred };
}
