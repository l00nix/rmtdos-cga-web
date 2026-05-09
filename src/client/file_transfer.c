/*
 * Copyright 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <net/ethernet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "client/file_transfer.h"
#include "common/protocol.h"

#define FILE_RETRIES 10
#define ACK_TIMEOUT_SEC 2

static uint32_t make_transfer_id(struct RawSocket *sock, uint32_t salt) {
  return sock->session_id ^ salt;
}

static int recv_file_ack(struct RawSocket *sock, const uint8_t *dest_mac_addr,
                         uint32_t transfer_id, uint16_t command,
                         uint32_t *offset) {
  uint8_t buf[ETH_FRAME_LEN];
  ssize_t received;

  for (;;) {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(sock->sock_fd, &fds);
    tv.tv_sec = ACK_TIMEOUT_SEC;
    tv.tv_usec = 0;

    if (select(sock->sock_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
      return -1;
    }

    received = recv(sock->sock_fd, buf, sizeof(buf), 0);
    if (received < (ssize_t)(sizeof(struct ether_header) +
                             sizeof(struct ProtocolHeader) +
                             sizeof(struct FileAck))) {
      continue;
    }

    const struct ether_header *eh = (const struct ether_header *)buf;
    const struct ProtocolHeader *ph = (const struct ProtocolHeader *)(eh + 1);
    const struct FileAck *ack = (const struct FileAck *)(ph + 1);

    if (memcmp(eh->ether_shost, dest_mac_addr, ETH_ALEN) ||
        memcmp(eh->ether_dhost, sock->if_addr, ETH_ALEN) ||
        ntohl(ph->signature) != PACKET_SIGNATURE ||
        ntohl(ph->session_id) != sock->session_id ||
        ntohs(ph->pkt_type) != V1_FILE_ACK ||
        ntohs(ph->payload_len) < sizeof(*ack) ||
        ntohl(ack->transfer_id) != transfer_id ||
        ntohs(ack->command) != command) {
      continue;
    }

    if (ntohs(ack->status) != FILE_ACK_OK) {
      return -1;
    }

    *offset = ntohl(ack->offset);
    return 0;
  }
}

static int send_with_ack(struct RawSocket *sock, const uint8_t *dest_mac_addr,
                         enum PKT_TYPE pkt_type, const void *payload,
                         size_t payload_len, uint32_t transfer_id,
                         uint16_t ack_command, uint32_t *ack_offset) {
  int attempt;

  for (attempt = 0; attempt < FILE_RETRIES; ++attempt) {
    if (send_packet(sock, dest_mac_addr, pkt_type, payload, payload_len) < 0) {
      return -1;
    }

    if (!recv_file_ack(sock, dest_mac_addr, transfer_id, ack_command,
                       ack_offset)) {
      return 0;
    }
  }

  fprintf(stderr, "timed out waiting for file-transfer ACK\n");
  return -1;
}

static int recv_file_get_data(struct RawSocket *sock,
                              const uint8_t *dest_mac_addr,
                              uint32_t transfer_id, uint32_t offset,
                              uint8_t *data, uint16_t *count) {
  uint8_t buf[ETH_FRAME_LEN];
  ssize_t received;

  for (;;) {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(sock->sock_fd, &fds);
    tv.tv_sec = ACK_TIMEOUT_SEC;
    tv.tv_usec = 0;

    if (select(sock->sock_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
      return -1;
    }

    received = recv(sock->sock_fd, buf, sizeof(buf), 0);
    if (received < (ssize_t)(sizeof(struct ether_header) +
                             sizeof(struct ProtocolHeader) +
                             sizeof(struct FileGetData))) {
      continue;
    }

    const struct ether_header *eh = (const struct ether_header *)buf;
    const struct ProtocolHeader *ph = (const struct ProtocolHeader *)(eh + 1);
    const struct FileGetData *resp = (const struct FileGetData *)(ph + 1);
    uint16_t payload_len = ntohs(ph->payload_len);
    uint16_t resp_count = ntohs(resp->count);

    if (memcmp(eh->ether_shost, dest_mac_addr, ETH_ALEN) ||
        memcmp(eh->ether_dhost, sock->if_addr, ETH_ALEN) ||
        ntohl(ph->signature) != PACKET_SIGNATURE ||
        ntohl(ph->session_id) != sock->session_id ||
        ntohs(ph->pkt_type) != V1_FILE_GET_DATA ||
        payload_len < sizeof(*resp) ||
        ntohl(resp->transfer_id) != transfer_id ||
        ntohl(resp->offset) != offset) {
      continue;
    }

    if (ntohs(resp->status) != FILE_ACK_OK ||
        resp_count > FILE_TRANSFER_CHUNK_BYTES ||
        payload_len < sizeof(*resp) + resp_count) {
      return -1;
    }

    memcpy(data, resp + 1, resp_count);
    *count = resp_count;
    return 0;
  }
}

static int send_with_data_response(struct RawSocket *sock,
                                   const uint8_t *dest_mac_addr,
                                   const struct FileGetDataReq *req,
                                   uint32_t transfer_id, uint32_t offset,
                                   uint8_t *data, uint16_t *count) {
  int attempt;

  for (attempt = 0; attempt < FILE_RETRIES; ++attempt) {
    if (send_packet(sock, dest_mac_addr, V1_FILE_GET_DATA_REQ, req,
                    sizeof(*req)) < 0) {
      return -1;
    }

    if (!recv_file_get_data(sock, dest_mac_addr, transfer_id, offset, data,
                            count)) {
      return 0;
    }
  }

  fprintf(stderr, "timed out waiting for file-transfer data\n");
  return -1;
}

static int validate_remote_name(const char *remote_path) {
  size_t len = strlen(remote_path);

  if (!len || len >= FILE_TRANSFER_NAME_BYTES) {
    fprintf(stderr, "remote filename must be 1-%d characters\n",
            FILE_TRANSFER_NAME_BYTES - 1);
    return -1;
  }

  if (strchr(remote_path, '/')) {
    fprintf(stderr, "remote filename must use DOS separators, not '/'\n");
    return -1;
  }

  return 0;
}

int file_transfer_put(struct RawSocket *sock, const uint8_t *dest_mac_addr,
                      const char *local_path, const char *remote_path) {
  struct stat st;
  FILE *fp;
  uint32_t transfer_id = make_transfer_id(sock, 0x66554c45);
  uint32_t offset = 0;
  uint32_t ack_offset = 0;
  size_t n;

  if (validate_remote_name(remote_path)) {
    return -1;
  }

  if (stat(local_path, &st)) {
    perror(local_path);
    return -1;
  }

  if (st.st_size < 0 || st.st_size > 0xffffffffLL) {
    fprintf(stderr, "file is too large for this transfer protocol\n");
    return -1;
  }

  fp = fopen(local_path, "rb");
  if (!fp) {
    perror(local_path);
    return -1;
  }

  struct FilePutBegin begin;
  memset(&begin, 0, sizeof(begin));
  begin.transfer_id = htonl(transfer_id);
  begin.size = htonl((uint32_t)st.st_size);
  snprintf(begin.filename, sizeof(begin.filename), "%s", remote_path);

  printf("upload: %s -> %s (%lu bytes)\n", local_path, remote_path,
         (unsigned long)st.st_size);

  if (send_with_ack(sock, dest_mac_addr, V1_FILE_PUT_BEGIN, &begin,
                    sizeof(begin), transfer_id, FILE_ACK_BEGIN,
                    &ack_offset) ||
      ack_offset != 0) {
    fclose(fp);
    return -1;
  }

  for (;;) {
    uint8_t packet[sizeof(struct FilePutData) + FILE_TRANSFER_CHUNK_BYTES];
    struct FilePutData *data = (struct FilePutData *)packet;
    uint8_t *payload = (uint8_t *)(data + 1);

    n = fread(payload, 1, FILE_TRANSFER_CHUNK_BYTES, fp);
    if (!n) {
      break;
    }

    data->transfer_id = htonl(transfer_id);
    data->offset = htonl(offset);
    data->count = htons(n);

    if (send_with_ack(sock, dest_mac_addr, V1_FILE_PUT_DATA, data,
                      sizeof(*data) + n, transfer_id, FILE_ACK_DATA,
                      &ack_offset) ||
        ack_offset != offset + n) {
      fclose(fp);
      return -1;
    }

    offset = ack_offset;
    printf("\r%lu/%lu bytes", (unsigned long)offset, (unsigned long)st.st_size);
    fflush(stdout);
  }

  if (ferror(fp)) {
    perror(local_path);
    fclose(fp);
    return -1;
  }

  printf("\n");
  fclose(fp);

  struct FilePutEnd end;
  end.transfer_id = htonl(transfer_id);
  end.size = htonl((uint32_t)st.st_size);

  if (send_with_ack(sock, dest_mac_addr, V1_FILE_PUT_END, &end, sizeof(end),
                    transfer_id, FILE_ACK_END, &ack_offset) ||
      ack_offset != (uint32_t)st.st_size) {
    return -1;
  }

  printf("upload complete\n");
  return 0;
}

int file_transfer_get(struct RawSocket *sock, const uint8_t *dest_mac_addr,
                      const char *remote_path, const char *local_path) {
  FILE *fp;
  uint32_t transfer_id = make_transfer_id(sock, 0x47455421);
  uint32_t offset = 0;
  uint32_t file_size = 0;
  uint32_t ack_offset = 0;

  if (validate_remote_name(remote_path)) {
    return -1;
  }

  struct FileGetBegin begin;
  memset(&begin, 0, sizeof(begin));
  begin.transfer_id = htonl(transfer_id);
  snprintf(begin.filename, sizeof(begin.filename), "%s", remote_path);

  printf("download: %s -> %s\n", remote_path, local_path);

  if (send_with_ack(sock, dest_mac_addr, V1_FILE_GET_BEGIN, &begin,
                    sizeof(begin), transfer_id, FILE_ACK_BEGIN,
                    &ack_offset)) {
    return -1;
  }
  file_size = ack_offset;

  fp = fopen(local_path, "wb");
  if (!fp) {
    perror(local_path);
    return -1;
  }

  while (offset < file_size) {
    uint8_t chunk[FILE_TRANSFER_CHUNK_BYTES];
    uint16_t count;
    uint32_t remaining = file_size - offset;
    struct FileGetDataReq req;

    count = remaining > FILE_TRANSFER_CHUNK_BYTES
                ? FILE_TRANSFER_CHUNK_BYTES
                : remaining;
    req.transfer_id = htonl(transfer_id);
    req.offset = htonl(offset);
    req.count = htons(count);

    if (send_with_data_response(sock, dest_mac_addr, &req, transfer_id, offset,
                                chunk, &count) ||
        count == 0 || count > remaining) {
      fclose(fp);
      return -1;
    }

    if (fwrite(chunk, 1, count, fp) != count) {
      perror(local_path);
      fclose(fp);
      return -1;
    }

    offset += count;
    printf("\r%lu/%lu bytes", (unsigned long)offset,
           (unsigned long)file_size);
    fflush(stdout);
  }

  if (fclose(fp)) {
    perror(local_path);
    return -1;
  }

  printf("\n");

  struct FileGetEnd end;
  end.transfer_id = htonl(transfer_id);
  if (send_with_ack(sock, dest_mac_addr, V1_FILE_GET_END, &end, sizeof(end),
                    transfer_id, FILE_ACK_END, &ack_offset)) {
    return -1;
  }

  printf("download complete\n");
  return 0;
}
