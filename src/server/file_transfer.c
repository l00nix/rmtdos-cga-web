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
#define FILE_MODE_DIR 3

#define FILE_OP_NONE 0
#define FILE_OP_PUT_BEGIN 1
#define FILE_OP_PUT_DATA 2
#define FILE_OP_PUT_END 3
#define FILE_OP_ACK 4
#define FILE_OP_GET_BEGIN 5
#define FILE_OP_GET_DATA_REQ 6
#define FILE_OP_GET_END 7
#define FILE_OP_DIR_LIST_DATA_REQ 8

#define DOS_DTA_BYTES 43
#define DIR_LIST_PAGE_ENTRIES 16

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
  uint32_t dir_request_id;
  uint16_t dir_start_index;
  uint16_t dir_max_entries;
  char dir_path[RMTDOS_PATH_BYTES];
  uint8_t dta[DOS_DTA_BYTES];
  struct DirListEntry dir_entries[DIR_LIST_PAGE_ENTRIES];
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

static void send_dir_list_data(uint16_t status, uint16_t count) {
  struct EthernetHeader *out_eh = (struct EthernetHeader *)(g_send_buffer);
  struct ProtocolHeader *out_ph = (struct ProtocolHeader *)(out_eh + 1);
  struct DirListData *data = (struct DirListData *)(out_ph + 1);
  struct DirListEntry *entries = (struct DirListEntry *)(data + 1);
  uint16_t payload_len = sizeof(*data);

  if (status == FILE_ACK_OK) {
    payload_len += count * sizeof(struct DirListEntry);
  } else {
    count = 0;
  }

  memcpy(out_eh->dest_mac_addr, g_file.client_mac, ETH_ALEN);
  memcpy(out_eh->src_mac_addr, g_pktdrv_info.mac_addr, ETH_ALEN);
  out_eh->ethertype = htons(g_ethertype);

  out_ph->signature = htonl(PACKET_SIGNATURE);
  out_ph->session_id = htonl(g_file.session_id);
  out_ph->payload_len = htons(payload_len);
  out_ph->pkt_type = htons(V1_DIR_LIST_DATA);

  data->request_id = htonl(g_file.dir_request_id);
  data->start_index = htons(g_file.dir_start_index);
  data->entry_count = htons(count);
  data->status = htons(status);

  if (count) {
    memcpy(entries, g_file.dir_entries, count * sizeof(struct DirListEntry));
  }

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

static uint16_t dta_get_u16(uint16_t offset) {
  return ((uint16_t)g_file.dta[offset]) |
         (((uint16_t)g_file.dta[offset + 1]) << 8);
}

static uint32_t dta_get_u32(uint16_t offset) {
  return ((uint32_t)g_file.dta[offset]) |
         (((uint32_t)g_file.dta[offset + 1]) << 8) |
         (((uint32_t)g_file.dta[offset + 2]) << 16) |
         (((uint32_t)g_file.dta[offset + 3]) << 24);
}

static void dos_set_dta(uint8_t *dta) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x1a;
  regs.u.w.dx = (uint16_t)dta;
  x86_call(0x21, &regs);
}

static void dos_get_dta(uint16_t *segment, uint16_t *offset) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x2f;
  x86_call(0x21, &regs);

  *segment = regs.es;
  *offset = regs.u.w.bx;
}

static void dos_restore_dta(uint16_t segment, uint16_t offset) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x1a;
  regs.ds = segment;
  regs.u.w.dx = offset;
  x86_call(0x21, &regs);
}

static int dos_find_first(const char *pattern) {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x4e;
  regs.u.w.cx = 0x37;
  regs.u.w.dx = (uint16_t)pattern;
  x86_call(0x21, &regs);

  return (regs.flags & CPU_FLAG_CARRY) ? -1 : 0;
}

static int dos_find_next() {
  struct CpuRegs regs;

  x86_reset_regs(&regs);
  regs.u.b.ah = 0x4f;
  x86_call(0x21, &regs);

  return (regs.flags & CPU_FLAG_CARRY) ? -1 : 0;
}

static int has_wildcard(const char *s) {
  while (*s) {
    if (*s == '*' || *s == '?') {
      return 1;
    }
    ++s;
  }
  return 0;
}

static int build_dir_pattern(char *dest, uint16_t dest_len, const char *path) {
  uint16_t i = 0;
  uint16_t len;

  if (!path[0]) {
    path = ".";
  }

  while (path[i] && i + 1 < dest_len) {
    dest[i] = path[i] == '/' ? '\\' : path[i];
    ++i;
  }

  if (path[i]) {
    return -1;
  }

  dest[i] = '\0';
  if (has_wildcard(dest)) {
    return 0;
  }

  len = i;
  if (!len) {
    return -1;
  }

  if (dest[len - 1] == '\\') {
    if (len + 4 >= dest_len) {
      return -1;
    }
    strcpy(dest + len, "*.*");
  } else if (dest[len - 1] == ':') {
    if (len + 5 >= dest_len) {
      return -1;
    }
    strcpy(dest + len, "\\*.*");
  } else {
    if (len + 5 >= dest_len) {
      return -1;
    }
    strcpy(dest + len, "\\*.*");
  }

  return 0;
}

static void copy_dta_name(char *dest, uint16_t dest_len) {
  const char *src = (const char *)(g_file.dta + 30);
  uint16_t i = 0;

  while (src[i] && i + 1 < dest_len) {
    dest[i] = src[i];
    ++i;
  }
  dest[i] = '\0';
}

static uint16_t collect_dir_entries(uint16_t *status) {
  char pattern[RMTDOS_PATH_BYTES + 5];
  uint16_t seen = 0;
  uint16_t count = 0;
  uint16_t max_entries = g_file.dir_max_entries;
  uint16_t saved_dta_segment;
  uint16_t saved_dta_offset;

  if (max_entries > DIR_LIST_PAGE_ENTRIES) {
    max_entries = DIR_LIST_PAGE_ENTRIES;
  }

  *status = FILE_ACK_OK;
  if (build_dir_pattern(pattern, sizeof(pattern), g_file.dir_path)) {
    *status = FILE_ACK_ERROR;
    return 0;
  }

  dos_get_dta(&saved_dta_segment, &saved_dta_offset);
  dos_set_dta(g_file.dta);
  if (dos_find_first(pattern)) {
    dos_restore_dta(saved_dta_segment, saved_dta_offset);
    return 0;
  }

  do {
    if (seen >= g_file.dir_start_index && count < max_entries) {
      struct DirListEntry *entry = &g_file.dir_entries[count];
      memset(entry, 0, sizeof(*entry));
      entry->attributes = g_file.dta[21];
      entry->size = htonl(dta_get_u32(26));
      entry->dos_time = htons(dta_get_u16(22));
      entry->dos_date = htons(dta_get_u16(24));
      copy_dta_name(entry->name, sizeof(entry->name));
      ++count;
    }
    ++seen;
  } while (count < max_entries && !dos_find_next());

  dos_restore_dta(saved_dta_segment, saved_dta_offset);
  return count;
}

static uint32_t dos_get_file_size(const char *filename) {
  uint16_t saved_dta_segment;
  uint16_t saved_dta_offset;
  uint32_t size;

  dos_get_dta(&saved_dta_segment, &saved_dta_offset);
  dos_set_dta(g_file.dta);
  if (dos_find_first(filename)) {
    dos_restore_dta(saved_dta_segment, saved_dta_offset);
    return 0xffffffffUL;
  }

  size = dta_get_u32(26);
  dos_restore_dta(saved_dta_segment, saved_dta_offset);
  return size;
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

    case V1_DIR_LIST_BEGIN: {
      const struct DirListBegin *begin = (const struct DirListBegin *)payload;
      if (payload_len < sizeof(*begin)) {
        return;
      }

      remember_client(buffer);
      g_file.dir_request_id = ntohl(begin->request_id);
      memcpy(g_file.dir_path, begin->path, sizeof(g_file.dir_path));
      g_file.dir_path[sizeof(g_file.dir_path) - 1] = '\0';
      g_file.active = 1;
      g_file.mode = FILE_MODE_DIR;
    } break;

    case V1_DIR_LIST_DATA_REQ: {
      const struct DirListDataReq *req = (const struct DirListDataReq *)payload;
      uint32_t request_id;
      if (payload_len < sizeof(*req)) {
        return;
      }

      request_id = ntohl(req->request_id);
      if (!g_file.active || g_file.mode != FILE_MODE_DIR ||
          request_id != g_file.dir_request_id) {
        return;
      }

      remember_client(buffer);
      g_file.dir_start_index = ntohs(req->start_index);
      g_file.dir_max_entries = ntohs(req->max_entries);
      g_file.pending_op = FILE_OP_DIR_LIST_DATA_REQ;
    } break;

    case V1_DIR_LIST_END: {
      const struct DirListEnd *end = (const struct DirListEnd *)payload;
      if (payload_len < sizeof(*end) ||
          ntohl(end->request_id) != g_file.dir_request_id) {
        return;
      }
      g_file.active = 0;
      g_file.mode = FILE_MODE_NONE;
      g_file.dir_request_id = 0;
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
        g_file.size = dos_get_file_size(g_file.filename);
        if (g_file.size != 0xffffffffUL) {
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

    case FILE_OP_DIR_LIST_DATA_REQ: {
      uint16_t dir_status;
      uint16_t count = collect_dir_entries(&dir_status);
      flags = x86_cli();
      send_dir_list_data(dir_status, count);
      x86_sti(flags);
      return;
    }
  }

  ack_offset = g_file.expected_offset;
  if (op == FILE_OP_GET_BEGIN && status == FILE_ACK_OK) {
    ack_offset = g_file.size;
  }

  flags = x86_cli();
  send_file_ack(command, status, ack_offset);
  x86_sti(flags);
}
