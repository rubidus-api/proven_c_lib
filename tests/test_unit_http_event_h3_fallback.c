/*
 * The event server's HTTP/3 test again, with datagrams sent one a call and no GRO asked for
 * (RFC-0018 S-3): the way every platform but Linux and Windows sends, and the way those fall back
 * when the system refuses UDP_SEGMENT or USO - so that way is a tested path on them too.
 */
#define main proven_test_main_with_batches_
#include "test_unit_http_event_h3.c"
#undef main
#include "../src/proven/proven_internal_quic_net.h"

int main(void) {
    proven_net_udp_segments_off_for_test_(true);
    return proven_test_main_with_batches_();
}
