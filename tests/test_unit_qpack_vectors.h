/* Generated from the published text of RFC 9204 (appendix B) by a private program; do not
 * edit. Each example's streams in order: the section, the stream (a number is a request
 * stream's field section), and its bytes. */
static const struct { const char *section; const char *stream; const char *hex; } qpack_examples[] = {
    { "B.1", "0", "0000510b2f696e6465782e68746d6c" },
    { "B.2", "Encoder", "3fbd01c00f7777772e6578616d706c652e636f6dc10c2f73616d706c652f70617468" },
    { "B.2", "4", "03811011" },
    { "B.2", "Decoder", "84" },
    { "B.3", "Encoder", "4a637573746f6d2d6b65790c637573746f6d2d76616c7565" },
    { "B.3", "Decoder", "01" },
    { "B.4", "Encoder", "02" },
    { "B.4", "8", "050080c181" },
    { "B.4", "Decoder", "48" },
    { "B.5", "Encoder", "810d637573746f6d2d76616c756532" },
};
