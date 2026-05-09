/*
 * Copyright 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>

#include "common/protocol.h"
#include "lib16/x86.h"
#include "server/file_transfer.h"
#include "server/globals.h"
#include "server/pktdrv.h"
#include "server/util.h"

#define FILE_MODE_NONE 0
#define FILE_MODE_PUT 1
#define FILE_MODE_GET 2

#define FILE_OP_NONE 0
#define FILE_OP_PUT_BEGIN 1
#define FILE_OP_PUT_DATA 2
#define FILE_OP_PUT_END 3
#define FILE_OP_ACK 4
#define FILE_OP_GET_BEGIN 5
#define FILE_OP_GET_DATA_REQ 6
#define FILE_OP_GET_END 7

struct FileTransferState {
  uint8_t active;
  uint8_t mode;
  uint8_t pending_op;
  uint8_t client_mac[ETH_ALEN];
  uint32_t session_id;
  uint32_t transfer_id;
  uint32_t expected_offset;
  uint32_t size;
  int handle;
  char filename[FILE_TRANSFER_NAME_BYTES];
  uint8_t chunk[FILE_TRANSFER_CHUNK_BYTES];
  uint16_t chunk_count;
  uint32_t chunk_offset;
  uint16_t ack_command;
  uint16_t ack_status;
  uint32_t ack_offset;
};

static struct FileTransferState g_file;

static void send_file_ack(uint16_t command, uint16_t status, uint32_t offset) {
  struct EthernetHeader *out_eh = (struct EthernetHeader *)(g_send_buffer);
  struct ProtocolHeader *out_ph = (struct ProtocolHeader *)(out_eh + 1);
  struct FileAck *ack = (struct FileAck *)(out_ph + 1);
  uint16_t payload_len = sizeof(*ack);

  memcpy(out_eh->dest_mac_addr, g_file.client_mac, ETH_ALEN);
  memcpy(out_eh->src_mac_addr, g_pktdrv_info.mac_addr, ETH_ALEN);
  out_eh->ethertype = htons(g_ethertype);

  out_ph->signature = htonl(PACKET_SIGNATURE);
  out_ph->session_id = htonl(g_file.session_id);
  out_ph->payload_len = htons(payload_len);
  out_ph->pkt_type = htons(V1_FILE_ACK);

  ack->transfer_id = htonl(g_file.transfer_id);
  ack->command = htons(command);
  ack->status = htons(status);
  ack->offset = htonl(offset);

  pktdrv_send(g_send_buffer, COMBINED_HEADER_LEN + payload_len);
}

static void send_file_get_data(uint16_t status) {
  struct EthernetHeader *out_eh = (struct EthernetHeader *)(g_send_buffer);
  struct ProtocolHeader *out_ph = (struct ProtocolHeader *)(out_eh + 1);
  struct FileGetData *data = (struct FileGetData *)(out_ph + 1);
  uint8_t *payload = (uint8_t *)(data + 1);
  uint16_t payload_len = sizeof(*data);

  if (status == FILE_ACK_OK) {
    payload_len += g_file.chunk_count;
  } else {
    g_file.chunk_count = 0;
  }

  memcpy(out_eh->dest_mac_addr, g_file.client_mac, ETH_ALEN);
  memcpy(out_eh->src_mac_addr, g_pktdrv_info.mac_addr, ETH_ALEN);
  out_eh->ethertype = htons(g_ethertype);

  out_ph->signature = htonl(PACKET_SIGNATURE);
  out_ph->session_id = htonl(g_file.session_id);
  out_ph->payload_len = htons(payload_len);
  out_ph->pkt_type = htons(V1_FILE_GET_DATA);

  data->transfer_id = htonl(g_file.transfer_id);
  data->offset = htonl(g_file.chunk_offset);
  data->count = htons(g_file.chunk_count);
  data->status = htons(status);
  memcpy(payload, g_file.chunk, g_file.chunk_count);

  pktdrv_send(g_send_buffer, COMBINED_HEADER_LEN + payload_len);
}

static int dos_create_file(const char *filename) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x3c;
  regs.u.w.cx = 0;
  regs.u.w.dx = (uint16_t)filename;
  x86_call(0x21, &regs);

  if (regs.flags & CPU_FLAG_CARRY) {
    return -1;
  }
  return regs.u.w.ax;
}

static int dos_open_read_file(const char *filename) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x3d;
  regs.u.b.al = 0;
  regs.u.w.dx = (uint16_t)filename;
  x86_call(0x21, &regs);

  if (regs.flags & CPU_FLAG_CARRY) {
    return -1;
  }
  return regs.u.w.ax;
}

static uint32_t dos_seek_file(int handle, uint8_t origin, uint32_t offset) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x42;
  regs.u.b.al = origin;
  regs.u.w.bx = handle;
  regs.u.w.cx = offset >> 16;
  regs.u.w.dx = offset & 0xffff;
  x86_call(0x21, &regs);

  if (regs.flags & CPU_FLAG_CARRY) {
    return 0xffffffffUL;
  }
  return ((uint32_t)regs.u.w.dx << 16) | regs.u.w.ax;
}

static int dos_read_file(int handle, uint8_t *data, uint16_t count) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x3f;
  regs.u.w.bx = handle;
  regs.u.w.cx = count;
  regs.u.w.dx = (uint16_t)data;
  x86_call(0x21, &regs);

  if (regs.flags & CPU_FLAG_CARRY) {
    return -1;
  }
  return regs.u.w.ax;
}

static int dos_write_file(int handle, const uint8_t *data, uint16_t count) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x40;
  regs.u.w.bx = handle;
  regs.u.w.cx = count;
  regs.u.w.dx = (uint16_t)data;
  x86_call(0x21, &regs);

  if (regs.flags & CPU_FLAG_CARRY) {
    return -1;
  }
  return regs.u.w.ax == count ? 0 : -1;
}

static void dos_close_file(int handle) {
  struct CpuRegs regs;

  if (handle < 0) {
    return;
  }

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x3e;
  regs.u.w.bx = handle;
  x86_call(0x21, &regs);
}

void file_transfer_init() {
  memset(&g_file, 0, sizeof(g_file));
  g_file.handle = -1;
}

static void remember_client(const struct Buffer *buffer) {
  const struct EthernetHeader *in_eh =
      (const struct EthernetHeader *)(buffer->data);
  const struct ProtocolHeader *in_ph =
      (const struct ProtocolHeader *)(in_eh + 1);

  memcpy(g_file.client_mac, in_eh->src_mac_addr, ETH_ALEN);
  g_file.session_id = ntohl(in_ph->session_id);
}

static void queue_ack(const struct Buffer *buffer, uint32_t transfer_id,
                      uint16_t command, uint16_t status) {
  if (g_file.pending_op) {
    return;
  }
  remember_client(buffer);
  g_file.transfer_id = transfer_id;
  g_file.ack_command = command;
  g_file.ack_status = status;
  g_file.ack_offset = g_file.expected_offset;
  g_file.pending_op = FILE_OP_ACK;
}

void file_transfer_handle_packet(const struct Buffer *buffer) {
  const struct EthernetHeader *in_eh =
      (const struct EthernetHeader *)(buffer->data);
  const struct ProtocolHeader *in_ph =
      (const struct ProtocolHeader *)(in_eh + 1);
  const uint8_t *payload = (const uint8_t *)(in_ph + 1);
  uint16_t pkt_type = ntohs(in_ph->pkt_type);
  uint16_t payload_len = ntohs(in_ph->payload_len);

  if (g_file.pending_op) {
    return;
  }

  switch (pkt_type) {
    case V1_FILE_PUT_BEGIN: {
      const struct FilePutBegin *begin = (const struct FilePutBegin *)payload;
      if (payload_len < sizeof(*begin)) {
        return;
      }

      remember_client(buffer);
      g_file.transfer_id = ntohl(begin->transfer_id);
      g_file.size = ntohl(begin->size);
      memcpy(g_file.filename, begin->filename, sizeof(g_file.filename));
      g_file.filename[sizeof(g_file.filename) - 1] = '\0';
      g_file.pending_op = FILE_OP_PUT_BEGIN;
    } break;

    case V1_FILE_PUT_DATA: {
      const struct FilePutData *data = (const struct FilePutData *)payload;
      uint16_t count;
      uint32_t offset;
      uint32_t transfer_id;
      if (payload_len < sizeof(*data)) {
        return;
      }

      transfer_id = ntohl(data->transfer_id);
      count = ntohs(data->count);
      offset = ntohl(data->offset);

      if (!g_file.active || g_file.mode != FILE_MODE_PUT ||
          transfer_id != g_file.transfer_id) {
        return;
      }

      if (offset != g_file.expected_offset) {
        queue_ack(buffer, transfer_id, FILE_ACK_DATA, FILE_ACK_OK);
        return;
      }

      if (count > sizeof(g_file.chunk) ||
          payload_len < sizeof(*data) + count) {
        queue_ack(buffer, transfer_id, FILE_ACK_DATA, FILE_ACK_ERROR);
        return;
      }

      remember_client(buffer);
      g_file.chunk_offset = offset;
      g_file.chunk_count = count;
      memcpy(g_file.chunk, data + 1, count);
      g_file.pending_op = FILE_OP_PUT_DATA;
    } break;

    case V1_FILE_PUT_END: {
      const struct FilePutEnd *end = (const struct FilePutEnd *)payload;
      uint32_t transfer_id;
      if (payload_len < sizeof(*end)) {
        return;
      }

      transfer_id = ntohl(end->transfer_id);
      if (!g_file.active || g_file.mode != FILE_MODE_PUT ||
          transfer_id != g_file.transfer_id ||
          ntohl(end->size) != g_file.size) {
        return;
      }

      remember_client(buffer);
      g_file.pending_op = FILE_OP_PUT_END;
    } break;

    case V1_FILE_GET_BEGIN: {
      const struct FileGetBegin *begin = (const struct FileGetBegin *)payload;
      if (payload_len < sizeof(*begin)) {
        return;
      }

      remember_client(buffer);
      g_file.transfer_id = ntohl(begin->transfer_id);
      memcpy(g_file.filename, begin->filename, sizeof(g_file.filename));
      g_file.filename[sizeof(g_file.filename) - 1] = '\0';
      g_file.pending_op = FILE_OP_GET_BEGIN;
    } break;

    case V1_FILE_GET_DATA_REQ: {
      const struct FileGetDataReq *req = (const struct FileGetDataReq *)payload;
      uint16_t count;
      uint32_t offset;
      uint32_t transfer_id;
      if (payload_len < sizeof(*req)) {
        return;
      }

      transfer_id = ntohl(req->transfer_id);
      count = ntohs(req->count);
      offset = ntohl(req->offset);

      if (!g_file.active || g_file.mode != FILE_MODE_GET ||
          transfer_id != g_file.transfer_id) {
        return;
      }

      remember_client(buffer);
      g_file.chunk_offset = offset;
      g_file.chunk_count = count;
      g_file.pending_op = FILE_OP_GET_DATA_REQ;
    } break;

    case V1_FILE_GET_END: {
      const struct FileGetEnd *end = (const struct FileGetEnd *)payload;
      uint32_t transfer_id;
      if (payload_len < sizeof(*end)) {
        return;
      }

      transfer_id = ntohl(end->transfer_id);
      if (!g_file.active || g_file.mode != FILE_MODE_GET ||
          transfer_id != g_file.transfer_id) {
        return;
      }

      remember_client(buffer);
      g_file.pending_op = FILE_OP_GET_END;
    } break;
  }
}

void file_transfer_process_idle() {
  uint8_t op = g_file.pending_op;
  uint16_t status = FILE_ACK_OK;
  uint16_t command = FILE_ACK_BEGIN;
  uint32_t ack_offset;
  uint16_t flags;

  if (!op) {
    return;
  }

  g_file.pending_op = FILE_OP_NONE;

  switch (op) {
    case FILE_OP_PUT_BEGIN:
      if (g_file.active) {
        dos_close_file(g_file.handle);
      }
      g_file.handle = dos_create_file(g_file.filename);
      g_file.expected_offset = 0;
      g_file.active = g_file.handle >= 0;
      g_file.mode = g_file.active ? FILE_MODE_PUT : FILE_MODE_NONE;
      status = g_file.active ? FILE_ACK_OK : FILE_ACK_ERROR;
      command = FILE_ACK_BEGIN;
      break;

    case FILE_OP_PUT_DATA:
      command = FILE_ACK_DATA;
      if (g_file.chunk_offset != g_file.expected_offset ||
          dos_write_file(g_file.handle, g_file.chunk, g_file.chunk_count)) {
        status = FILE_ACK_ERROR;
      } else {
        g_file.expected_offset += g_file.chunk_count;
      }
      break;

    case FILE_OP_PUT_END:
      command = FILE_ACK_END;
      if (g_file.expected_offset != g_file.size) {
        status = FILE_ACK_ERROR;
      }
      dos_close_file(g_file.handle);
      g_file.handle = -1;
      g_file.active = 0;
      g_file.mode = FILE_MODE_NONE;
      break;

    case FILE_OP_ACK:
      command = g_file.ack_command;
      status = g_file.ack_status;
      g_file.expected_offset = g_file.ack_offset;
      break;

    case FILE_OP_GET_BEGIN:
      if (g_file.active) {
        dos_close_file(g_file.handle);
      }
      g_file.handle = dos_open_read_file(g_file.filename);
      g_file.active = 0;
      g_file.mode = FILE_MODE_NONE;
      g_file.expected_offset = 0;
      g_file.size = 0;
      if (g_file.handle >= 0) {
        g_file.size = dos_seek_file(g_file.handle, 2, 0);
        if (g_file.size != 0xffffffffUL &&
            dos_seek_file(g_file.handle, 0, 0) != 0xffffffffUL) {
          g_file.active = 1;
          g_file.mode = FILE_MODE_GET;
        }
      }
      if (!g_file.active) {
        dos_close_file(g_file.handle);
        g_file.handle = -1;
        status = FILE_ACK_ERROR;
      }
      command = FILE_ACK_BEGIN;
      break;

    case FILE_OP_GET_DATA_REQ:
      status = FILE_ACK_OK;
      if (!g_file.active || g_file.mode != FILE_MODE_GET ||
          g_file.chunk_count > FILE_TRANSFER_CHUNK_BYTES ||
          g_file.chunk_offset > g_file.size ||
          dos_seek_file(g_file.handle, 0, g_file.chunk_offset) ==
              0xffffffffUL) {
        status = FILE_ACK_ERROR;
        g_file.chunk_count = 0;
      } else {
        int read_count = dos_read_file(g_file.handle, g_file.chunk,
                                       g_file.chunk_count);
        if (read_count < 0) {
          status = FILE_ACK_ERROR;
          g_file.chunk_count = 0;
        } else {
          g_file.chunk_count = read_count;
          g_file.expected_offset = g_file.chunk_offset + g_file.chunk_count;
        }
      }
      flags = x86_cli();
      send_file_get_data(status);
      x86_sti(flags);
      return;

    case FILE_OP_GET_END:
      command = FILE_ACK_END;
      dos_close_file(g_file.handle);
      g_file.handle = -1;
      g_file.active = 0;
      g_file.mode = FILE_MODE_NONE;
      break;
  }

  ack_offset = g_file.expected_offset;
  if (op == FILE_OP_GET_BEGIN && status == FILE_ACK_OK) {
    ack_offset = g_file.size;
  }

  flags = x86_cli();
  send_file_ack(command, status, ack_offset);
  x86_sti(flags);
}
