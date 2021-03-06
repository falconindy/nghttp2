/*
 * nghttp2 - HTTP/2.0 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef SHRPX_CLIENT_HANDLER_H
#define SHRPX_CLIENT_HANDLER_H

#include "shrpx.h"

#include <set>

#include <event.h>
#include <openssl/ssl.h>

namespace shrpx {

class Upstream;
class DownstreamConnection;
class SpdySession;
class HttpsUpstream;

class ClientHandler {
public:
  ClientHandler(bufferevent *bev, int fd, SSL *ssl, const char *ipaddr);
  ~ClientHandler();
  int on_read();
  int on_event();
  bufferevent* get_bev() const;
  event_base* get_evbase() const;
  void set_bev_cb(bufferevent_data_cb readcb, bufferevent_data_cb writecb,
                  bufferevent_event_cb eventcb);
  void set_upstream_timeouts(const timeval *read_timeout,
                             const timeval *write_timeout);
  int validate_next_proto();
  const std::string& get_ipaddr() const;
  bool get_should_close_after_write() const;
  void set_should_close_after_write(bool f);
  Upstream* get_upstream();

  void pool_downstream_connection(DownstreamConnection *dconn);
  void remove_downstream_connection(DownstreamConnection *dconn);
  DownstreamConnection* get_downstream_connection();
  size_t get_pending_write_length();
  SSL* get_ssl() const;
  void set_spdy_session(SpdySession *spdy);
  SpdySession* get_spdy_session() const;
  size_t get_left_connhd_len() const;
  void set_left_connhd_len(size_t left);
  // Call this function when HTTP/2.0 connection header is received at
  // the start of the connection.
  void direct_http2_upgrade();
  // Performs HTTP/2.0 Upgrade from the connection managed by
  // |http|. If this function fails, the connection must be
  // terminated. This function returns 0 if it succeeds, or -1.
  int perform_http2_upgrade(HttpsUpstream *http);
  bool get_http2_upgrade_allowed() const;
private:
  bufferevent *bev_;
  ev_token_bucket_cfg *evbucket_cfg_;
  int fd_;
  SSL *ssl_;
  Upstream *upstream_;
  std::string ipaddr_;
  bool should_close_after_write_;
  std::set<DownstreamConnection*> dconn_pool_;
  // Shared SPDY session for each thread. NULL if backend is not
  // SPDY. Not deleted by this object.
  SpdySession *spdy_;
  // The number of bytes of HTTP/2.0 client connection header to read
  size_t left_connhd_len_;
};

} // namespace shrpx

#endif // SHRPX_CLIENT_HANDLER_H
