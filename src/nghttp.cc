/*
 * nghttp2 - HTTP/2.0 C Library
 *
 * Copyright (c) 2013 Tatsuhiro Tsujikawa
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
#include "nghttp2_config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <getopt.h>

#include <cassert>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iostream>
#include <string>
#include <set>
#include <iomanip>
#include <fstream>
#include <map>
#include <vector>
#include <sstream>
#include <tuple>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <event.h>
#include <event2/bufferevent_ssl.h>
#include <event2/dns.h>

#include <nghttp2/nghttp2.h>

#include "http-parser/http_parser.h"

#include "app_helper.h"
#include "HtmlParser.h"
#include "util.h"
#include "base64.h"

#ifndef O_BINARY
# define O_BINARY (0)
#endif // O_BINARY

namespace nghttp2 {

struct Config {
  bool null_out;
  bool remote_name;
  bool verbose;
  bool get_assets;
  bool stat;
  bool no_flow_control;
  bool upgrade;
  int32_t pri;
  int multiply;
  // milliseconds
  int timeout;
  std::string certfile;
  std::string keyfile;
  int window_bits;
  std::map<std::string, std::string> headers;
  std::string datafile;
  size_t output_upper_thres;
  Config()
    : null_out(false),
      remote_name(false),
      verbose(false),
      get_assets(false),
      stat(false),
      no_flow_control(false),
      upgrade(false),
      pri(NGHTTP2_PRI_DEFAULT),
      multiply(1),
      timeout(-1),
      window_bits(-1),
      output_upper_thres(1024*1024)
  {}
};

struct RequestStat {
  timeval on_syn_stream_time;
  timeval on_syn_reply_time;
  timeval on_complete_time;
  RequestStat()
  {
    on_syn_stream_time.tv_sec = -1;
    on_syn_stream_time.tv_usec = -1;
    on_syn_reply_time.tv_sec = -1;
    on_syn_reply_time.tv_usec = -1;
    on_complete_time.tv_sec = -1;
    on_complete_time.tv_usec = -1;
  }
};

void record_time(timeval *tv)
{
  get_time(tv);
}

bool has_uri_field(const http_parser_url &u, http_parser_url_fields field)
{
  return u.field_set & (1 << field);
}

bool fieldeq(const char *uri1, const http_parser_url &u1,
             const char *uri2, const http_parser_url &u2,
             http_parser_url_fields field)
{
  if(!has_uri_field(u1, field)) {
    if(!has_uri_field(u2, field)) {
      return true;
    } else {
      return false;
    }
  } else if(!has_uri_field(u2, field)) {
    return false;
  }
  if(u1.field_data[field].len != u2.field_data[field].len) {
    return false;
  }
  return memcmp(uri1+u1.field_data[field].off,
                uri2+u2.field_data[field].off,
                u1.field_data[field].len) == 0;
}

bool fieldeq(const char *uri, const http_parser_url &u,
             http_parser_url_fields field,
             const char *t)
{
  if(!has_uri_field(u, field)) {
    if(!t[0]) {
      return true;
    } else {
      return false;
    }
  } else if(!t[0]) {
    return false;
  }
  int i, len = u.field_data[field].len;
  const char *p = uri+u.field_data[field].off;
  for(i = 0; i < len && t[i] && p[i] == t[i]; ++i);
  return i == len && !t[i];
}

uint16_t get_default_port(const char *uri, const http_parser_url &u)
{
  if(fieldeq(uri, u, UF_SCHEMA, "https")) {
    return 443;
  } else if(fieldeq(uri, u, UF_SCHEMA, "http")) {
    return 80;
  } else {
    return 443;
  }
}

std::string get_uri_field(const char *uri, const http_parser_url &u,
                          http_parser_url_fields field)
{
  if(has_uri_field(u, field)) {
    return std::string(uri+u.field_data[field].off,
                       u.field_data[field].len);
  } else {
    return "";
  }
}

bool porteq(const char *uri1, const http_parser_url &u1,
            const char *uri2, const http_parser_url &u2)
{
  uint16_t port1, port2;
  port1 = has_uri_field(u1, UF_PORT) ? u1.port : get_default_port(uri1, u1);
  port2 = has_uri_field(u2, UF_PORT) ? u2.port : get_default_port(uri2, u2);
  return port1 == port2;
}

void write_uri_field(std::ostream& o,
                     const char *uri, const http_parser_url &u,
                     http_parser_url_fields field)
{
  if(has_uri_field(u, field)) {
    o.write(uri+u.field_data[field].off, u.field_data[field].len);
  }
}

std::string strip_fragment(const char *raw_uri)
{
  const char *end;
  for(end = raw_uri; *end && *end != '#'; ++end);
  size_t len = end-raw_uri;
  return std::string(raw_uri, len);
}

struct Request {
  // URI without fragment
  std::string uri;
  http_parser_url u;
  nghttp2_gzip *inflater;
  HtmlParser *html_parser;
  const nghttp2_data_provider *data_prd;
  int64_t data_length;
  int64_t data_offset;
  // Recursion level: 0: first entity, 1: entity linked from first entity
  int level;
  RequestStat stat;
  std::string status;
  Request(const std::string& uri, const http_parser_url &u,
          const nghttp2_data_provider *data_prd, int64_t data_length,
          int level = 0)
    : uri(uri), u(u),
      inflater(nullptr), html_parser(nullptr), data_prd(data_prd),
      data_length(data_length), data_offset(0),
      level(level)
  {}

  ~Request()
  {
    nghttp2_gzip_inflate_del(inflater);
    delete html_parser;
  }

  void init_inflater()
  {
    int rv;
    rv = nghttp2_gzip_inflate_new(&inflater);
    assert(rv == 0);
  }

  void init_html_parser()
  {
    html_parser = new HtmlParser(uri);
  }

  int update_html_parser(const uint8_t *data, size_t len, int fin)
  {
    if(!html_parser) {
      return 0;
    }
    int rv;
    rv = html_parser->parse_chunk(reinterpret_cast<const char*>(data), len,
                                  fin);
    return rv;
  }

  std::string make_reqpath() const
  {
    std::string path = has_uri_field(u, UF_PATH) ?
      get_uri_field(uri.c_str(), u, UF_PATH) : "/";
    if(has_uri_field(u, UF_QUERY)) {
      path += "?";
      path.append(uri.c_str()+u.field_data[UF_QUERY].off,
                  u.field_data[UF_QUERY].len);
    }
    return path;
  }

  bool is_ipv6_literal_addr() const
  {
    if(has_uri_field(u, UF_HOST)) {
      return memchr(uri.c_str()+u.field_data[UF_HOST].off, ':',
                    u.field_data[UF_HOST].len);
    } else {
      return false;
    }
  }

  void record_syn_stream_time()
  {
    record_time(&stat.on_syn_stream_time);
  }

  void record_syn_reply_time()
  {
    record_time(&stat.on_syn_reply_time);
  }

  void record_complete_time()
  {
    record_time(&stat.on_complete_time);
  }
};

struct SessionStat {
  timeval on_handshake_time;
  SessionStat()
  {
    on_handshake_time.tv_sec = -1;
    on_handshake_time.tv_usec = -1;
  }
};

Config config;

namespace {
size_t populate_settings(nghttp2_settings_entry *iv)
{
  size_t niv = 2;
  iv[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
  iv[0].value = 100;
  iv[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
  if(config.window_bits != -1) {
    iv[1].value = (1 << config.window_bits) - 1;
  } else {
    iv[1].value = NGHTTP2_INITIAL_WINDOW_SIZE;
  }
  if(config.no_flow_control) {
    iv[niv].settings_id = NGHTTP2_SETTINGS_FLOW_CONTROL_OPTIONS;
    iv[niv].value = 1;
    ++niv;
  }
  return niv;
}
} // namespace

namespace {
void eventcb(bufferevent *bev, short events, void *ptr);
} // namespace

namespace {
extern http_parser_settings htp_hooks;
} // namespace

namespace {
void upgrade_readcb(bufferevent *bev, void *ptr);
} // namespace

namespace {
void readcb(bufferevent *bev, void *ptr);
} // namespace

namespace {
void writecb(bufferevent *bev, void *ptr);
} // namespace

struct HttpClient;

namespace {
void submit_request(HttpClient *client,
                    const std::map<std::string, std::string>& headers,
                    Request *req);
} // namespace

namespace {
void check_stream_id(nghttp2_session *session, int32_t stream_id,
                     void *user_data);
} // namespace

enum client_state {
  STATE_IDLE,
  STATE_CONNECTED
};

struct HttpClient {
  nghttp2_session *session;
  const nghttp2_session_callbacks *callbacks;
  event_base *evbase;
  evdns_base *dnsbase;
  SSL_CTX *ssl_ctx;
  SSL *ssl;
  bufferevent *bev;
  client_state state;
  std::vector<std::unique_ptr<Request>> reqvec;
  // Map from stream ID to Request object.
  std::map<int32_t, Request*> streams;
  // Insert path already added in reqvec to prevent multiple request
  // for 1 resource.
  std::set<std::string> path_cache;
  // The number of completed requests, including failed ones.
  size_t complete;
  std::string scheme;
  std::string hostport;
  SessionStat stat;
  // Used for parse the HTTP upgrade response from server
  std::unique_ptr<http_parser> htp;
  // true if the response message of HTTP Upgrade request is fully
  // received. It is not relevant the upgrade succeeds, or not.
  bool upgrade_response_complete;
  // The HTTP status code of the response message of HTTP Upgrade.
  unsigned int upgrade_response_status_code;
  // SETTINGS payload sent as token68 in HTTP Upgrade
  uint8_t settings_payload[16];
  // The length of settings_payload
  size_t settings_payloadlen;

  HttpClient(const nghttp2_session_callbacks* callbacks,
             event_base *evbase, SSL_CTX *ssl_ctx)
    : session(nullptr),
      callbacks(callbacks),
      evbase(evbase),
      dnsbase(evdns_base_new(evbase, 1)),
      ssl_ctx(ssl_ctx),
      ssl(nullptr),
      bev(nullptr),
      state(STATE_IDLE),
      complete(0),
      upgrade_response_complete(false),
      upgrade_response_status_code(0),
      settings_payloadlen(0)
  {}

  ~HttpClient()
  {
    disconnect();
  }

  bool need_upgrade() const
  {
    return config.upgrade && scheme == "http";
  }

  int initiate_connection(const std::string& host, uint16_t port)
  {
    int rv;
    if(ssl_ctx) {
      // We are establishing TLS connection.
      ssl = SSL_new(ssl_ctx);
      if(!ssl) {
        std::cerr << "SSL_new() failed: "
                  << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
        return -1;
      }

      // If the user overrode the host header, use that value for the
      // SNI extension
      const char *host_string = nullptr;
      auto i = config.headers.find( "Host" );
      if ( i != config.headers.end() ) {
        host_string = (*i).second.c_str();
      } else {
        host_string = host.c_str();
      }
      if (!SSL_set_tlsext_host_name(ssl, host_string)) {
        std::cerr << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
        return -1;
      }
      bev = bufferevent_openssl_socket_new(evbase, -1, ssl,
                                           BUFFEREVENT_SSL_CONNECTING,
                                           BEV_OPT_DEFER_CALLBACKS);
      rv = bufferevent_socket_connect_hostname
        (bev, dnsbase, AF_UNSPEC, host_string, port);
    } else {
      bev = bufferevent_socket_new(evbase, -1, BEV_OPT_DEFER_CALLBACKS);
      rv = bufferevent_socket_connect_hostname
        (bev, dnsbase, AF_UNSPEC, host.c_str(), port);
    }
    if(rv != 0) {
      return -1;
    }
    bufferevent_enable(bev, EV_READ);
    if(need_upgrade()) {
      htp = util::make_unique<http_parser>();
      http_parser_init(htp.get(), HTTP_RESPONSE);
      htp->data = this;
      bufferevent_setcb(bev, upgrade_readcb, nullptr, eventcb, this);
    } else {
      bufferevent_setcb(bev, readcb, writecb, eventcb, this);
    }
    if(config.timeout != -1) {
      timeval tv = { config.timeout, 0 };
      bufferevent_set_timeouts(bev, &tv, &tv);
    }
    return 0;
  }

  void disconnect()
  {
    state = STATE_IDLE;
    nghttp2_session_del(session);
    session = nullptr;
    if(ssl) {
      SSL_shutdown(ssl);
    }
    if(bev) {
      bufferevent_disable(bev, EV_READ | EV_WRITE);
      bufferevent_free(bev);
      bev = nullptr;
    }
    if(dnsbase) {
      evdns_base_free(dnsbase, 1);
      dnsbase = nullptr;
    }
    if(ssl) {
      SSL_free(ssl);
      ssl = nullptr;
    }
  }

  int on_upgrade_connect()
  {
    ssize_t rv;
    record_handshake_time();
    assert(!reqvec.empty());
    nghttp2_settings_entry iv[16];
    size_t niv = populate_settings(iv);
    assert(sizeof(settings_payload) >= 8*niv);
    rv = nghttp2_pack_settings_payload(settings_payload, iv, niv);
    if(rv < 0) {
      return -1;
    }
    settings_payloadlen = rv;
    auto token68 = base64::encode(&settings_payload[0],
                                  &settings_payload[settings_payloadlen]);
    util::to_token68(token68);
    std::string req;
    if(reqvec[0]->data_prd) {
      // If the request contains upload data, use OPTIONS * to upgrade
      req = "OPTIONS *";
    } else {
      req = "GET ";
      req += reqvec[0]->make_reqpath();
    }
    req += " HTTP/1.1\r\n"
      "Host: ";
    req += hostport;
    req += "\r\n"
      "Connection: Upgrade, HTTP2-Settings\r\n"
      "Upgrade: " NGHTTP2_PROTO_VERSION_ID "\r\n"
      "HTTP2-Settings: ";
    req += token68;
    req += "\r\n"
      "Accept: */*\r\n"
      "User-Agent: nghttp2/" NGHTTP2_VERSION "\r\n"
      "\r\n";
    bufferevent_write(bev, req.c_str(), req.size());
    if(config.verbose) {
      print_timer();
      std::cout << " HTTP Upgrade request\n"
                << req << std::endl;
    }
    return 0;
  }

  int on_upgrade_read()
  {
    int rv;
    auto input = bufferevent_get_input(bev);
    auto inputlen = evbuffer_get_length(input);
    if(inputlen == 0) {
      return 0;
    }
    auto mem = evbuffer_pullup(input, -1);
    auto nread = http_parser_execute(htp.get(), &htp_hooks,
                                     reinterpret_cast<const char*>(mem),
                                     inputlen);
    if(config.verbose) {
      std::cout.write(reinterpret_cast<const char*>(mem), nread);
    }
    evbuffer_drain(input, nread);

    auto htperr = HTTP_PARSER_ERRNO(htp.get());
    if(htperr == HPE_OK) {
      if(upgrade_response_complete) {
        if(config.verbose) {
          std::cout << std::endl;
        }
        if(upgrade_response_status_code == 101) {
          if(config.verbose) {
            print_timer();
            std::cout << " HTTP Upgrade success" << std::endl;
          }
          bufferevent_setcb(bev, readcb, writecb, eventcb, this);
          rv = on_connect();
          if(rv != 0) {
            return rv;
          }
          // Read remaining data in the buffer because it is not
          // notified callback anymore.
          rv = on_read();
          if(rv != 0) {
            return rv;
          }
        } else {
          std::cerr << "HTTP Upgrade failed" << std::endl;
          return -1;
        }
      }
    } else {
      std::cerr << "Failed to parse HTTP Upgrade response header: "
                << "(" << http_errno_name(htperr) << ") "
                << http_errno_description(htperr) << std::endl;
      return -1;
    }
    return 0;
  }

  int on_connect()
  {
    int rv;
    if(!need_upgrade()) {
      record_handshake_time();
    }
    rv = nghttp2_session_client_new(&session, callbacks, this);
    if(rv != 0) {
      return -1;
    }
    if(need_upgrade()) {
      // Adjust stream user-data depending on the existence of upload
      // data
      Request *stream_user_data = nullptr;
      if(!reqvec[0]->data_prd) {
        stream_user_data = reqvec[0].get();
      }
      rv = nghttp2_session_upgrade(session, settings_payload,
                                   settings_payloadlen, stream_user_data);
      if(rv != 0) {
        std::cerr << "nghttp2_session_upgrade() returned error: "
                  << nghttp2_strerror(rv) << std::endl;
        return -1;
      }
      if(stream_user_data) {
        check_stream_id(session, 1, this);
      }
    }
    // Send connection header here
    bufferevent_write(bev, NGHTTP2_CLIENT_CONNECTION_HEADER,
                      NGHTTP2_CLIENT_CONNECTION_HEADER_LEN);
    // If upgrade succeeds, the SETTINGS value sent with
    // HTTP2-Settings header field has already been submitted to
    // session object.
    if(!need_upgrade()) {
      nghttp2_settings_entry iv[16];
      auto niv = populate_settings(iv);
      rv = nghttp2_submit_settings(session, iv, niv);
      if(rv != 0) {
        return -1;
      }
    }
    // Adjust first request depending on the existence of the upload
    // data
    for(auto i = std::begin(reqvec)+(need_upgrade() && !reqvec[0]->data_prd);
        i != std::end(reqvec); ++i) {
      submit_request(this, config.headers, (*i).get());
    }
    return on_write();
  }

  int on_read()
  {
    int rv = 0;
    if((rv = nghttp2_session_recv(session)) < 0) {
      if(rv != NGHTTP2_ERR_EOF) {
        std::cerr << "nghttp2_session_recv() returned error: "
                  << nghttp2_strerror(rv) << std::endl;
      }
    } else if((rv = nghttp2_session_send(session)) < 0) {
      std::cerr << "nghttp2_session_send() returned error: "
                << nghttp2_strerror(rv) << std::endl;
    }
    if(rv == 0) {
      if(nghttp2_session_want_read(session) == 0 &&
         nghttp2_session_want_write(session) == 0 &&
         evbuffer_get_length(bufferevent_get_output(bev)) == 0) {
        rv = -1;
      }
    }
    return rv;
  }

  int on_write()
  {
    int rv = 0;
    if((rv = nghttp2_session_send(session)) < 0) {
      std::cerr << "nghttp2_session_send() returned error: "
                << nghttp2_strerror(rv) << std::endl;
    }
    if(rv == 0) {
      if(nghttp2_session_want_read(session) == 0 &&
         nghttp2_session_want_write(session) == 0 &&
         evbuffer_get_length(bufferevent_get_output(bev)) == 0) {
        rv = -1;
      }
    }
    return rv;
  }

  int sendcb(const uint8_t *data, size_t len)
  {
    int rv;
    auto output = bufferevent_get_output(bev);
    // Check buffer length and return WOULDBLOCK if it is large enough.
    if(evbuffer_get_length(output) > config.output_upper_thres) {
      return NGHTTP2_ERR_WOULDBLOCK;
    }

    rv = evbuffer_add(output, data, len);
    if(rv == -1) {
      std::cerr << "evbuffer_add() failed" << std::endl;
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    } else {
      return len;
    }
  }

  int recvcb(uint8_t *buf, size_t len)
  {
    auto input = bufferevent_get_input(bev);
    int nread = evbuffer_remove(input, buf, len);
    if(nread == -1) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    } else if(nread == 0) {
      return NGHTTP2_ERR_WOULDBLOCK;
    } else {
      return nread;
    }
  }

  bool all_requests_processed() const
  {
    return complete == reqvec.size();
  }
  void update_hostport()
  {
    if(reqvec.empty()) {
      return;
    }
    scheme = get_uri_field(reqvec[0]->uri.c_str(), reqvec[0]->u, UF_SCHEMA);
    std::stringstream ss;
    if(reqvec[0]->is_ipv6_literal_addr()) {
      ss << "[";
      write_uri_field(ss, reqvec[0]->uri.c_str(), reqvec[0]->u, UF_HOST);
      ss << "]";
    } else {
      write_uri_field(ss, reqvec[0]->uri.c_str(), reqvec[0]->u, UF_HOST);
    }
    if(has_uri_field(reqvec[0]->u, UF_PORT) &&
       reqvec[0]->u.port != get_default_port(reqvec[0]->uri.c_str(),
                                             reqvec[0]->u)) {
      ss << ":" << reqvec[0]->u.port;
    }
    hostport = ss.str();
  }
  bool add_request(const std::string& uri,
                   const nghttp2_data_provider *data_prd,
                   int64_t data_length,
                   int level = 0)
  {
    http_parser_url u;
    if(http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) != 0) {
      return false;
    }
    if(path_cache.count(uri)) {
      return false;
    } else {
      if(config.multiply == 1) {
        path_cache.insert(uri);
      }
      reqvec.push_back(util::make_unique<Request>(uri, u, data_prd,
                                                  data_length, level));
      return true;
    }
  }
  void record_handshake_time()
  {
    record_time(&stat.on_handshake_time);
  }
};

namespace {
int htp_msg_begincb(http_parser *htp)
{
  if(config.verbose) {
    print_timer();
    std::cout << " HTTP Upgrade response" << std::endl;
  }
  return 0;
}
} // namespace

namespace {
int htp_status_completecb(http_parser *htp)
{
  auto client = reinterpret_cast<HttpClient*>(htp->data);
  client->upgrade_response_status_code = htp->status_code;
  return 0;
}
} // namespace

namespace {
int htp_msg_completecb(http_parser *htp)
{
  auto client = reinterpret_cast<HttpClient*>(htp->data);
  client->upgrade_response_complete = true;
  return 0;
}
} // namespace

namespace {
http_parser_settings htp_hooks = {
  htp_msg_begincb, /*http_cb      on_message_begin;*/
  nullptr, /*http_data_cb on_url;*/
  htp_status_completecb, /*http_cb on_status_complete */
  nullptr, /*http_data_cb on_header_field;*/
  nullptr, /*http_data_cb on_header_value;*/
  nullptr, /*http_cb      on_headers_complete;*/
  nullptr, /*http_data_cb on_body;*/
  htp_msg_completecb /*http_cb      on_message_complete;*/
};
} // namespace

namespace {
void submit_request(HttpClient *client,
                    const std::map<std::string, std::string>& headers,
                    Request *req)
{
  enum eStaticHeaderPosition
  {
    POS_METHOD = 0,
    POS_PATH,
    POS_SCHEME,
    POS_HOST,
    POS_ACCEPT,
    POS_ACCEPT_ENCODING,
    POS_USERAGENT
  };
  auto path = req->make_reqpath();
  auto scheme = get_uri_field(req->uri.c_str(), req->u, UF_SCHEMA);
  const char *static_nv[] = {
    ":method", req->data_prd ? "POST" : "GET",
    ":path", path.c_str(),
    ":scheme", scheme.c_str(),
    ":host", client->hostport.c_str(),
    "accept", "*/*",
    "accept-encoding", "gzip, deflate",
    "user-agent", "nghttp2/" NGHTTP2_VERSION
  };

  int hardcoded_entry_count = sizeof(static_nv) / sizeof(*static_nv);
  int header_count          = headers.size();
  int total_entry_count     = hardcoded_entry_count + header_count * 2;
  if(req->data_prd) {
    total_entry_count += 2;
  }

  auto nv = util::make_unique<const char*[]>(total_entry_count + 1);

  memcpy(nv.get(), static_nv, hardcoded_entry_count * sizeof(*static_nv));

  auto i = std::begin(headers);
  auto end = std::end(headers);

  int pos = hardcoded_entry_count;

  std::string content_length_str;
  if(req->data_prd) {
    content_length_str = std::to_string(req->data_length);
    nv[pos++] = "content-length";
    nv[pos++] = content_length_str.c_str();
  }
  while( i != end ) {
    auto key = (*i).first.c_str();
    auto value = (*i).second.c_str();
    if ( util::strieq( key, "accept" ) ) {
      nv[POS_ACCEPT*2+1] = value;
    }
    else if ( util::strieq( key, "user-agent" ) ) {
      nv[POS_USERAGENT*2+1] = value;
    }
    else if ( util::strieq( key, "host" ) ) {
      nv[POS_HOST*2+1] = value;
    }
    else {
      nv[pos] = key;
      nv[pos+1] = value;
      pos += 2;
    }
    ++i;
  }
  nv[pos] = nullptr;

  int r = nghttp2_submit_request(client->session, config.pri,
                                 nv.get(), req->data_prd, req);
  assert(r == 0);
}
} // namespace

void update_html_parser(HttpClient *client, Request *req,
                        const uint8_t *data, size_t len, int fin)
{
  if(!req->html_parser) {
    return;
  }
  req->update_html_parser(data, len, fin);

  for(size_t i = 0; i < req->html_parser->get_links().size(); ++i) {
    const auto& raw_uri = req->html_parser->get_links()[i];
    auto uri = strip_fragment(raw_uri.c_str());
    http_parser_url u;
    if(http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) == 0 &&
       fieldeq(uri.c_str(), u, req->uri.c_str(), req->u, UF_SCHEMA) &&
       fieldeq(uri.c_str(), u, req->uri.c_str(), req->u, UF_HOST) &&
       porteq(uri.c_str(), u, req->uri.c_str(), req->u)) {
      // No POST data for assets
      if ( client->add_request(uri, nullptr, 0, req->level+1) ) {
        submit_request(client, config.headers,
                       client->reqvec.back().get());
      }
    }
  }
  req->html_parser->clear_links();
}

HttpClient* get_session(void *user_data)
{
  return reinterpret_cast<HttpClient*>(user_data);
}

int on_data_chunk_recv_callback
(nghttp2_session *session, uint8_t flags, int32_t stream_id,
 const uint8_t *data, size_t len, void *user_data)
{
  auto client = get_session(user_data);
  auto itr = client->streams.find(stream_id);
  if(itr != client->streams.end()) {
    auto req = (*itr).second;
    if(req->inflater) {
      while(len > 0) {
        const size_t MAX_OUTLEN = 4096;
        uint8_t out[MAX_OUTLEN];
        size_t outlen = MAX_OUTLEN;
        size_t tlen = len;
        int rv = nghttp2_gzip_inflate(req->inflater, out, &outlen, data, &tlen);
        if(rv != 0) {
          nghttp2_submit_rst_stream(session, stream_id, NGHTTP2_INTERNAL_ERROR);
          break;
        }
        if(!config.null_out) {
          std::cout.write(reinterpret_cast<const char*>(out), outlen);
        }
        update_html_parser(client, req, out, outlen, 0);
        data += tlen;
        len -= tlen;
      }
    } else {
      if(!config.null_out) {
        std::cout.write(reinterpret_cast<const char*>(data), len);
      }
      update_html_parser(client, req, data, len, 0);
    }
  }
  return 0;
}

namespace {
void check_stream_id(nghttp2_session *session, int32_t stream_id,
                     void *user_data)
{
  auto client = get_session(user_data);
  auto req = (Request*)nghttp2_session_get_stream_user_data(session,
                                                            stream_id);
  assert(req);
  client->streams[stream_id] = req;
  req->record_syn_stream_time();
}
} // namespace

int before_frame_send_callback
(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
  if(frame->hd.type == NGHTTP2_HEADERS &&
     frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
    check_stream_id(session, frame->hd.stream_id, user_data);
  }
  return 0;
}

int on_frame_send_callback2
(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
  if(config.verbose) {
    on_frame_send_callback(session, frame, user_data);
  }
  return 0;
}

void check_response_header
(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
  if(frame->hd.type != NGHTTP2_HEADERS ||
     frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return;
  }
  auto req = (Request*)nghttp2_session_get_stream_user_data
    (session, frame->hd.stream_id);
  if(!req) {
    // Server-pushed stream does not have stream user data
    return;
  }
  bool gzip = false;
  for(size_t i = 0; i < frame->headers.nvlen; ++i) {
    auto nv = &frame->headers.nva[i];
    if(util::strieq("content-encoding", nv->name, nv->namelen)) {
      gzip = util::strieq("gzip", nv->value, nv->valuelen) ||
        util::strieq("deflate", nv->value, nv->valuelen);
    } else if(util::strieq(":status", nv->name, nv->namelen)) {
      req->status.assign(nv->value, nv->value + nv->valuelen);
    }
  }
  if(gzip) {
    if(!req->inflater) {
      req->init_inflater();
    }
  }
  if(config.get_assets && req->level == 0) {
    if(!req->html_parser) {
      req->init_html_parser();
    }
  }
}

int on_frame_recv_callback2
(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
  if(frame->hd.type == NGHTTP2_HEADERS &&
     frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
    auto req = (Request*)nghttp2_session_get_stream_user_data
      (session, frame->hd.stream_id);
    // If this is the HTTP Upgrade with OPTIONS method to avoid POST,
    // req is nullptr.
    if(req) {
      req->record_syn_reply_time();
    }
  }
  check_response_header(session, frame, user_data);
  if(config.verbose) {
    on_frame_recv_callback(session, frame, user_data);
  }
  return 0;
}

int on_stream_close_callback
(nghttp2_session *session, int32_t stream_id, nghttp2_error_code error_code,
 void *user_data)
{
  auto client = get_session(user_data);
  auto itr = client->streams.find(stream_id);
  if(itr != client->streams.end()) {
    update_html_parser(client, (*itr).second, nullptr, 0, 1);
    (*itr).second->record_complete_time();
    ++client->complete;
    if(client->all_requests_processed()) {
      nghttp2_submit_goaway(session, NGHTTP2_NO_ERROR, nullptr, 0);
    }
  }
  return 0;
}

void print_stats(const HttpClient& client)
{
  std::cout << "***** Statistics *****" << std::endl;
  int i = 0;
  for(auto& req : client.reqvec) {
    std::cout << "#" << ++i << ": " << req->uri << std::endl;
    std::cout << "    Status: " << req->status << std::endl;
    std::cout << "    Delta (ms) from handshake(HEADERS):"
              << std::endl;
    if(req->stat.on_syn_reply_time.tv_sec >= 0) {
      std::cout << "        SYN_REPLY: "
                << time_delta(req->stat.on_syn_reply_time,
                              client.stat.on_handshake_time)
                << "("
                << time_delta(req->stat.on_syn_reply_time,
                              req->stat.on_syn_stream_time)
                << ")"
                << std::endl;
    }
    if(req->stat.on_complete_time.tv_sec >= 0) {
      std::cout << "        Completed: "
                << time_delta(req->stat.on_complete_time,
                              client.stat.on_handshake_time)
                << "("
                << time_delta(req->stat.on_complete_time,
                              req->stat.on_syn_stream_time)
                << ")"
                << std::endl;
    }
    std::cout << std::endl;
  }
}

namespace {
int client_select_next_proto_cb(SSL* ssl,
                                unsigned char **out, unsigned char *outlen,
                                const unsigned char *in, unsigned int inlen,
                                void *arg)
{
  if(config.verbose) {
    print_timer();
    std::cout << " NPN select next protocol: the remote server offers:"
              << std::endl;
  }
  for(unsigned int i = 0; i < inlen; i += in[i]+1) {
    if(config.verbose) {
      std::cout << "          * ";
      std::cout.write(reinterpret_cast<const char*>(&in[i+1]), in[i]);
      std::cout << std::endl;
    }
  }
  if(nghttp2_select_next_protocol(out, outlen, in, inlen) <= 0) {
    std::cerr << "Server did not advertise HTTP/2.0 protocol."
              << std::endl;
  } else {
    if(config.verbose) {
      std::cout << "          NPN selected the protocol: ";
      std::cout.write(reinterpret_cast<const char*>(*out), (size_t)*outlen);
      std::cout << std::endl;
    }
  }
  return SSL_TLSEXT_ERR_OK;
}
} // namespace

namespace {
void upgrade_readcb(bufferevent *bev, void *ptr)
{
  int rv;
  auto client = reinterpret_cast<HttpClient*>(ptr);
  rv = client->on_upgrade_read();
  if(rv != 0) {
    client->disconnect();
  }
}
} // namespace

namespace {
void readcb(bufferevent *bev, void *ptr)
{
  int rv;
  auto client = reinterpret_cast<HttpClient*>(ptr);
  rv = client->on_read();
  if(rv != 0) {
    client->disconnect();
  }
}
} // namespace

namespace {
void writecb(bufferevent *bev, void *ptr)
{
  if(evbuffer_get_length(bufferevent_get_output(bev)) > 0) {
    return;
  }
  int rv;
  auto client = reinterpret_cast<HttpClient*>(ptr);
  rv = client->on_write();
  if(rv != 0) {
    client->disconnect();
  }
}
} // namespace

namespace {
void eventcb(bufferevent *bev, short events, void *ptr)
{
  int rv;
  auto client = reinterpret_cast<HttpClient*>(ptr);
  if(events & BEV_EVENT_CONNECTED) {
    client->state = STATE_CONNECTED;
    int fd = bufferevent_getfd(bev);
    int val = 1;
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                  reinterpret_cast<char *>(&val), sizeof(val)) == -1) {
      std::cerr << "Setting option TCP_NODELAY failed: errno="
                << errno << std::endl;
    }
    if(client->need_upgrade()) {
      rv = client->on_upgrade_connect();
    } else {
      // TODO Check NPN result and fail fast?
      rv = client->on_connect();
    }
    if(rv != 0) {
      client->disconnect();
      return;
    }
  } else if(events & BEV_EVENT_EOF) {
    std::cerr << "EOF" << std::endl;
    client->disconnect();
    return;
  } else if(events & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
    if(events & BEV_EVENT_ERROR) {
      if(client->state == STATE_IDLE) {
        std::cerr << "Could not connect to the host" << std::endl;
      } else {
        std::cerr << "Network error" << std::endl;
      }
    } else {
      std::cerr << "Timeout" << std::endl;
    }
    // TODO Needs disconnect()?
    client->disconnect();
    return;
  }
}
} // namespace


namespace {
ssize_t client_send_callback(nghttp2_session *session,
                             const uint8_t *data, size_t len, int flags,
                             void *user_data)
{
  auto client = reinterpret_cast<HttpClient*>(user_data);
  return client->sendcb(data, len);
}
} // namespace

namespace {
ssize_t client_recv_callback(nghttp2_session *session,
                             uint8_t *buf, size_t len, int flags,
                             void *user_data)
{
  auto client = reinterpret_cast<HttpClient*>(user_data);
  return client->recvcb(buf, len);
}
} // namespace

int communicate(const std::string& scheme, const std::string& host,
                uint16_t port,
                std::vector<std::tuple<std::string,
                                       nghttp2_data_provider*,
                                       int64_t>> requests,
                const nghttp2_session_callbacks *callbacks)
{
  int result = 0;
  auto evbase = event_base_new();
  SSL_CTX *ssl_ctx = nullptr;
  if(scheme == "https") {
    ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if(!ssl_ctx) {
      std::cerr << "Failed to create SSL_CTX: "
                << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
      result = -1;
      goto fin;
    }
    SSL_CTX_set_options(ssl_ctx,
                        SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_COMPRESSION |
                        SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
    if(!config.keyfile.empty()) {
      if(SSL_CTX_use_PrivateKey_file(ssl_ctx, config.keyfile.c_str(),
                                     SSL_FILETYPE_PEM) != 1) {
        std::cerr << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
        result = -1;
        goto fin;
      }
    }
    if(!config.certfile.empty()) {
      if(SSL_CTX_use_certificate_chain_file(ssl_ctx,
                                            config.certfile.c_str()) != 1) {
        std::cerr << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
        result = -1;
        goto fin;
      }
    }
    SSL_CTX_set_next_proto_select_cb(ssl_ctx,
                                     client_select_next_proto_cb, nullptr);
  }
  {
    HttpClient client{callbacks, evbase, ssl_ctx};
    for(auto req : requests) {
      for(int i = 0; i < config.multiply; ++i) {
        client.add_request(std::get<0>(req), std::get<1>(req),
                            std::get<2>(req));
      }
    }
    client.update_hostport();
    if(client.initiate_connection(host, port) != 0) {
      goto fin;
    }
    event_base_loop(evbase, 0);

    if(!client.all_requests_processed()) {
      std::cerr << "Some requests were not processed. total="
                << client.reqvec.size()
                << ", processed=" << client.complete << std::endl;
    }
    if(config.stat) {
      print_stats(client);
    }
  }
 fin:
  if(ssl_ctx) {
    SSL_CTX_free(ssl_ctx);
  }
  if(evbase) {
    event_base_free(evbase);
  }
  return result;
}

ssize_t file_read_callback
(nghttp2_session *session, int32_t stream_id,
 uint8_t *buf, size_t length, int *eof,
 nghttp2_data_source *source, void *user_data)
{
  auto req = (Request*)nghttp2_session_get_stream_user_data
    (session, stream_id);
  assert(req);
  int fd = source->fd;
  ssize_t r;
  while((r = pread(fd, buf, length, req->data_offset)) == -1 &&
        errno == EINTR);
  if(r == -1) {
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  } else {
    if(r == 0) {
      *eof = 1;
    } else {
      req->data_offset += r;
    }
    return r;
  }
}

int run(char **uris, int n)
{
  nghttp2_session_callbacks callbacks;
  memset(&callbacks, 0, sizeof(nghttp2_session_callbacks));
  callbacks.send_callback = client_send_callback;
  callbacks.recv_callback = client_recv_callback;
  callbacks.on_stream_close_callback = on_stream_close_callback;
  callbacks.on_frame_recv_callback = on_frame_recv_callback2;
  callbacks.on_frame_send_callback = on_frame_send_callback2;
  callbacks.before_frame_send_callback = before_frame_send_callback;
  if(config.verbose) {
    callbacks.on_data_recv_callback = on_data_recv_callback;
    callbacks.on_data_send_callback = on_data_send_callback;
    callbacks.on_invalid_frame_recv_callback = on_invalid_frame_recv_callback;
    callbacks.on_frame_recv_parse_error_callback =
      on_frame_recv_parse_error_callback;
    callbacks.on_unknown_frame_recv_callback = on_unknown_frame_recv_callback;
  }
  callbacks.on_data_chunk_recv_callback = on_data_chunk_recv_callback;
  std::string prev_scheme;
  std::string prev_host;
  uint16_t prev_port = 0;
  int failures = 0;
  int data_fd = -1;
  nghttp2_data_provider data_prd;
  struct stat data_stat;

  if(!config.datafile.empty()) {
    data_fd = open(config.datafile.c_str(), O_RDONLY | O_BINARY);
    if(data_fd == -1) {
      std::cerr << "Could not open file " << config.datafile << std::endl;
      return 1;
    }
    if(fstat(data_fd, &data_stat) == -1) {
      close(data_fd);
      std::cerr << "Could not stat file " << config.datafile << std::endl;
      return 1;
    }
    data_prd.source.fd = data_fd;
    data_prd.read_callback = file_read_callback;
  }
  std::vector<std::tuple<std::string, nghttp2_data_provider*, int64_t>>
    requests;
  for(int i = 0; i < n; ++i) {
    http_parser_url u;
    auto uri = strip_fragment(uris[i]);
    if(http_parser_parse_url(uri.c_str(), uri.size(), 0, &u) == 0 &&
       has_uri_field(u, UF_SCHEMA)) {
      uint16_t port = has_uri_field(u, UF_PORT) ?
        u.port : get_default_port(uri.c_str(), u);
      if(!fieldeq(uri.c_str(), u, UF_SCHEMA, prev_scheme.c_str()) ||
         !fieldeq(uri.c_str(), u, UF_HOST, prev_host.c_str()) ||
         u.port != prev_port) {
        if(!requests.empty()) {
          if (communicate(prev_scheme, prev_host, prev_port,
                          std::move(requests), &callbacks) != 0) {
            ++failures;
          }
          requests.clear();
        }
        prev_scheme = get_uri_field(uri.c_str(), u, UF_SCHEMA);
        prev_host = get_uri_field(uri.c_str(), u, UF_HOST);
        prev_port = port;
      }
      requests.emplace_back(uri, data_fd == -1 ? nullptr : &data_prd,
                            data_stat.st_size);
    }
  }
  if(!requests.empty()) {
    if (communicate(prev_scheme, prev_host, prev_port, std::move(requests),
                    &callbacks) != 0) {
      ++failures;
    }
  }
  return failures;
}

void print_usage(std::ostream& out)
{
  out << "Usage: nghttp [-Oafnsuv] [-t <SECONDS>] [-w <WINDOW_BITS>] [--cert=<CERT>]\n"
      << "              [--key=<KEY>] [-d <FILE>] [-m <N>] [-p <PRIORITY>]\n"
      << "              <URI>..."
      << std::endl;
}

void print_help(std::ostream& out)
{
  print_usage(out);
  out << "\n"
      << "OPTIONS:\n"
      << "    -v, --verbose      Print debug information such as reception/\n"
      << "                       transmission of frames and name/value pairs.\n"
      << "    -n, --null-out     Discard downloaded data.\n"
      << "    -O, --remote-name  Save download data in the current directory.\n"
      << "                       The filename is dereived from URI. If URI\n"
      << "                       ends with '/', 'index.html' is used as a\n"
      << "                       filename. Not implemented yet.\n"
      << "    -t, --timeout=<N>  Timeout each request after <N> seconds.\n"
      << "    -w, --window-bits=<N>\n"
      << "                       Sets the initial window size to 2**<N>-1.\n"
      << "    -a, --get-assets   Download assets such as stylesheets, images\n"
      << "                       and script files linked from the downloaded\n"
      << "                       resource. Only links whose origins are the\n"
      << "                       same with the linking resource will be\n"
      << "                       downloaded.\n"
      << "    -s, --stat         Print statistics.\n"
      << "    -H, --header       Add a header to the requests.\n"
      << "    --cert=<CERT>      Use the specified client certificate file.\n"
      << "                       The file must be in PEM format.\n"
      << "    --key=<KEY>        Use the client private key file. The file\n"
      << "                       must be in PEM format.\n"
      << "    -d, --data=<FILE>  Post FILE to server. If - is given, data\n"
      << "                       will be read from stdin.\n"
      << "    -m, --multiply=<N> Request each URI <N> times. By default, same\n"
      << "                       URI is not requested twice. This option\n"
      << "                       disables it too.\n"
      << "    -f, --no-low-control\n"
      << "                       Disables connection and stream level flow\n"
      << "                       controls.\n"
      << "    -u, --upgrade      Perform HTTP Upgrade for HTTP/2.0. This\n"
      << "                       option is ignored if the request URI has\n"
      << "                       https scheme.\n"
      << "                       If -d is used, the HTTP upgrade request is\n"
      << "                       performed with OPTIONS method.\n"
      << "    -p, --pri=<PRIORITY>\n"
      << "                       Sets stream priority. Default: "
      << NGHTTP2_PRI_DEFAULT << "\n"
      << std::endl;
}

int main(int argc, char **argv)
{
  while(1) {
    int flag = 0;
    static option long_options[] = {
      {"verbose", no_argument, nullptr, 'v'},
      {"null-out", no_argument, nullptr, 'n'},
      {"remote-name", no_argument, nullptr, 'O'},
      {"timeout", required_argument, nullptr, 't'},
      {"window-bits", required_argument, nullptr, 'w'},
      {"get-assets", no_argument, nullptr, 'a'},
      {"stat", no_argument, nullptr, 's'},
      {"cert", required_argument, &flag, 1},
      {"key", required_argument, &flag, 2},
      {"help", no_argument, nullptr, 'h'},
      {"header", required_argument, nullptr, 'H'},
      {"data", required_argument, nullptr, 'd'},
      {"multiply", required_argument, nullptr, 'm'},
      {"no-flow-control", no_argument, nullptr, 'f'},
      {"upgrade", no_argument, nullptr, 'u'},
      {"pri", required_argument, nullptr, 'p'},
      {nullptr, 0, nullptr, 0 }
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "Oad:fm:np:hH:vst:uw:", long_options,
                        &option_index);
    if(c == -1) {
      break;
    }
    switch(c) {
    case 'O':
      config.remote_name = true;
      break;
    case 'f':
      config.no_flow_control = true;
      break;
    case 'h':
      print_help(std::cout);
      exit(EXIT_SUCCESS);
    case 'n':
      config.null_out = true;
      break;
    case 'p': {
      auto n = strtoul(optarg, nullptr, 10);
      if(n <= NGHTTP2_PRI_LOWEST) {
        config.pri = n;
      } else {
        std::cerr << "-p: specify the integer in the range [0, "
                  << NGHTTP2_PRI_LOWEST << "], inclusive"
                  << std::endl;
        exit(EXIT_FAILURE);
      }
      break;
    }
    case 'v':
      config.verbose = true;
      break;
    case 't':
      config.timeout = atoi(optarg) * 1000;
      break;
    case 'u':
      config.upgrade = true;
      break;
    case 'w': {
      errno = 0;
      unsigned long int n = strtoul(optarg, nullptr, 10);
      if(errno == 0 && n < 31) {
        config.window_bits = n;
      } else {
        std::cerr << "-w: specify the integer in the range [0, 30], inclusive"
                  << std::endl;
        exit(EXIT_FAILURE);
      }
      break;
    }
    case 'H': {
      char *header = optarg;
      char *value = strchr( optarg, ':' );
      if ( ! value || header == value) {
        std::cerr << "-H: invalid header: " << optarg
                  << std::endl;
        exit(EXIT_FAILURE);
      }
      *value = 0;
      value++;
      while( isspace( *value ) ) { value++; }
      if ( *value == 0 ) {
        // This could also be a valid case for suppressing a header
        // similar to curl
        std::cerr << "-H: invalid header - value missing: " << optarg
                  << std::endl;
        exit(EXIT_FAILURE);
      }
      // Note that there is no processing currently to handle multiple
      // message-header fields with the same field name
      config.headers.insert(std::pair<std::string,std::string>(header, value));
      break;
    }
    case 'a':
#ifdef HAVE_LIBXML2
      config.get_assets = true;
#else // !HAVE_LIBXML2
      std::cerr << "Warning: -a, --get-assets option cannot be used because\n"
                << "the binary was not compiled with libxml2."
                << std::endl;
#endif // !HAVE_LIBXML2
      break;
    case 's':
      config.stat = true;
      break;
    case 'd':
      config.datafile = strcmp("-", optarg) == 0 ? "/dev/stdin" : optarg;
      break;
    case 'm':
      config.multiply = strtoul(optarg, nullptr, 10);
      break;
    case '?':
      exit(EXIT_FAILURE);
    case 0:
      switch(flag) {
      case 1:
        // cert option
        config.certfile = optarg;
        break;
      case 2:
        // key option
        config.keyfile = optarg;
        break;
      }
      break;
    default:
      break;
    }
  }

  set_color_output(isatty(fileno(stdout)));

  struct sigaction act;
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, nullptr);
  SSL_load_error_strings();
  SSL_library_init();
  reset_timer();
  return run(argv+optind, argc-optind);
}

} // namespace nghttp2

int main(int argc, char **argv)
{
  return nghttp2::main(argc, argv);
}
