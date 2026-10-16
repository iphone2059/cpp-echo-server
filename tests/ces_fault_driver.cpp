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
#include <cwchar>
#include <memory>
#include <new>
#include <type_traits>

import ces.types;
import ces.internal;
import ces.engine;
import ces.native;

static int                          ces_fault_notify_status          = ERROR_SUCCESS;
static DWORD                        ces_fault_accept_status          = ERROR_SUCCESS;
static std::uint32_t                ces_fault_accept_calls           = 0;
static bool                         ces_fault_accept_arguments_valid = true;
static ces_engine_accept_operation* ces_fault_accept_operation       = nullptr;
static std::uint32_t                ces_fault_release_step           = 0;
static bool                         ces_fault_release_order_valid    = true;
static HANDLE                       ces_fault_release_port           = nullptr;
static void*                        ces_fault_release_arena          = nullptr;

static void WINAPI ces_fault_close_queue(RIO_CQ) noexcept {
    DWORD                    flags = 0;
    MEMORY_BASIC_INFORMATION information{};
    ces_fault_release_order_valid =
        ces_fault_release_order_valid && ces_fault_release_step == 0 &&
        GetHandleInformation(ces_fault_release_port, &flags) != FALSE &&
        VirtualQuery(ces_fault_release_arena, &information, sizeof(information)) == sizeof(information) &&
        information.State == MEM_COMMIT;
    ces_fault_release_step = 1;
}

static void WINAPI ces_fault_deregister_buffer(RIO_BUFFERID) noexcept {
    ces_fault_release_order_valid = ces_fault_release_order_valid && ces_fault_release_step == 1;
    ces_fault_release_step        = 2;
}

static int WINAPI ces_fault_rio_notify(RIO_CQ) noexcept {
    return ces_fault_notify_status;
}

static BOOL WINAPI ces_fault_accept_ex(SOCKET       listener,
                                       SOCKET       accepted,
                                       PVOID        addresses,
                                       DWORD        receive_length,
                                       DWORD        local_length,
                                       DWORD        remote_length,
                                       LPDWORD      received,
                                       LPOVERLAPPED overlapped) noexcept {
    int socket_type       = 0;
    int socket_type_bytes = sizeof(socket_type);
    ces_fault_accept_arguments_valid =
        ces_fault_accept_arguments_valid && listener != INVALID_SOCKET && accepted != INVALID_SOCKET &&
        addresses != nullptr && receive_length == 0 && local_length == sizeof(SOCKADDR_STORAGE) + 16U &&
        remote_length == sizeof(SOCKADDR_STORAGE) + 16U && received != nullptr &&
        ces_fault_accept_operation != nullptr && overlapped == &ces_fault_accept_operation->overlapped &&
        ces_fault_accept_operation->posted &&
        getsockopt(accepted, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socket_type), &socket_type_bytes) == 0 &&
        socket_type == SOCK_STREAM;
    ++ces_fault_accept_calls;
    WSASetLastError(static_cast<int>(ces_fault_accept_status));
    return FALSE;
}

static int ces_fault_accept_retries(DWORD status) noexcept {
    ces_engine_winsock winsock{};
    if (winsock.start() != 0) {
        return 1;
    }
    ces_engine_acceptor_resources resources{};
    resources.port.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    resources.listener.reset(ces_engine_registered_socket(SOCK_STREAM, IPPROTO_TCP));
    resources.operation_sockets.reset(new (std::nothrow) ces_socket_owner[1]);
    if (resources.port.get() == nullptr || resources.listener.get() == INVALID_SOCKET || !resources.operation_sockets) {
        return 1;
    }
    std::atomic<bool>           failed{ false };
    ces_engine_acceptor         acceptor{};
    ces_engine_accept_operation operation{};
    ces_accept_slot             slot{};
    acceptor.core.slots                 = &slot;
    acceptor.core.operation_count       = 1;
    acceptor.transport->resources       = &resources;
    acceptor.failed                     = &failed;
    acceptor.transport->port            = resources.port.get();
    acceptor.transport->listener        = resources.listener.get();
    acceptor.transport->accept_ex       = &ces_fault_accept_ex;
    acceptor.transport->operations      = &operation;
    acceptor.transport->operation_count = 1;
    operation.owner                     = acceptor.transport.get();
    operation.socket                    = INVALID_SOCKET;
    slot.state                          = ces_accept_state::idle;
    operation.index                     = 0;
    ces_fault_accept_status             = status;
    ces_fault_accept_calls              = 0;
    ces_fault_accept_arguments_valid    = true;
    ces_fault_accept_operation          = &operation;

    if (!ces_engine_accept_error_is_recoverable(status) || !ces_engine_post_accept(&acceptor, &operation) ||
        ces_fault_accept_calls != 4 || !ces_fault_accept_arguments_valid || acceptor.core.network_errors != 4 ||
        slot.state != ces_accept_state::deferred || operation.socket != INVALID_SOCKET ||
        resources.operation_sockets[0].get() != INVALID_SOCKET) {
        return 1;
    }
    DWORD       transferred = 0;
    ULONG_PTR   key         = 0;
    OVERLAPPED* overlapped  = nullptr;
    if (GetQueuedCompletionStatus(acceptor.transport->port, &transferred, &key, &overlapped, 0) == FALSE ||
        transferred != 0 || key != reinterpret_cast<ULONG_PTR>(&operation) || overlapped != nullptr) {
        return 1;
    }
    transferred = 0;
    key         = 0;
    overlapped  = nullptr;
    if (GetQueuedCompletionStatus(acceptor.transport->port, &transferred, &key, &overlapped, 0) != FALSE ||
        GetLastError() != WAIT_TIMEOUT) {
        return 1;
    }

    // The first dequeue inspects the one deferred packet. The real acceptor loop below receives a
    // stop packet before its own deferred packet, then consumes that deferred packet while stopping.
    operation.control_pending = false;
    slot.state                = ces_accept_state::idle;
    if (PostQueuedCompletionStatus(acceptor.transport->port, 0, ces_engine_stop_key, nullptr) == FALSE ||
        ces_engine_acceptor_thread(&acceptor) != 0 || !acceptor.core.stopping ||
        failed.load(std::memory_order_acquire) || ces_fault_accept_calls != 8 || !ces_fault_accept_arguments_valid ||
        acceptor.core.network_errors != 8 || slot.state != ces_accept_state::idle ||
        operation.socket != INVALID_SOCKET || resources.operation_sockets[0].get() != INVALID_SOCKET ||
        acceptor.transport->listener != INVALID_SOCKET) {
        return 1;
    }
    transferred = 0;
    key         = 0;
    overlapped  = nullptr;
    const bool empty =
        GetQueuedCompletionStatus(acceptor.transport->port, &transferred, &key, &overlapped, 0) == FALSE &&
        GetLastError() == WAIT_TIMEOUT;
    ces_engine_acceptor_destroy(&acceptor);
    return empty ? 0 : 1;
}

static int ces_fault_tcp_completion(bool oversized) noexcept {
    ces_native_worker     worker{};
    ces_engine_connection connection{};
    worker.connections               = &connection;
    worker.slot_count                = 1;
    worker.rio_outstanding           = 1;
    connection.owner                 = &worker;
    connection.index                 = 0;
    connection.outstanding           = 1;
    connection.generation            = 1;
    connection.request.operation     = ces_engine_operation::receive;
    connection.request.sequence      = 1;
    connection.request.token         = ces_operation_token_encode(0, ces_engine_operation::receive, 1, 1);
    connection.request.posted_length = 8;
    RIORESULT result{};
    result.SocketContext            = reinterpret_cast<ULONG_PTR>(&connection);
    result.RequestContext           = static_cast<ULONG_PTR>(connection.request.token);
    result.Status                   = ERROR_OPERATION_ABORTED;
    result.BytesTransferred         = oversized ? 9U : 0U;
    const ces_completion completion = ces_native_tcp_completion(&worker, result);
    worker.connections              = nullptr;
    return completion.status == 995 && completion.bytes == 0 && connection.outstanding == 0 &&
                   worker.rio_outstanding == 0 && connection.request.token == 0 ?
               0 :
               1;
}

/// The token was posted by generation one and the slot was then adopted as generation two. The lane
/// still matches the token, so only the generation carried by value inside the token can reject it.
static int ces_fault_stale_completion_generation() noexcept {
    ces_native_worker     worker{};
    ces_engine_connection connection{};
    worker.connections               = &connection;
    worker.slot_count                = 1;
    worker.rio_outstanding           = 1;
    connection.owner                 = &worker;
    connection.index                 = 0;
    connection.outstanding           = 1;
    connection.generation            = 2;
    connection.request.operation     = ces_engine_operation::receive;
    connection.request.sequence      = 1;
    connection.request.token         = ces_operation_token_encode(0, ces_engine_operation::receive, 1, 1);
    connection.request.posted_length = 8;
    RIORESULT result{};
    result.SocketContext  = reinterpret_cast<ULONG_PTR>(&connection);
    result.RequestContext = static_cast<ULONG_PTR>(connection.request.token);
    (void) ces_native_tcp_completion(&worker, result);
    return 1;
}

/// A completion whose token does not match the posted lane: a stale retirement or a duplicate.
static int ces_fault_stale_completion_token() noexcept {
    ces_native_worker     worker{};
    ces_engine_connection connection{};
    worker.connections               = &connection;
    worker.slot_count                = 1;
    worker.rio_outstanding           = 1;
    connection.owner                 = &worker;
    connection.index                 = 0;
    connection.outstanding           = 1;
    connection.generation            = 1;
    connection.request.operation     = ces_engine_operation::receive;
    connection.request.sequence      = 2;
    connection.request.token         = ces_operation_token_encode(0, ces_engine_operation::receive, 1, 2);
    connection.request.posted_length = 8;
    RIORESULT result{};
    result.SocketContext  = reinterpret_cast<ULONG_PTR>(&connection);
    result.RequestContext = static_cast<ULONG_PTR>(ces_operation_token_encode(0, ces_engine_operation::receive, 1, 1));
    (void) ces_native_tcp_completion(&worker, result);
    return 1;
}

static int ces_fault_worker_release(bool invalid, bool implicit) noexcept {
    ces_engine_worker                         worker{};
    std::array<ces_rio_registration_owner, 1> stack_registrations{};
    ces_engine_worker_resources               resources{};
    worker.transport->resources = &resources;
    resources.lifetime          = worker.transport.get();
    if (invalid) {
        // A release before the boundary guard would delete an array stored on the stack,
        // preventing the deterministic named ownership failure from being reported.
        resources.registrations.reset(stack_registrations.data());
        worker.transport->rio_outstanding = 1;
        if (implicit) {
            return 1;
        }
        ces_engine_worker_destroy(&worker);
        return 1;
    }
    void* allocation = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (allocation == nullptr) {
        return 1;
    }
    resources.arena.reset(allocation);
    worker.transport->memory = static_cast<char*>(allocation);
    ces_engine_worker_destroy(&worker);
    MEMORY_BASIC_INFORMATION information{};
    return VirtualQuery(allocation, &information, sizeof(information)) == sizeof(information) &&
                   information.State == MEM_FREE ?
               0 :
               1;
}

static bool ces_fault_attach_release_resources(ces_engine_worker*                  worker,
                                               ces_engine_worker_resources*        resources,
                                               const RIO_EXTENSION_FUNCTION_TABLE* rio) noexcept {
    if (!worker->transport || !worker->core) {
        return false;
    }
    ces_fault_release_step        = 0;
    ces_fault_release_order_valid = true;
    ces_fault_release_port        = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    ces_fault_release_arena       = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    resources->port.reset(ces_fault_release_port);
    resources->arena.reset(ces_fault_release_arena);
    if (ces_fault_release_port == nullptr || ces_fault_release_arena == nullptr) {
        return false;
    }
    resources->registrations.reset(new (std::nothrow) ces_rio_registration_owner[1]);
    if (!resources->registrations) {
        return false;
    }
    const RIO_CQ queue = reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(0x1234U));
    resources->completion_queue.reset(rio, queue);
    resources->registrations[0].reset(rio, reinterpret_cast<RIO_BUFFERID>(static_cast<std::uintptr_t>(0x5678U)));
    worker->transport->resources        = resources;
    resources->lifetime                 = worker->transport.get();
    worker->transport->port             = ces_fault_release_port;
    worker->transport->memory           = static_cast<char*>(ces_fault_release_arena);
    worker->transport->completion_queue = queue;
    return true;
}

static bool ces_fault_released_resources() noexcept {
    DWORD                    flags = 0;
    MEMORY_BASIC_INFORMATION information{};
    return ces_fault_release_order_valid && ces_fault_release_step == 2 &&
           VirtualQuery(ces_fault_release_arena, &information, sizeof(information)) == sizeof(information) &&
           information.State == MEM_FREE && GetHandleInformation(ces_fault_release_port, &flags) == FALSE &&
           GetLastError() == ERROR_INVALID_HANDLE;
}

static int ces_fault_implicit_release(bool runtime) noexcept {
    RIO_EXTENSION_FUNCTION_TABLE rio{};
    rio.RIOCloseCompletionQueue = &ces_fault_close_queue;
    rio.RIODeregisterBuffer     = &ces_fault_deregister_buffer;
    if (runtime) {
        ces_engine_worker_resources resources{};
        {
            ces_engine_worker worker{};
            if (!ces_fault_attach_release_resources(&worker, &resources, &rio)) {
                return 1;
            }
        }
        return ces_fault_released_resources() && resources.lifetime == nullptr ? 0 : 1;
    }
    ces_engine_worker worker{};
    {
        ces_engine_worker_resources resources{};
        if (!ces_fault_attach_release_resources(&worker, &resources, &rio)) {
            return 1;
        }
    }
    return ces_fault_released_resources() && worker.transport->resources == nullptr &&
                   worker.transport->port == nullptr && worker.transport->completion_queue == RIO_INVALID_CQ ?
               0 :
               1;
}

static int ces_fault_runtime_release_outstanding() noexcept {
    std::array<ces_rio_registration_owner, 1> stack_registrations{};
    ces_engine_worker_resources               resources{};
    ces_engine_worker                         worker{};
    if (!worker.transport || !worker.core) {
        return 1;
    }
    resources.registrations.reset(stack_registrations.data());
    resources.lifetime                = worker.transport.get();
    worker.transport->resources       = &resources;
    worker.transport->rio_outstanding = 1;
    return 1;
}

static int ces_fault_acceptor_implicit_release(bool invalid, bool control) noexcept {
    std::array<ces_socket_owner, 1> stack_sockets{};
    ces_engine_accept_operation     stack_operation{};
    ces_accept_slot                 stack_slot{};
    ces_engine_acceptor_resources   resources{};
    HANDLE                          released_port = nullptr;
    {
        ces_engine_acceptor acceptor{};
        if (!acceptor.transport) {
            return 1;
        }
        resources.lifetime            = acceptor.transport.get();
        acceptor.transport->resources = &resources;
        if (invalid) {
            resources.operation_sockets.reset(stack_sockets.data());
            acceptor.transport->operations      = &stack_operation;
            acceptor.transport->operation_count = 1;
            stack_operation.posted              = !control;
            stack_operation.control_pending     = control;
            acceptor.core.slots                 = &stack_slot;
            acceptor.core.operation_count       = 1;
            stack_slot.state                    = control ? ces_accept_state::transit : ces_accept_state::posted;
            return 1;
        }
        released_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        resources.port.reset(released_port);
        acceptor.transport->port = released_port;
        if (released_port == nullptr) {
            return 1;
        }
    }
    DWORD flags = 0;
    return resources.lifetime == nullptr && GetHandleInformation(released_port, &flags) == FALSE &&
                   GetLastError() == ERROR_INVALID_HANDLE ?
               0 :
               1;
}

int ces_fault_driver_main(int argc, wchar_t** argv) {
    if (argc != 2) {
        return 1;
    }
    if (std::wcscmp(argv[1], L"normal") == 0) {
        ces_require_rio_notify_success(ERROR_SUCCESS, L"test notify success");
        return ces_require_valid_dequeue_count(0, L"test dequeue success") == 0 ? 0 : 1;
    }
    if (std::wcscmp(argv[1], L"accept_retry_64") == 0) {
        return ces_fault_accept_retries(ERROR_NETNAME_DELETED);
    }
    if (std::wcscmp(argv[1], L"handoff_invalid") == 0) {
        (void) ces_engine_require_handoff_disposition(false, false, true, false);
        return 1;
    }
    if (std::wcscmp(argv[1], L"handoff_positive") == 0) {
        return ces_engine_require_handoff_disposition(false, false, true, true) == ces_handoff_disposition::handoff &&
                       ces_engine_require_handoff_disposition(true, false, true, false) ==
                           ces_handoff_disposition::withdraw &&
                       ces_engine_require_handoff_disposition(false, true, true, false) ==
                           ces_handoff_disposition::withdraw &&
                       ces_engine_require_handoff_disposition(false, false, false, false) ==
                           ces_handoff_disposition::withdraw ?
                   0 :
                   1;
    }
    if (std::wcscmp(argv[1], L"accept_retry_10054") == 0) {
        return ces_fault_accept_retries(WSAECONNRESET);
    }
    if (std::wcscmp(argv[1], L"accept_retry_10053") == 0) {
        return ces_fault_accept_retries(WSAECONNABORTED);
    }
    if (std::wcscmp(argv[1], L"notify_provider_failure") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &ces_fault_rio_notify;
        bool          armed     = false;
        std::uint64_t arms      = 0;
        ces_fault_notify_status = WSAEINVAL;
        ces_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(server provider)");
    } else if (std::wcscmp(argv[1], L"notify_provider_duplicate") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &ces_fault_rio_notify;
        bool          armed     = false;
        std::uint64_t arms      = 0;
        ces_fault_notify_status = WSAEALREADY;
        ces_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(server provider duplicate)");
    } else if (std::wcscmp(argv[1], L"notify_precondition_duplicate") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &ces_fault_rio_notify;
        bool          armed     = true;
        std::uint64_t arms      = 0;
        ces_fault_notify_status = ERROR_SUCCESS;
        ces_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(server duplicate precondition)");
    } else if (std::wcscmp(argv[1], L"completion_positive") == 0) {
        return ces_fault_tcp_completion(false);
    } else if (std::wcscmp(argv[1], L"completion_oversized") == 0) {
        return ces_fault_tcp_completion(true);
    } else if (std::wcscmp(argv[1], L"completion_stale_generation") == 0) {
        return ces_fault_stale_completion_generation();
    } else if (std::wcscmp(argv[1], L"completion_stale_token") == 0) {
        return ces_fault_stale_completion_token();
    } else if (std::wcscmp(argv[1], L"release_positive") == 0) {
        return ces_fault_worker_release(false, false);
    } else if (std::wcscmp(argv[1], L"release_outstanding") == 0) {
        return ces_fault_worker_release(true, false);
    } else if (std::wcscmp(argv[1], L"resource_deinit_outstanding") == 0) {
        return ces_fault_worker_release(true, true);
    } else if (std::wcscmp(argv[1], L"runtime_deinit_outstanding") == 0) {
        return ces_fault_runtime_release_outstanding();
    } else if (std::wcscmp(argv[1], L"runtime_deinit_positive") == 0) {
        return ces_fault_implicit_release(true);
    } else if (std::wcscmp(argv[1], L"resource_deinit_positive") == 0) {
        return ces_fault_implicit_release(false);
    } else if (std::wcscmp(argv[1], L"acceptor_deinit_positive") == 0) {
        return ces_fault_acceptor_implicit_release(false, false);
    } else if (std::wcscmp(argv[1], L"acceptor_deinit_posted") == 0) {
        return ces_fault_acceptor_implicit_release(true, false);
    } else if (std::wcscmp(argv[1], L"acceptor_deinit_control") == 0) {
        return ces_fault_acceptor_implicit_release(true, true);
    } else if (std::wcscmp(argv[1], L"notify_failure") == 0) {
        ces_require_rio_notify_success(WSAEINVAL, L"test notify failure");
    } else if (std::wcscmp(argv[1], L"notify_duplicate") == 0) {
        // WSAEALREADY means a previous RIONotify has not completed: the state machine armed twice,
        // so the process must report the duplicate-arm stage instead of the caller's stage.
        ces_require_rio_notify_success(WSAEALREADY, L"test notify duplicate");
    } else if (std::wcscmp(argv[1], L"corrupt_cq") == 0) {
        static_cast<void>(ces_require_valid_dequeue_count(RIO_CORRUPT_CQ, L"test corrupt CQ"));
    } else {
        return 1;
    }
    return 1;
}
