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
#include "shrpx_downstream_test.h"

#include <iostream>

#include <CUnit/CUnit.h>

#include "shrpx_downstream.h"

namespace shrpx {

void test_downstream_normalize_request_headers(void)
{
  Downstream d(nullptr, 0, 0);
  d.add_request_header("Charlie", "0");
  d.add_request_header("Alpha", "1");
  d.add_request_header("Delta", "2");
  d.add_request_header("BravO", "3");
  d.normalize_request_headers();

  auto ans = Headers{
    {"alpha", "1"},
    {"bravo", "3"},
    {"charlie", "0"},
    {"delta", "2"}
  };
  CU_ASSERT(ans == d.get_request_headers());
}

void test_downstream_normalize_response_headers(void)
{
  Downstream d(nullptr, 0, 0);
  d.add_response_header("Charlie", "0");
  d.add_response_header("Alpha", "1");
  d.add_response_header("Delta", "2");
  d.add_response_header("BravO", "3");
  d.normalize_response_headers();

  auto ans = Headers{
    {"alpha", "1"},
    {"bravo", "3"},
    {"charlie", "0"},
    {"delta", "2"}
  };
  CU_ASSERT(ans == d.get_response_headers());
}

void test_downstream_get_norm_request_header(void)
{
  Downstream d(nullptr, 0, 0);
  d.add_request_header("alpha", "0");
  d.add_request_header("bravo", "1");
  d.add_request_header("bravo", "2");
  d.add_request_header("charlie", "3");
  d.add_request_header("delta", "4");
  d.add_request_header("echo", "5");
  auto i = d.get_norm_request_header("alpha");
  CU_ASSERT(std::make_pair(std::string("alpha"), std::string("0")) == *i);
  i = d.get_norm_request_header("bravo");
  CU_ASSERT(std::make_pair(std::string("bravo"), std::string("1")) == *i);
  i = d.get_norm_request_header("delta");
  CU_ASSERT(std::make_pair(std::string("delta"), std::string("4")) == *i);
  i = d.get_norm_request_header("echo");
  CU_ASSERT(std::make_pair(std::string("echo"), std::string("5")) == *i);
  i = d.get_norm_request_header("foxtrot");
  CU_ASSERT(i == std::end(d.get_request_headers()));
}

void test_downstream_get_norm_response_header(void)
{
  Downstream d(nullptr, 0, 0);
  d.add_response_header("alpha", "0");
  d.add_response_header("bravo", "1");
  d.add_response_header("bravo", "2");
  d.add_response_header("charlie", "3");
  d.add_response_header("delta", "4");
  d.add_response_header("echo", "5");
  auto i = d.get_norm_response_header("alpha");
  CU_ASSERT(std::make_pair(std::string("alpha"), std::string("0")) == *i);
  i = d.get_norm_response_header("bravo");
  CU_ASSERT(std::make_pair(std::string("bravo"), std::string("1")) == *i);
  i = d.get_norm_response_header("delta");
  CU_ASSERT(std::make_pair(std::string("delta"), std::string("4")) == *i);
  i = d.get_norm_response_header("echo");
  CU_ASSERT(std::make_pair(std::string("echo"), std::string("5")) == *i);
  i = d.get_norm_response_header("foxtrot");
  CU_ASSERT(i == std::end(d.get_response_headers()));
}

} // namespace shrpx
