#ifndef INPUT_UDP_H
#define INPUT_UDP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <sys/types.h>

typedef struct input_udp {
    int fd;
    bool joined_multicast;
    struct ip_mreq multicast_request;
    char url[256];
    char interface_address[64];
} input_udp_t;

int input_udp_parse_url(const char *url, char *host, size_t host_size, uint16_t *port);
bool input_udp_is_multicast_host(const char *host);
int input_udp_open(input_udp_t *input, const char *url);
int input_udp_open_with_interface(input_udp_t *input, const char *url, const char *interface_address);
ssize_t input_udp_receive(input_udp_t *input, uint8_t *buffer, size_t buffer_size);
void input_udp_close(input_udp_t *input);

#endif
