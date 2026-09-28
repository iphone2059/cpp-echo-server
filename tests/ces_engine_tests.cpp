#include "ces_engine_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <utility>

static int ces_engine_test_failures = 0;

static std::uint32_t ces_engine_test_xorshift(std::uint32_t* state) noexcept
{
    std::uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static void ces_engine_test_expect(bool condition, const char* name) noexcept
{
    if (condition)
    {
        std::printf("PASS %s\n", name);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", name);
    ++ces_engine_test_failures;
}

static void ces_engine_test_lifecycle() noexcept
{
    ces_worker_lifecycle lifecycle{ces_worker_phase::quiescing, 0, 0, true};
    ces_engine_test_expect(!ces_worker_may_exit(&lifecycle), "worker requires admission barrier");
    lifecycle.phase = ces_worker_phase::admission_closed;
    ces_engine_test_expect(ces_worker_may_exit(&lifecycle), "worker exits after admission closes and work drains");
    lifecycle.pending_handoffs = 1;
    ces_engine_test_expect(!ces_worker_may_exit(&lifecycle), "worker retains pending handoff");
    lifecycle.pending_handoffs = 0;
    lifecycle.active_connections = 1;
    ces_engine_test_expect(!ces_worker_may_exit(&lifecycle), "worker retains active connection");
    ces_engine_test_expect(!ces_udp_may_release(ces_udp_phase::draining, 0), "UDP drain is not released early");
    ces_engine_test_expect(!ces_udp_may_release(ces_udp_phase::stopped, 1), "UDP outstanding retains storage");
    ces_engine_test_expect(ces_udp_may_release(ces_udp_phase::stopped, 0), "UDP stopped state releases storage");
    OVERLAPPED expected{};
    OVERLAPPED other{};
    ces_engine_test_expect(ces_notification_packet_matches(7, &expected, 7, &expected),
                           "server notification identity requires matching key and OVERLAPPED");
    ces_engine_test_expect(!ces_notification_packet_matches(8, &expected, 7, &expected) &&
                               !ces_notification_packet_matches(7, &other, 7, &expected),
                           "server notification identity rejects wrong packet fields");
}

static void ces_engine_test_owners() noexcept
{
    ces_socket_owner first{};
    ces_socket_owner second{std::move(first)};
    ces_engine_test_expect(first.get() == INVALID_SOCKET && second.get() == INVALID_SOCKET,
                           "socket owner move clears source");
    ces_handle_owner first_handle{};
    ces_handle_owner second_handle{std::move(first_handle)};
    ces_engine_test_expect(first_handle.get() == nullptr && second_handle.get() == nullptr,
                           "handle owner move clears source");
    void* allocation = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ces_virtual_arena_owner first_arena{allocation};
    ces_virtual_arena_owner second_arena{std::move(first_arena)};
    ces_engine_test_expect(first_arena.get() == nullptr && second_arena.get() == allocation,
                           "virtual arena owner move transfers allocation");
    ces_virtual_arena_owner released_arena{second_arena.release()};
    ces_engine_test_expect(second_arena.get() == nullptr && released_arena.get() == allocation,
                           "virtual arena owner release transfers ownership");
    void* heap_allocation = HeapAlloc(GetProcessHeap(), 0, 64);
    ces_heap_owner first_heap{heap_allocation};
    ces_heap_owner second_heap{std::move(first_heap)};
    ces_engine_test_expect(first_heap.get() == nullptr && second_heap.get() == heap_allocation,
                           "heap owner move transfers allocation");
}

static void ces_engine_test_timer() noexcept
{
    std::array<ces_timer_node, 4> nodes{};
    std::array<std::uint32_t, 4> positions{};
    ces_timer_heap heap{};
    ces_engine_test_expect(ces_timer_initialize(&heap, nodes.data(), positions.data(), 4), "timer initializes");
    ces_engine_test_expect(ces_timer_wait_milliseconds(&heap, 10) == INFINITE, "empty timer waits indefinitely");
    ces_engine_test_expect(ces_timer_insert_or_update(&heap, 2, 40) && ces_timer_insert_or_update(&heap, 1, 20) &&
                               ces_timer_insert_or_update(&heap, 3, 30),
                           "timer inserts deadlines");
    ces_engine_test_expect(ces_timer_wait_milliseconds(&heap, 10) == 10, "timer exposes nearest deadline");
    ces_engine_test_expect(ces_timer_insert_or_update(&heap, 2, 15) && ces_timer_wait_milliseconds(&heap, 10) == 5,
                           "timer updates deadline earlier");
    std::uint32_t index = UINT32_MAX;
    ces_engine_test_expect(ces_timer_pop_expired(&heap, 15, &index) && index == 2, "timer pops expired connection");
    ces_engine_test_expect(ces_timer_remove(&heap, 1), "timer removes indexed connection");
    ces_engine_test_expect(ces_timer_wait_milliseconds(&heap, 20) == 10, "timer repairs heap after removal");
    ces_engine_test_expect(!ces_timer_insert_or_update(&heap, 4, 1), "timer rejects index outside capacity");
    ces_engine_test_expect(ces_timer_insert_or_update(&heap, 0, std::numeric_limits<ULONGLONG>::max()) &&
                               ces_timer_wait_milliseconds(&heap, 0) == 30,
                           "nearer timer wins over saturated deadline");
    ces_engine_test_expect(ces_timer_remove(&heap, 3) && ces_timer_remove(&heap, 0) && heap.size == 0,
                           "timer removes all remaining entries");
    ces_engine_test_expect(!ces_timer_pop_expired(&heap, std::numeric_limits<ULONGLONG>::max(), &index),
                           "empty timer has no expired root");
    ces_engine_test_expect(ces_timer_insert_or_update(&heap, 3, 50) && ces_timer_insert_or_update(&heap, 1, 50) &&
                               ces_timer_pop_expired(&heap, 50, &index) && index == 1,
                           "timer orders equal deadlines by index");
    ces_engine_test_expect(ces_timer_remove(&heap, 3) &&
                               ces_timer_insert_or_update(&heap, 0, std::numeric_limits<ULONGLONG>::max()) &&
                               ces_timer_wait_milliseconds(&heap, 0) == INFINITE - 1U,
                           "timer wait saturates below INFINITE");
}

static void ces_engine_test_timer_model() noexcept
{
    constexpr std::uint32_t capacity = 64;
    constexpr std::uint32_t steps = 100000;
    std::array<ces_timer_node, capacity> nodes{};
    std::array<std::uint32_t, capacity> positions{};
    std::array<bool, capacity> active{};
    std::array<ULONGLONG, capacity> deadlines{};
    ces_timer_heap heap{};
    bool valid = ces_timer_initialize(&heap, nodes.data(), positions.data(), capacity);
    std::uint32_t random_state = 0x51A7E123U;

    for (std::uint32_t step = 0; step < steps && valid; ++step)
    {
        const std::uint32_t operation = ces_engine_test_xorshift(&random_state) & 3U;
        const std::uint32_t connection_index = ces_engine_test_xorshift(&random_state) % capacity;
        const ULONGLONG now = static_cast<ULONGLONG>(step % 4096U);
        if (operation <= 1U)
        {
            const ULONGLONG deadline = static_cast<ULONGLONG>(ces_engine_test_xorshift(&random_state) % 4096U);
            valid = ces_timer_insert_or_update(&heap, connection_index, deadline);
            active[connection_index] = true;
            deadlines[connection_index] = deadline;
        }
        else if (operation == 2U)
        {
            const bool expected = active[connection_index];
            valid = ces_timer_remove(&heap, connection_index) == expected;
            if (expected)
            {
                active[connection_index] = false;
            }
        }
        else
        {
            bool expected_found = false;
            std::uint32_t expected_index = UINT32_MAX;
            ULONGLONG expected_deadline = 0;
            for (std::uint32_t index = 0; index < capacity; ++index)
            {
                if (active[index] && deadlines[index] <= now &&
                    (!expected_found || deadlines[index] < expected_deadline ||
                     (deadlines[index] == expected_deadline && index < expected_index)))
                {
                    expected_found = true;
                    expected_index = index;
                    expected_deadline = deadlines[index];
                }
            }
            std::uint32_t popped_index = UINT32_MAX;
            const bool popped = ces_timer_pop_expired(&heap, now, &popped_index);
            valid = popped == expected_found && (!popped || popped_index == expected_index);
            if (expected_found)
            {
                active[expected_index] = false;
            }
        }

        std::uint32_t active_count = 0;
        bool minimum_found = false;
        std::uint32_t minimum_index = UINT32_MAX;
        ULONGLONG minimum_deadline = 0;
        for (std::uint32_t index = 0; index < capacity && valid; ++index)
        {
            if (!active[index])
            {
                valid = positions[index] == UINT32_MAX;
                continue;
            }
            ++active_count;
            const std::uint32_t position = positions[index];
            valid = position < heap.size && nodes[position].connection_index == index &&
                    nodes[position].deadline == deadlines[index];
            if (!minimum_found || deadlines[index] < minimum_deadline ||
                (deadlines[index] == minimum_deadline && index < minimum_index))
            {
                minimum_found = true;
                minimum_index = index;
                minimum_deadline = deadlines[index];
            }
        }
        valid = valid && heap.size == active_count;
        if (valid && minimum_found)
        {
            valid = nodes[0].connection_index == minimum_index && nodes[0].deadline == minimum_deadline;
        }
        const DWORD expected_wait =
            !minimum_found ? INFINITE
            : minimum_deadline <= now
                ? 0
                : static_cast<DWORD>(std::min(minimum_deadline - now, static_cast<ULONGLONG>(INFINITE) - 1ULL));
        valid = valid && ces_timer_wait_milliseconds(&heap, now) == expected_wait;
    }

    ces_engine_test_expect(valid, "server timer matches fixed-seed reference model for 100000 operations");
}

static void ces_engine_test_statistics() noexcept
{
    ces_engine_statistics total{};
    ces_engine_test_expect(total.accepted == 0 && total.completions == 0 && total.receives == 0 && total.sends == 0 &&
                               total.bytes == 0,
                           "server statistics zero initialize");

    const ces_engine_statistics first{3, 11, 5, 6, 4096};
    const ces_engine_statistics second{7, 19, 9, 10, 8192};
    ces_statistics_add(&total, &first);
    ces_statistics_add(&total, &second);
    ces_engine_test_expect(total.accepted == 10 && total.completions == 30 && total.receives == 14 &&
                               total.sends == 16 && total.bytes == 12288,
                           "server statistics aggregate worker snapshots");

    ces_engine_statistics large{std::numeric_limits<std::uint64_t>::max() - 100U, 0, 0, 0,
                                std::numeric_limits<std::uint64_t>::max() - 4096U};
    const ces_engine_statistics remainder{100, 0, 0, 0, 4096};
    ces_statistics_add(&large, &remainder);
    ces_engine_test_expect(large.accepted == std::numeric_limits<std::uint64_t>::max() &&
                               large.bytes == std::numeric_limits<std::uint64_t>::max(),
                           "server statistics preserve exact large-value addition");
}

int main()
{
    ces_engine_test_lifecycle();
    ces_engine_test_owners();
    ces_engine_test_timer();
    ces_engine_test_timer_model();
    ces_engine_test_statistics();
    std::printf("server_engine_failures=%d\n", ces_engine_test_failures);
    return ces_engine_test_failures == 0 ? 0 : 1;
}
