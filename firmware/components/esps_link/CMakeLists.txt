# esps_link: transport abstraction + the UART implementation (S0/S1). tcp.c
# and espnow.c are TODO(S6) stubs (see their headers) built in now so the
# interface shape is exercised by the linker today, not discovered wrong
# later. driver provides driver/uart.h; freertos provides the task/queue
# primitives the UART transport's TX/RX tasks run on.
idf_component_register(
    SRCS "src/esps_link_uart.c" "src/esps_link_tcp.c" "src/esps_link_espnow.c"
    INCLUDE_DIRS "include"
    REQUIRES esps_proto driver freertos
)
