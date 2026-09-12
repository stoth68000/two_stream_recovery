#ifndef INPUT_UDP_H
#define INPUT_UDP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct input_udp {
    int fd;
    char url[256];
} input_udp_t;

int input_udp_open(input_udp_t *input, const char *url);
ssize_t input_udp_receive(input_udp_t *input, uint8_t *buffer, size_t buffer_size);
void input_udp_close(input_udp_t *input);

#endif
