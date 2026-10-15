#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <type_traits>

import ces.test.support;

int ces_pure_engine_tests_main() {
    ces_engine_test_tcp_event_contract();
    ces_engine_test_udp_event_contract();
    ces_engine_test_acceptor_state_contract();
    std::printf("server_pure_engine_failures=%d\n", ces_engine_test_failures);
    return ces_engine_test_failures == 0 ? 0 : 1;
}
