/*
 * Copyright 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client/globals.h"
#include "client/hostlist.h"
#include "client/web.h"

#define WEB_FRAME_HEADER_BYTES 16

static const char WEB_INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>rmtdos CGA</title>"
    "<style>"
    "html,body{height:100%;margin:0;background:#111;color:#ddd;"
    "font:14px system-ui,sans-serif}"
    "body{display:grid;place-items:center}"
    "main{display:grid;gap:12px;justify-items:center}"
    "canvas{image-rendering:pixelated;background:#000;"
    "width:min(96vw,1280px);height:auto;border:1px solid #444}"
    "#status{color:#aaa}"
    "</style></head><body><main>"
    "<canvas id=\"screen\" width=\"640\" height=\"200\"></canvas>"
    "<div id=\"status\">Waiting for CGA frames...</div>"
    "</main><script>"
    "const c=document.getElementById('screen'),x=c.getContext('2d'),"
    "s=document.getElementById('status');"
    "function le16(d,o){return d.getUint16(o,true)}"
    "function le32(d,o){return d.getUint32(o,true)}"
    "const pal04=[[0,0,0],[0,170,0],[170,0,0],[170,85,0]],"
    "pal05=[[0,0,0],[0,170,170],[170,0,170],[170,170,170]];"
    "function pix1(data,w,h){const img=x.createImageData(w,h);"
    "for(let y=0;y<h;y++){let ro=(y&1?0x2000:0)+((y>>1)*80);"
    "for(let px=0;px<w;px++){let b=data[ro+(px>>3)],v=(b>>(7-(px&7)))&1,"
    "g=v?255:0,i=(y*w+px)*4;img.data[i]=img.data[i+1]=img.data[i+2]=g;"
    "img.data[i+3]=255}}return img}"
    "function pix2(data,w,h,mode){const img=x.createImageData(w,h),"
    "p=mode===5?pal05:pal04;"
    "for(let y=0;y<h;y++){let ro=(y&1?0x2000:0)+((y>>1)*80);"
    "for(let px=0;px<w;px++){let b=data[ro+(px>>2)],v=(b>>(6-((px&3)*2)))&3,"
    "rgb=p[v],i=(y*w+px)*4;img.data[i]=rgb[0];img.data[i+1]=rgb[1];"
    "img.data[i+2]=rgb[2];"
    "img.data[i+3]=255}}return img}"
    "async function tick(){try{const r=await fetch('/frame',{cache:'no-store'});"
    "if(r.status===204){setTimeout(tick,250);return}"
    "const a=await r.arrayBuffer(),d=new DataView(a);"
    "if(d.getUint8(0)!==67||d.getUint8(1)!==71||d.getUint8(2)!==65)return;"
    "const mode=d.getUint8(4),bpp=d.getUint8(5),w=le16(d,6),h=le16(d,8),"
    "gen=le32(d,10),bytes=le16(d,14),data=new Uint8Array(a,16,bytes);"
    "if(c.width!==w||c.height!==h){c.width=w;c.height=h}"
    "x.putImageData(bpp===1?pix1(data,w,h):pix2(data,w,h,mode),0,0);"
    "s.textContent=`mode ${mode.toString(16)}h, ${w}x${h}x${bpp}, frame ${gen}`"
    "}catch(e){s.textContent=e.message}setTimeout(tick,150)}tick();"
    "</script></body></html>";

static int send_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  while (len) {
    ssize_t n = send(fd, p, len, 0);
    if (n <= 0) {
      return -1;
    }
    p += n;
    len -= n;
  }
  return 0;
}

static void header_u16(uint8_t *header, int offset, uint16_t value) {
  header[offset] = value & 0xff;
  header[offset + 1] = value >> 8;
}

static void header_u32(uint8_t *header, int offset, uint32_t value) {
  header[offset] = value & 0xff;
  header[offset + 1] = (value >> 8) & 0xff;
  header[offset + 2] = (value >> 16) & 0xff;
  header[offset + 3] = (value >> 24) & 0xff;
}

static struct RemoteHost *web_find_graphics_host() {
  if (g_active_host && g_active_host->cga_graphics_valid) {
    return g_active_host;
  }

  int iter = 0;
  struct RemoteHost *rh;
  while (NULL != (rh = hostlist_iter(&iter))) {
    if (rh->cga_graphics_valid) {
      return rh;
    }
  }

  return NULL;
}

static void web_send_index(int fd) {
  char header[256];
  int header_len =
      snprintf(header, sizeof(header),
               "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/html; charset=utf-8\r\n"
               "Content-Length: %zu\r\n"
               "Connection: close\r\n\r\n",
               sizeof(WEB_INDEX_HTML) - 1);
  send_all(fd, header, header_len);
  send_all(fd, WEB_INDEX_HTML, sizeof(WEB_INDEX_HTML) - 1);
}

static void web_send_no_frame(int fd) {
  static const char response[] =
      "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
  send_all(fd, response, sizeof(response) - 1);
}

static void web_send_frame(int fd) {
  struct RemoteHost *rh = web_find_graphics_host();
  if (!rh) {
    web_send_no_frame(fd);
    return;
  }

  char response[256];
  const size_t body_len = WEB_FRAME_HEADER_BYTES + CGA_GRAPHICS_FRAME_BYTES;
  int response_len =
      snprintf(response, sizeof(response),
               "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/octet-stream\r\n"
               "Content-Length: %zu\r\n"
               "Cache-Control: no-store\r\n"
               "Connection: close\r\n\r\n",
               body_len);

  uint8_t header[WEB_FRAME_HEADER_BYTES] = {'C', 'G', 'A', '1'};
  header[4] = rh->cga_graphics_mode;
  header[5] = rh->cga_graphics_bpp;
  header_u16(header, 6, rh->cga_graphics_width);
  header_u16(header, 8, rh->cga_graphics_height);
  header_u32(header, 10, rh->cga_graphics_generation);
  header_u16(header, 14, CGA_GRAPHICS_FRAME_BYTES);

  send_all(fd, response, response_len);
  send_all(fd, header, sizeof(header));
  send_all(fd, rh->cga_graphics_buffer, sizeof(rh->cga_graphics_buffer));
}

int web_server_start(struct WebServer *server, const char *addr, int port) {
  int opt = 1;
  struct sockaddr_in sa;

  server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server->listen_fd < 0) {
    perror("web socket");
    return -1;
  }

  setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
    fprintf(stderr, "invalid web address: %s\n", addr);
    close(server->listen_fd);
    server->listen_fd = -1;
    return -1;
  }

  if (bind(server->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    perror("web bind");
    close(server->listen_fd);
    server->listen_fd = -1;
    return -1;
  }

  if (listen(server->listen_fd, 8) < 0) {
    perror("web listen");
    close(server->listen_fd);
    server->listen_fd = -1;
    return -1;
  }

  return 0;
}

void web_server_process(struct WebServer *server) {
  char request[1024];
  int fd = accept(server->listen_fd, NULL, NULL);
  if (fd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("web accept");
    }
    return;
  }

  ssize_t n = recv(fd, request, sizeof(request) - 1, 0);
  if (n > 0) {
    request[n] = '\0';
    if (!strncmp(request, "GET /frame", 10)) {
      web_send_frame(fd);
    } else {
      web_send_index(fd);
    }
  }

  close(fd);
}

void web_server_close(struct WebServer *server) {
  if (server->listen_fd >= 0) {
    close(server->listen_fd);
    server->listen_fd = -1;
  }
}
