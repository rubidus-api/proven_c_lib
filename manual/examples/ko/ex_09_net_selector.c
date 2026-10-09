#include "example.h"

/*
 * selector: 소켓을 한 번씩 등록해 두고, 준비된 것에 대해서만 통지받는다.
 *
 * 루프는 poll 예제의 그것이다 - 기다리고, 준비된 것을 기다리지 않고 처리한다 - . 다만 기다림과
 * 기다림 사이에 다시 짓는 것이 없고, 한가한 연결 천 개가 기다림에 아무 비용도 지우지 않는다.
 */

typedef struct {
    proven_net_conn_t conn;
    int number;
    bool open;
} client_t;

int main(void) {
    proven_net_listener_t listener;
    proven_net_addr_t at;
    proven_err_t err = proven_net_listen(proven_net_addr_loopback(PROVEN_NET_FAMILY_IPV4, 0), 16, &listener, &at);
    if (err == PROVEN_ERR_PERMISSION || err == PROVEN_ERR_UNSUPPORTED) {
        printf("no sockets in this environment; nothing to show\n");
        return EXAMPLE_OK();
    }
    EXAMPLE_REQUIRE(err == PROVEN_OK, "the listener opens");

    proven_net_selector_t *selector = NULL;
    EXAMPLE_REQUIRE(proven_net_selector_create(proven_heap_allocator(), &selector) == PROVEN_OK, "a selector");
    /* Linux에서는 epoll, BSD와 macOS에서는 kqueue. 그 밖에서는 poll 위의 같은 인터페이스. */
    proven_net_selector_kind_t kind = proven_net_selector_kind(selector);
    printf("selector kind: %s\n", kind == PROVEN_NET_SELECTOR_EPOLL ? "epoll" : kind == PROVEN_NET_SELECTOR_KQUEUE ? "kqueue" : "poll");

    /* tag는 이벤트와 함께 돌아오는 것이다: 그 소켓에 대한 여러분 자신의 기록을 가리키는 포인터.
     * 여기서 리스너의 tag는 리스너 자신이다. */
    EXAMPLE_REQUIRE(proven_net_selector_add(selector, proven_net_listener_handle(&listener), PROVEN_NET_READABLE, &listener) == PROVEN_OK,
                    "the listener is registered, once");

    /* 클라이언트 셋이 연결하고, 둘째와 셋째가 무언가를 말한다. */
    proven_net_conn_t far[3];
    for (int i = 0; i < 3; ++i) EXAMPLE_REQUIRE(proven_net_connect(at, proven_net_deadline_in(5000), &far[i]) == PROVEN_OK, "a client connects");
    EXAMPLE_REQUIRE(proven_net_write_all(&far[1], proven_mem_view_from_u8(PROVEN_LIT("from one")), proven_net_deadline_in(5000)).err == PROVEN_OK &&
                    proven_net_write_all(&far[2], proven_mem_view_from_u8(PROVEN_LIT("from two")), proven_net_deadline_in(5000)).err == PROVEN_OK, "two of them send");

    client_t clients[3] = {0};
    int accepted = 0, messages = 0;
    for (int round = 0; round < 20 && messages < 2; ++round) {
        proven_net_ready_t ready[8];
        proven_size_t count = 0;
        err = proven_net_selector_wait(selector, ready, 8, proven_net_deadline_in(1000), &count);
        if (err == PROVEN_ERR_TIMEOUT) continue;        /* 루프의 한가한 한 박자 */
        EXAMPLE_REQUIRE(err == PROVEN_OK, "something is ready");
        for (proven_size_t i = 0; i < count; ++i) {
            if (ready[i].tag == &listener) {
                /* 준비됨은 "기다리지 않는다"는 뜻이다: 대기 중인 연결을 기다리지 않고 모두 받는다. */
                while (accepted < 3 && proven_net_accept(&listener, PROVEN_NET_DONT_WAIT, &clients[accepted].conn, NULL) == PROVEN_OK) {
                    client_t *c = &clients[accepted];
                    c->number = accepted++;
                    c->open = true;
                    EXAMPLE_REQUIRE(proven_net_selector_add(selector, proven_net_conn_handle(&c->conn), PROVEN_NET_READABLE, c) == PROVEN_OK,
                                    "each accepted connection is registered, with its record as the tag");
                }
            } else {
                client_t *c = ready[i].tag;
                proven_byte_t buf[32];
                proven_result_size_t got = proven_net_read(&c->conn, (proven_mem_mut_t){ buf, sizeof buf }, PROVEN_NET_DONT_WAIT);
                if (got.err == PROVEN_OK) messages++;
            }
        }
    }
    EXAMPLE_REQUIRE(accepted == 3 && messages == 2, "three connections accepted, two messages read");
    EXAMPLE_REQUIRE(proven_net_selector_count(selector) == 4, "four sockets registered: the listener and three connections");

    /* 첫 연결은 말한 적이 없어서 보고된 적도 없다: 해 줄 일이 없었다. 이제 그 연결에 다른 것을
     * 묻는다 - 써도 되는가? - 그러면 곧장 보고된다. */
    proven_net_ready_t one[4];
    proven_size_t count = 0;
    EXAMPLE_REQUIRE(proven_net_selector_wait(selector, one, 4, proven_net_deadline_in(30), &count) == PROVEN_ERR_TIMEOUT, "all quiet: nothing is reported");
    EXAMPLE_REQUIRE(proven_net_selector_modify(selector, proven_net_conn_handle(&clients[0].conn), PROVEN_NET_WRITABLE, &clients[0]) == PROVEN_OK, "ask about writing instead");
    EXAMPLE_REQUIRE(proven_net_selector_wait(selector, one, 4, proven_net_deadline_in(1000), &count) == PROVEN_OK && count == 1 &&
                    one[0].tag == &clients[0] && (one[0].got & PROVEN_NET_WRITABLE), "an idle connection is writable");
    /* 쓸 것이 있는 동안에만 쓰기 가능을 물어라. 그러지 않으면 모든 기다림이 곧장 돌아온다. */
    EXAMPLE_REQUIRE(proven_net_selector_modify(selector, proven_net_conn_handle(&clients[0].conn), PROVEN_NET_READABLE, &clients[0]) == PROVEN_OK, "back to reading");

    /* 소켓은 닫기 "전에" 꺼낸다. */
    for (int i = 0; i < 3; ++i) {
        EXAMPLE_REQUIRE(proven_net_selector_remove(selector, proven_net_conn_handle(&clients[i].conn)) == PROVEN_OK, "removed");
        (void)proven_net_close(&clients[i].conn);
        (void)proven_net_close(&far[i]);
    }
    EXAMPLE_REQUIRE(proven_net_selector_remove(selector, proven_net_listener_handle(&listener)) == PROVEN_OK &&
                    proven_net_selector_count(selector) == 0, "and the listener: the selector is empty");
    (void)proven_net_listener_close(&listener);

    /* 이식형 종류를 이름으로 요청할 수 있다 - 둘을 비교하거나, Windows가 쓰는 코드 경로를
     * Linux에서 돌려 보려고. */
    proven_net_selector_t *portable = NULL;
    EXAMPLE_REQUIRE(proven_net_selector_create_poll(proven_heap_allocator(), &portable) == PROVEN_OK &&
                    proven_net_selector_kind(portable) == PROVEN_NET_SELECTOR_POLL, "a selector of the poll kind");
    proven_net_selector_destroy(portable);
    proven_net_selector_destroy(selector);
    return EXAMPLE_OK();
}
