/*
 * Copyright 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __RMTDOS_CLIENT_FILE_TRANSFER_H
#define __RMTDOS_CLIENT_FILE_TRANSFER_H

#include <stdint.h>

#include "client/network.h"

int file_transfer_put(struct RawSocket *sock, const uint8_t *dest_mac_addr,
                      const char *local_path, const char *remote_path);

#endif // __RMTDOS_CLIENT_FILE_TRANSFER_H
