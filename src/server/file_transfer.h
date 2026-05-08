/*
 * Copyright 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __RMTDOS_SERVER_FILE_TRANSFER_H
#define __RMTDOS_SERVER_FILE_TRANSFER_H

#include "server/bufmgr.h"

void file_transfer_init();
void file_transfer_handle_packet(const struct Buffer *buffer);
void file_transfer_process_idle();

#endif // __RMTDOS_SERVER_FILE_TRANSFER_H
