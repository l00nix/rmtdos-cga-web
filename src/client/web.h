/*
 * Copyright 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __RMTDOS_CLIENT_WEB_H
#define __RMTDOS_CLIENT_WEB_H

struct WebServer {
  int listen_fd;
};

int web_server_start(struct WebServer *server, const char *addr, int port);
void web_server_process(struct WebServer *server);
void web_server_close(struct WebServer *server);

#endif // __RMTDOS_CLIENT_WEB_H
