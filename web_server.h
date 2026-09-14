#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "report_stats.h"

#include <stdint.h>

typedef struct web_server {
    int fd;
    char webroot[256];
} web_server_t;

int web_server_open(web_server_t *server, uint16_t port, const char *webroot);
void web_server_handle_ready(web_server_t *server, report_stats_t *stats);
void web_server_close(web_server_t *server);

#endif
