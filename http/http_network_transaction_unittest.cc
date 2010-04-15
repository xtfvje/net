// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <math.h>  // ceil
#include <vector>

#include "base/compiler_specific.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/scoped_ptr.h"
#include "net/base/completion_callback.h"
#include "net/base/mock_host_resolver.h"
#include "net/base/request_priority.h"
#include "net/base/ssl_config_service_defaults.h"
#include "net/base/ssl_info.h"
#include "net/base/test_completion_callback.h"
#include "net/base/upload_data.h"
#include "net/spdy/spdy_session_pool.h"
#include "net/http/http_auth_handler_ntlm.h"
#include "net/http/http_basic_stream.h"
#include "net/http/http_network_session.h"
#include "net/http/http_network_transaction.h"
#include "net/http/http_stream.h"
#include "net/http/http_transaction_unittest.h"
#include "net/proxy/proxy_config_service_fixed.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/ssl_client_socket.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"

//-----------------------------------------------------------------------------

namespace net {

// Helper to manage the lifetimes of the dependencies for a
// HttpNetworkTransaction.
class SessionDependencies {
 public:
  // Default set of dependencies -- "null" proxy service.
  SessionDependencies()
      : host_resolver(new MockHostResolver),
        proxy_service(ProxyService::CreateNull()),
        ssl_config_service(new SSLConfigServiceDefaults),
        http_auth_handler_factory(HttpAuthHandlerFactory::CreateDefault()) {}

  // Custom proxy service dependency.
  explicit SessionDependencies(ProxyService* proxy_service)
      : host_resolver(new MockHostResolver),
        proxy_service(proxy_service),
        ssl_config_service(new SSLConfigServiceDefaults),
        http_auth_handler_factory(HttpAuthHandlerFactory::CreateDefault()) {}

  scoped_refptr<MockHostResolverBase> host_resolver;
  scoped_refptr<ProxyService> proxy_service;
  scoped_refptr<SSLConfigService> ssl_config_service;
  MockClientSocketFactory socket_factory;
  scoped_ptr<HttpAuthHandlerFactory> http_auth_handler_factory;
};

ProxyService* CreateFixedProxyService(const std::string& proxy) {
  net::ProxyConfig proxy_config;
  proxy_config.proxy_rules().ParseFromString(proxy);
  return ProxyService::CreateFixed(proxy_config);
}

HttpNetworkSession* CreateSession(SessionDependencies* session_deps) {
  return new HttpNetworkSession(NULL,
                                session_deps->host_resolver,
                                session_deps->proxy_service,
                                &session_deps->socket_factory,
                                session_deps->ssl_config_service,
                                session_deps->http_auth_handler_factory.get());
}

class HttpNetworkTransactionTest : public PlatformTest {
 public:
  virtual void TearDown() {
    // Empty the current queue.
    MessageLoop::current()->RunAllPending();
    PlatformTest::TearDown();
  }

 protected:
  void KeepAliveConnectionResendRequestTest(const MockRead& read_failure);

  struct SimpleGetHelperResult {
    int rv;
    std::string status_line;
    std::string response_data;
  };

  SimpleGetHelperResult SimpleGetHelper(MockRead data_reads[],
                                        size_t reads_count) {
    SimpleGetHelperResult out;

    SessionDependencies session_deps;
    scoped_ptr<HttpTransaction> trans(
        new HttpNetworkTransaction(CreateSession(&session_deps)));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/");
    request.load_flags = 0;

    StaticSocketDataProvider data(data_reads, reads_count, NULL, 0);
    session_deps.socket_factory.AddSocketDataProvider(&data);

    TestCompletionCallback callback;

    int rv = trans->Start(&request, &callback, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    out.rv = callback.WaitForResult();
    if (out.rv != OK)
      return out;

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    out.status_line = response->headers->GetStatusLine();

    rv = ReadTransaction(trans.get(), &out.response_data);
    EXPECT_EQ(OK, rv);

    return out;
  }

  void ConnectStatusHelperWithExpectedStatus(const MockRead& status,
                                             int expected_status);

  void ConnectStatusHelper(const MockRead& status);
};

// Fill |str| with a long header list that consumes >= |size| bytes.
void FillLargeHeadersString(std::string* str, int size) {
  const char* row =
      "SomeHeaderName: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n";
  const int sizeof_row = strlen(row);
  const int num_rows = static_cast<int>(
      ceil(static_cast<float>(size) / sizeof_row));
  const int sizeof_data = num_rows * sizeof_row;
  DCHECK(sizeof_data >= size);
  str->reserve(sizeof_data);

  for (int i = 0; i < num_rows; ++i)
    str->append(row, sizeof_row);
}

// Alternative functions that eliminate randomness and dependency on the local
// host name so that the generated NTLM messages are reproducible.
void MockGenerateRandom1(uint8* output, size_t n) {
  static const uint8 bytes[] = {
    0x55, 0x29, 0x66, 0x26, 0x6b, 0x9c, 0x73, 0x54
  };
  static size_t current_byte = 0;
  for (size_t i = 0; i < n; ++i) {
    output[i] = bytes[current_byte++];
    current_byte %= arraysize(bytes);
  }
}

void MockGenerateRandom2(uint8* output, size_t n) {
  static const uint8 bytes[] = {
    0x96, 0x79, 0x85, 0xe7, 0x49, 0x93, 0x70, 0xa1,
    0x4e, 0xe7, 0x87, 0x45, 0x31, 0x5b, 0xd3, 0x1f
  };
  static size_t current_byte = 0;
  for (size_t i = 0; i < n; ++i) {
    output[i] = bytes[current_byte++];
    current_byte %= arraysize(bytes);
  }
}

std::string MockGetHostName() {
  return "WTC-WIN7";
}

template<typename EmulatedClientSocketPool, typename SocketSourceType>
class CaptureGroupNameSocketPool : public EmulatedClientSocketPool {
 public:
  CaptureGroupNameSocketPool(HttpNetworkSession* session,
                             SocketSourceType* socket_source)
      : EmulatedClientSocketPool(0, 0, "CaptureGroupNameTestPool",
                                 session->host_resolver(), socket_source) {}
  const std::string last_group_name_received() const {
    return last_group_name_;
  }

  virtual int RequestSocket(const std::string& group_name,
                            const void* socket_params,
                            RequestPriority priority,
                            ClientSocketHandle* handle,
                            CompletionCallback* callback,
                            const BoundNetLog& net_log) {
    last_group_name_ = group_name;
    return ERR_IO_PENDING;
  }
  virtual void CancelRequest(const std::string& group_name,
                             const ClientSocketHandle* handle) { }
  virtual void ReleaseSocket(const std::string& group_name,
                             ClientSocket* socket) {}
  virtual void CloseIdleSockets() {}
  virtual HostResolver* GetHostResolver() const {
    return NULL;
  }
  virtual int IdleSocketCount() const {
    return 0;
  }
  virtual int IdleSocketCountInGroup(const std::string& group_name) const {
    return 0;
  }
  virtual LoadState GetLoadState(const std::string& group_name,
                                 const ClientSocketHandle* handle) const {
    return LOAD_STATE_IDLE;
  }
  virtual base::TimeDelta ConnectionTimeout() const {
    return base::TimeDelta();
  }

 private:
  std::string last_group_name_;
};

typedef CaptureGroupNameSocketPool<TCPClientSocketPool, ClientSocketFactory>
    CaptureGroupNameTCPSocketPool;
typedef CaptureGroupNameSocketPool<SOCKSClientSocketPool, TCPClientSocketPool>
    CaptureGroupNameSOCKSSocketPool;
//-----------------------------------------------------------------------------

TEST_F(HttpNetworkTransactionTest, Basic) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));
}

TEST_F(HttpNetworkTransactionTest, SimpleGET) {
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.0 200 OK", out.status_line);
  EXPECT_EQ("hello world", out.response_data);
}

// Response with no status line.
TEST_F(HttpNetworkTransactionTest, SimpleGETNoHeaders) {
  MockRead data_reads[] = {
    MockRead("hello world"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/0.9 200 OK", out.status_line);
  EXPECT_EQ("hello world", out.response_data);
}

// Allow up to 4 bytes of junk to precede status line.
TEST_F(HttpNetworkTransactionTest, StatusLineJunk2Bytes) {
  MockRead data_reads[] = {
    MockRead("xxxHTTP/1.0 404 Not Found\nServer: blah\n\nDATA"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.0 404 Not Found", out.status_line);
  EXPECT_EQ("DATA", out.response_data);
}

// Allow up to 4 bytes of junk to precede status line.
TEST_F(HttpNetworkTransactionTest, StatusLineJunk4Bytes) {
  MockRead data_reads[] = {
    MockRead("\n\nQJHTTP/1.0 404 Not Found\nServer: blah\n\nDATA"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.0 404 Not Found", out.status_line);
  EXPECT_EQ("DATA", out.response_data);
}

// Beyond 4 bytes of slop and it should fail to find a status line.
TEST_F(HttpNetworkTransactionTest, StatusLineJunk5Bytes) {
  MockRead data_reads[] = {
    MockRead("xxxxxHTTP/1.1 404 Not Found\nServer: blah"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/0.9 200 OK", out.status_line);
  EXPECT_EQ("xxxxxHTTP/1.1 404 Not Found\nServer: blah", out.response_data);
}

// Same as StatusLineJunk4Bytes, except the read chunks are smaller.
TEST_F(HttpNetworkTransactionTest, StatusLineJunk4Bytes_Slow) {
  MockRead data_reads[] = {
    MockRead("\n"),
    MockRead("\n"),
    MockRead("Q"),
    MockRead("J"),
    MockRead("HTTP/1.0 404 Not Found\nServer: blah\n\nDATA"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.0 404 Not Found", out.status_line);
  EXPECT_EQ("DATA", out.response_data);
}

// Close the connection before enough bytes to have a status line.
TEST_F(HttpNetworkTransactionTest, StatusLinePartial) {
  MockRead data_reads[] = {
    MockRead("HTT"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/0.9 200 OK", out.status_line);
  EXPECT_EQ("HTT", out.response_data);
}

// Simulate a 204 response, lacking a Content-Length header, sent over a
// persistent connection.  The response should still terminate since a 204
// cannot have a response body.
TEST_F(HttpNetworkTransactionTest, StopsReading204) {
  MockRead data_reads[] = {
    MockRead("HTTP/1.1 204 No Content\r\n\r\n"),
    MockRead("junk"),  // Should not be read!!
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.1 204 No Content", out.status_line);
  EXPECT_EQ("", out.response_data);
}

// A simple request using chunked encoding with some extra data after.
// (Like might be seen in a pipelined response.)
TEST_F(HttpNetworkTransactionTest, ChunkedEncoding) {
  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"),
    MockRead("5\r\nHello\r\n"),
    MockRead("1\r\n"),
    MockRead(" \r\n"),
    MockRead("5\r\nworld\r\n"),
    MockRead("0\r\n\r\nHTTP/1.1 200 OK\r\n"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(OK, out.rv);
  EXPECT_EQ("HTTP/1.1 200 OK", out.status_line);
  EXPECT_EQ("Hello world", out.response_data);
}

// Do a request using the HEAD method. Verify that we don't try to read the
// message body (since HEAD has none).
TEST_F(HttpNetworkTransactionTest, Head) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "HEAD";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("HEAD / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
  };
  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 404 Not Found\r\n"),
    MockRead("Server: Blah\r\n"),
    MockRead("Content-Length: 1234\r\n\r\n"),

    // No response body because the test stops reading here.
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // Check that the headers got parsed.
  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ(1234, response->headers->GetContentLength());
  EXPECT_EQ("HTTP/1.1 404 Not Found", response->headers->GetStatusLine());

  std::string server_header;
  void* iter = NULL;
  bool has_server_header = response->headers->EnumerateHeader(
      &iter, "Server", &server_header);
  EXPECT_TRUE(has_server_header);
  EXPECT_EQ("Blah", server_header);

  // Reading should give EOF right away, since there is no message body
  // (despite non-zero content-length).
  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("", response_data);
}

TEST_F(HttpNetworkTransactionTest, ReuseConnection) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("hello"),
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  const char* kExpectedResponseData[] = {
    "hello", "world"
  };

  for (int i = 0; i < 2; ++i) {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/");
    request.load_flags = 0;

    TestCompletionCallback callback;

    int rv = trans->Start(&request, &callback, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

    std::string response_data;
    rv = ReadTransaction(trans.get(), &response_data);
    EXPECT_EQ(OK, rv);
    EXPECT_EQ(kExpectedResponseData[i], response_data);
  }
}

TEST_F(HttpNetworkTransactionTest, Ignores100) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.foo.com/");
  request.upload_data = new UploadData;
  request.upload_data->AppendBytes("foo", 3);
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 100 Continue\r\n\r\n"),
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("hello world", response_data);
}

// This test is almost the same as Ignores100 above, but the response contains
// a 102 instead of a 100. Also, instead of HTTP/1.0 the response is
// HTTP/1.1 and the two status headers are read in one read.
TEST_F(HttpNetworkTransactionTest, Ignores1xx) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 102 Unspecified status code\r\n\r\n"
             "HTTP/1.1 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("hello world", response_data);
}

TEST_F(HttpNetworkTransactionTest, Incomplete100ThenEOF) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead(false, "HTTP/1.0 100 Continue\r\n"),
    MockRead(true, 0),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("", response_data);
}

TEST_F(HttpNetworkTransactionTest, EmptyResponse) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead(true, 0),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_EMPTY_RESPONSE, rv);
}

// read_failure specifies a read failure that should cause the network
// transaction to resend the request.
void HttpNetworkTransactionTest::KeepAliveConnectionResendRequestTest(
    const MockRead& read_failure) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data1_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("hello"),
    read_failure,  // Now, we reuse the connection and fail the first read.
  };
  StaticSocketDataProvider data1(data1_reads, arraysize(data1_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  MockRead data2_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"),
    MockRead("world"),
    MockRead(true, OK),
  };
  StaticSocketDataProvider data2(data2_reads, arraysize(data2_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  const char* kExpectedResponseData[] = {
    "hello", "world"
  };

  for (int i = 0; i < 2; ++i) {
    TestCompletionCallback callback;

    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    int rv = trans->Start(&request, &callback, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

    std::string response_data;
    rv = ReadTransaction(trans.get(), &response_data);
    EXPECT_EQ(OK, rv);
    EXPECT_EQ(kExpectedResponseData[i], response_data);
  }
}

TEST_F(HttpNetworkTransactionTest, KeepAliveConnectionReset) {
  MockRead read_failure(true, ERR_CONNECTION_RESET);
  KeepAliveConnectionResendRequestTest(read_failure);
}

TEST_F(HttpNetworkTransactionTest, KeepAliveConnectionEOF) {
  MockRead read_failure(false, OK);  // EOF
  KeepAliveConnectionResendRequestTest(read_failure);
}

TEST_F(HttpNetworkTransactionTest, NonKeepAliveConnectionReset) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead(true, ERR_CONNECTION_RESET),
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),  // Should not be used
    MockRead("hello world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_CONNECTION_RESET, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response == NULL);
}

// What do various browsers do when the server closes a non-keepalive
// connection without sending any response header or body?
//
// IE7: error page
// Safari 3.1.2 (Windows): error page
// Firefox 3.0.1: blank page
// Opera 9.52: after five attempts, blank page
// Us with WinHTTP: error page (ERR_INVALID_RESPONSE)
// Us: error page (EMPTY_RESPONSE)
TEST_F(HttpNetworkTransactionTest, NonKeepAliveConnectionEOF) {
  MockRead data_reads[] = {
    MockRead(false, OK),  // EOF
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),  // Should not be used
    MockRead("hello world"),
    MockRead(false, OK),
  };
  SimpleGetHelperResult out = SimpleGetHelper(data_reads,
                                              arraysize(data_reads));
  EXPECT_EQ(ERR_EMPTY_RESPONSE, out.rv);
}

// Test the request-challenge-retry sequence for basic auth.
// (basic auth is the easiest to mock, because it has no randomness).
TEST_F(HttpNetworkTransactionTest, BasicAuth) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    // Give a couple authenticate options (only the middle one is actually
    // supported).
    MockRead("WWW-Authenticate: Basic invalid\r\n"),  // Malformed.
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("WWW-Authenticate: UNSUPPORTED realm=\"FOO\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    // Large content-length -- won't matter, as connection will be reset.
    MockRead("Content-Length: 10000\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After calling trans->RestartWithAuth(), this is the request we should
  // be issuing -- the final header line contains the credentials.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

TEST_F(HttpNetworkTransactionTest, DoNotSendAuth) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = net::LOAD_DO_NOT_SEND_AUTH_DATA;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    // Large content-length -- won't matter, as connection will be reset.
    MockRead("Content-Length: 10000\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);
  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(0, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// connection.
TEST_F(HttpNetworkTransactionTest, BasicAuthKeepAlive) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),

    // After calling trans->RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 14\r\n\r\n"),
    MockRead("Unauthorized\r\n"),

    // Lastly, the server responds with the actual content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// connection and with no response body to drain.
TEST_F(HttpNetworkTransactionTest, BasicAuthKeepAliveNoBody) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),

    // After calling trans->RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 0\r\n\r\n"),  // No response body.

    // Lastly, the server responds with the actual content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// connection and with a large response body to drain.
TEST_F(HttpNetworkTransactionTest, BasicAuthKeepAliveLargeBody) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),

    // After calling trans->RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Respond with 5 kb of response body.
  std::string large_body_string("Unauthorized");
  large_body_string.append(5 * 1024, ' ');
  large_body_string.append("\r\n");

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    // 5134 = 12 + 5 * 1024 + 2
    MockRead("Content-Length: 5134\r\n\r\n"),
    MockRead(true, large_body_string.data(), large_body_string.size()),

    // Lastly, the server responds with the actual content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// connection, but the server gets impatient and closes the connection.
TEST_F(HttpNetworkTransactionTest, BasicAuthKeepAliveImpatientServer) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
    // This simulates the seemingly successful write to a closed connection
    // if the bug is not fixed.
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 14\r\n\r\n"),
    // Tell MockTCPClientSocket to simulate the server closing the connection.
    MockRead(false, ERR_TEST_PEER_CLOSE_AFTER_NEXT_MOCK_READ),
    MockRead("Unauthorized\r\n"),
    MockRead(false, OK),  // The server closes the connection.
  };

  // After calling trans->RestartWithAuth(), this is the request we should
  // be issuing -- the final header line contains the credentials.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test the request-challenge-retry sequence for basic auth, over a keep-alive
// proxy connection, when setting up an SSL tunnel.
TEST_F(HttpNetworkTransactionTest, BasicAuthProxyKeepAlive) {
  // Configure against proxy server "myproxy:70".
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));
  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  // Ensure that proxy authentication is attempted even
  // when the no authentication data flag is set.
  request.load_flags = net::LOAD_DO_NOT_SEND_AUTH_DATA;

  // Since we have proxy, should try to establish tunnel.
  MockWrite data_writes1[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),

    // After calling trans->RestartWithAuth(), this is the request we should
    // be issuing -- the final header line contains the credentials.
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n"
              "Proxy-Authorization: Basic Zm9vOmJheg==\r\n\r\n"),
  };

  // The proxy responds to the connect with a 407, using a persistent
  // connection.
  MockRead data_reads1[] = {
    // No credentials.
    MockRead("HTTP/1.1 407 Proxy Authentication Required\r\n"),
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead("0123456789"),

    // Wrong credentials (wrong password).
    MockRead("HTTP/1.1 407 Proxy Authentication Required\r\n"),
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    // No response body because the test stops reading here.
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  EXPECT_TRUE(response->headers->IsKeepAlive());
  EXPECT_EQ(407, response->headers->response_code());
  EXPECT_EQ(10, response->headers->GetContentLength());
  EXPECT_TRUE(HttpVersion(1, 1) == response->headers->GetHttpVersion());

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"myproxy:70", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  // Wrong password (should be "bar").
  rv = trans->RestartWithAuth(L"foo", L"baz", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  EXPECT_TRUE(response->headers->IsKeepAlive());
  EXPECT_EQ(407, response->headers->response_code());
  EXPECT_EQ(10, response->headers->GetContentLength());
  EXPECT_TRUE(HttpVersion(1, 1) == response->headers->GetHttpVersion());

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"myproxy:70", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);
}

// Test that we don't read the response body when we fail to establish a tunnel,
// even if the user cancels the proxy's auth attempt.
TEST_F(HttpNetworkTransactionTest, BasicAuthProxyCancelTunnel) {
  // Configure against proxy server "myproxy:70".
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  // Since we have proxy, should try to establish tunnel.
  MockWrite data_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  // The proxy responds to the connect with a 407.
  MockRead data_reads[] = {
    MockRead("HTTP/1.1 407 Proxy Authentication Required\r\n"),
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  EXPECT_TRUE(response->headers->IsKeepAlive());
  EXPECT_EQ(407, response->headers->response_code());
  EXPECT_EQ(10, response->headers->GetContentLength());
  EXPECT_TRUE(HttpVersion(1, 1) == response->headers->GetHttpVersion());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(ERR_TUNNEL_CONNECTION_FAILED, rv);
}

void HttpNetworkTransactionTest::ConnectStatusHelperWithExpectedStatus(
    const MockRead& status, int expected_status) {
  // Configure against proxy server "myproxy:70".
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  // Since we have proxy, should try to establish tunnel.
  MockWrite data_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    status,
    MockRead("Content-Length: 10\r\n\r\n"),
    // No response body because the test stops reading here.
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(expected_status, rv);
}

void HttpNetworkTransactionTest::ConnectStatusHelper(const MockRead& status) {
  ConnectStatusHelperWithExpectedStatus(
      status, ERR_TUNNEL_CONNECTION_FAILED);
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus100) {
  ConnectStatusHelper(MockRead("HTTP/1.1 100 Continue\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus101) {
  ConnectStatusHelper(MockRead("HTTP/1.1 101 Switching Protocols\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus201) {
  ConnectStatusHelper(MockRead("HTTP/1.1 201 Created\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus202) {
  ConnectStatusHelper(MockRead("HTTP/1.1 202 Accepted\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus203) {
  ConnectStatusHelper(
      MockRead("HTTP/1.1 203 Non-Authoritative Information\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus204) {
  ConnectStatusHelper(MockRead("HTTP/1.1 204 No Content\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus205) {
  ConnectStatusHelper(MockRead("HTTP/1.1 205 Reset Content\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus206) {
  ConnectStatusHelper(MockRead("HTTP/1.1 206 Partial Content\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus300) {
  ConnectStatusHelper(MockRead("HTTP/1.1 300 Multiple Choices\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus301) {
  ConnectStatusHelper(MockRead("HTTP/1.1 301 Moved Permanently\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus302) {
  ConnectStatusHelper(MockRead("HTTP/1.1 302 Found\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus303) {
  ConnectStatusHelper(MockRead("HTTP/1.1 303 See Other\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus304) {
  ConnectStatusHelper(MockRead("HTTP/1.1 304 Not Modified\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus305) {
  ConnectStatusHelper(MockRead("HTTP/1.1 305 Use Proxy\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus306) {
  ConnectStatusHelper(MockRead("HTTP/1.1 306\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus307) {
  ConnectStatusHelper(MockRead("HTTP/1.1 307 Temporary Redirect\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus400) {
  ConnectStatusHelper(MockRead("HTTP/1.1 400 Bad Request\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus401) {
  ConnectStatusHelper(MockRead("HTTP/1.1 401 Unauthorized\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus402) {
  ConnectStatusHelper(MockRead("HTTP/1.1 402 Payment Required\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus403) {
  ConnectStatusHelper(MockRead("HTTP/1.1 403 Forbidden\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus404) {
  ConnectStatusHelper(MockRead("HTTP/1.1 404 Not Found\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus405) {
  ConnectStatusHelper(MockRead("HTTP/1.1 405 Method Not Allowed\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus406) {
  ConnectStatusHelper(MockRead("HTTP/1.1 406 Not Acceptable\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus407) {
  ConnectStatusHelperWithExpectedStatus(
      MockRead("HTTP/1.1 407 Proxy Authentication Required\r\n"),
      ERR_PROXY_AUTH_REQUESTED);
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus408) {
  ConnectStatusHelper(MockRead("HTTP/1.1 408 Request Timeout\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus409) {
  ConnectStatusHelper(MockRead("HTTP/1.1 409 Conflict\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus410) {
  ConnectStatusHelper(MockRead("HTTP/1.1 410 Gone\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus411) {
  ConnectStatusHelper(MockRead("HTTP/1.1 411 Length Required\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus412) {
  ConnectStatusHelper(MockRead("HTTP/1.1 412 Precondition Failed\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus413) {
  ConnectStatusHelper(MockRead("HTTP/1.1 413 Request Entity Too Large\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus414) {
  ConnectStatusHelper(MockRead("HTTP/1.1 414 Request-URI Too Long\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus415) {
  ConnectStatusHelper(MockRead("HTTP/1.1 415 Unsupported Media Type\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus416) {
  ConnectStatusHelper(
      MockRead("HTTP/1.1 416 Requested Range Not Satisfiable\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus417) {
  ConnectStatusHelper(MockRead("HTTP/1.1 417 Expectation Failed\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus500) {
  ConnectStatusHelper(MockRead("HTTP/1.1 500 Internal Server Error\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus501) {
  ConnectStatusHelper(MockRead("HTTP/1.1 501 Not Implemented\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus502) {
  ConnectStatusHelper(MockRead("HTTP/1.1 502 Bad Gateway\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus503) {
  ConnectStatusHelper(MockRead("HTTP/1.1 503 Service Unavailable\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus504) {
  ConnectStatusHelper(MockRead("HTTP/1.1 504 Gateway Timeout\r\n"));
}

TEST_F(HttpNetworkTransactionTest, ConnectStatus505) {
  ConnectStatusHelper(MockRead("HTTP/1.1 505 HTTP Version Not Supported\r\n"));
}

// Test the flow when both the proxy server AND origin server require
// authentication. Again, this uses basic auth for both since that is
// the simplest to mock.
TEST_F(HttpNetworkTransactionTest, BasicAuthProxyThenServer) {
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  // Configure against proxy server "myproxy:70".
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(
      CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET http://www.google.com/ HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 407 Unauthorized\r\n"),
    // Give a couple authenticate options (only the middle one is actually
    // supported).
    MockRead("Proxy-Authenticate: Basic invalid\r\n"),  // Malformed.
    MockRead("Proxy-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Proxy-Authenticate: UNSUPPORTED realm=\"FOO\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    // Large content-length -- won't matter, as connection will be reset.
    MockRead("Content-Length: 10000\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After calling trans->RestartWithAuth() the first time, this is the
  // request we should be issuing -- the final header line contains the
  // proxy's credentials.
  MockWrite data_writes2[] = {
    MockWrite("GET http://www.google.com/ HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n"
              "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Now the proxy server lets the request pass through to origin server.
  // The origin server responds with a 401.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    // Note: We are using the same realm-name as the proxy server. This is
    // completely valid, as realms are unique across hosts.
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 2000\r\n\r\n"),
    MockRead(false, ERR_FAILED),  // Won't be reached.
  };

  // After calling trans->RestartWithAuth() the second time, we should send
  // the credentials for both the proxy and origin server.
  MockWrite data_writes3[] = {
    MockWrite("GET http://www.google.com/ HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n"
              "Proxy-Authorization: Basic Zm9vOmJhcg==\r\n"
              "Authorization: Basic Zm9vMjpiYXIy\r\n\r\n"),
  };

  // Lastly we get the desired content.
  MockRead data_reads3[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                 data_writes3, arraysize(data_writes3));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);
  session_deps.socket_factory.AddSocketDataProvider(&data3);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"myproxy:70", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback3;

  rv = trans->RestartWithAuth(L"foo2", L"bar2", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// For the NTLM implementation using SSPI, we skip the NTLM tests since we
// can't hook into its internals to cause it to generate predictable NTLM
// authorization headers.
#if defined(NTLM_PORTABLE)
// The NTLM authentication unit tests were generated by capturing the HTTP
// requests and responses using Fiddler 2 and inspecting the generated random
// bytes in the debugger.

// Enter the correct password and authenticate successfully.
TEST_F(HttpNetworkTransactionTest, NTLMAuth1) {
  HttpAuthHandlerNTLM::ScopedProcSetter proc_setter(MockGenerateRandom1,
                                                         MockGetHostName);
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://172.22.68.17/kids/login.aspx");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    // Negotiate and NTLM are often requested together.  We only support NTLM.
    MockRead("WWW-Authenticate: Negotiate\r\n"),
    MockRead("WWW-Authenticate: NTLM\r\n"),
    MockRead("Connection: close\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    // Missing content -- won't matter, as connection will be reset.
    MockRead(false, ERR_UNEXPECTED),
  };

  MockWrite data_writes2[] = {
    // After restarting with a null identity, this is the
    // request we should be issuing -- the final header line contains a Type
    // 1 message.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM "
              "TlRMTVNTUAABAAAAB4IIAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"),

    // After calling trans->RestartWithAuth(), we should send a Type 3 message
    // (the credentials for the origin server).  The second request continues
    // on the same connection.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM TlRMTVNTUAADAAAAGAAYAGgAAAAYABgAgA"
              "AAAAAAAABAAAAAGAAYAEAAAAAQABAAWAAAAAAAAAAAAAAABYIIAHQA"
              "ZQBzAHQAaQBuAGcALQBuAHQAbABtAFcAVABDAC0AVwBJAE4ANwBVKW"
              "Yma5xzVAAAAAAAAAAAAAAAAAAAAACH+gWcm+YsP9Tqb9zCR3WAeZZX"
              "ahlhx5I=\r\n\r\n"),
  };

  MockRead data_reads2[] = {
    // The origin server responds with a Type 2 message.
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    MockRead("WWW-Authenticate: NTLM "
             "TlRMTVNTUAACAAAADAAMADgAAAAFgokCjGpMpPGlYKkAAAAAAAAAALo"
             "AugBEAAAABQEoCgAAAA9HAE8ATwBHAEwARQACAAwARwBPAE8ARwBMAE"
             "UAAQAaAEEASwBFAEUAUwBBAFIAQQAtAEMATwBSAFAABAAeAGMAbwByA"
             "HAALgBnAG8AbwBnAGwAZQAuAGMAbwBtAAMAQABhAGsAZQBlAHMAYQBy"
             "AGEALQBjAG8AcgBwAC4AYQBkAC4AYwBvAHIAcAAuAGcAbwBvAGcAbAB"
             "lAC4AYwBvAG0ABQAeAGMAbwByAHAALgBnAG8AbwBnAGwAZQAuAGMAbw"
             "BtAAAAAAA=\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    MockRead("You are not authorized to view this page\r\n"),

    // Lastly we get the desired content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=utf-8\r\n"),
    MockRead("Content-Length: 13\r\n\r\n"),
    MockRead("Please Login\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"172.22.68.17:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"", response->auth_challenge->realm);
  EXPECT_EQ(L"ntlm", response->auth_challenge->scheme);

  TestCompletionCallback callback3;

  rv = trans->RestartWithAuth(L"testing-ntlm", L"testing-ntlm", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(13, response->headers->GetContentLength());
}

// Enter a wrong password, and then the correct one.
TEST_F(HttpNetworkTransactionTest, NTLMAuth2) {
  HttpAuthHandlerNTLM::ScopedProcSetter proc_setter(MockGenerateRandom2,
                                                         MockGetHostName);
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://172.22.68.17/kids/login.aspx");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    // Negotiate and NTLM are often requested together.  We only support NTLM.
    MockRead("WWW-Authenticate: Negotiate\r\n"),
    MockRead("WWW-Authenticate: NTLM\r\n"),
    MockRead("Connection: close\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    // Missing content -- won't matter, as connection will be reset.
    MockRead(false, ERR_UNEXPECTED),
  };

  MockWrite data_writes2[] = {
    // After restarting with a null identity, this is the
    // request we should be issuing -- the final header line contains a Type
    // 1 message.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM "
              "TlRMTVNTUAABAAAAB4IIAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"),

    // After calling trans->RestartWithAuth(), we should send a Type 3 message
    // (the credentials for the origin server).  The second request continues
    // on the same connection.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM TlRMTVNTUAADAAAAGAAYAGgAAAAYABgAgA"
              "AAAAAAAABAAAAAGAAYAEAAAAAQABAAWAAAAAAAAAAAAAAABYIIAHQA"
              "ZQBzAHQAaQBuAGcALQBuAHQAbABtAFcAVABDAC0AVwBJAE4ANwCWeY"
              "XnSZNwoQAAAAAAAAAAAAAAAAAAAADLa34/phTTKzNTWdub+uyFleOj"
              "4Ww7b7E=\r\n\r\n"),
  };

  MockRead data_reads2[] = {
    // The origin server responds with a Type 2 message.
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    MockRead("WWW-Authenticate: NTLM "
             "TlRMTVNTUAACAAAADAAMADgAAAAFgokCbVWUZezVGpAAAAAAAAAAALo"
             "AugBEAAAABQEoCgAAAA9HAE8ATwBHAEwARQACAAwARwBPAE8ARwBMAE"
             "UAAQAaAEEASwBFAEUAUwBBAFIAQQAtAEMATwBSAFAABAAeAGMAbwByA"
             "HAALgBnAG8AbwBnAGwAZQAuAGMAbwBtAAMAQABhAGsAZQBlAHMAYQBy"
             "AGEALQBjAG8AcgBwAC4AYQBkAC4AYwBvAHIAcAAuAGcAbwBvAGcAbAB"
             "lAC4AYwBvAG0ABQAeAGMAbwByAHAALgBnAG8AbwBnAGwAZQAuAGMAbw"
             "BtAAAAAAA=\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    MockRead("You are not authorized to view this page\r\n"),

    // Wrong password.
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    MockRead("WWW-Authenticate: Negotiate\r\n"),
    MockRead("WWW-Authenticate: NTLM\r\n"),
    MockRead("Connection: close\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    // Missing content -- won't matter, as connection will be reset.
    MockRead(false, ERR_UNEXPECTED),
  };

  MockWrite data_writes3[] = {
    // After restarting with a null identity, this is the
    // request we should be issuing -- the final header line contains a Type
    // 1 message.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM "
              "TlRMTVNTUAABAAAAB4IIAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n"),

    // After calling trans->RestartWithAuth(), we should send a Type 3 message
    // (the credentials for the origin server).  The second request continues
    // on the same connection.
    MockWrite("GET /kids/login.aspx HTTP/1.1\r\n"
              "Host: 172.22.68.17\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: NTLM TlRMTVNTUAADAAAAGAAYAGgAAAAYABgAgA"
              "AAAAAAAABAAAAAGAAYAEAAAAAQABAAWAAAAAAAAAAAAAAABYIIAHQA"
              "ZQBzAHQAaQBuAGcALQBuAHQAbABtAFcAVABDAC0AVwBJAE4ANwBO54"
              "dFMVvTHwAAAAAAAAAAAAAAAAAAAACS7sT6Uzw7L0L//WUqlIaVWpbI"
              "+4MUm7c=\r\n\r\n"),
  };

  MockRead data_reads3[] = {
    // The origin server responds with a Type 2 message.
    MockRead("HTTP/1.1 401 Access Denied\r\n"),
    MockRead("WWW-Authenticate: NTLM "
             "TlRMTVNTUAACAAAADAAMADgAAAAFgokCL24VN8dgOR8AAAAAAAAAALo"
             "AugBEAAAABQEoCgAAAA9HAE8ATwBHAEwARQACAAwARwBPAE8ARwBMAE"
             "UAAQAaAEEASwBFAEUAUwBBAFIAQQAtAEMATwBSAFAABAAeAGMAbwByA"
             "HAALgBnAG8AbwBnAGwAZQAuAGMAbwBtAAMAQABhAGsAZQBlAHMAYQBy"
             "AGEALQBjAG8AcgBwAC4AYQBkAC4AYwBvAHIAcAAuAGcAbwBvAGcAbAB"
             "lAC4AYwBvAG0ABQAeAGMAbwByAHAALgBnAG8AbwBnAGwAZQAuAGMAbw"
             "BtAAAAAAA=\r\n"),
    MockRead("Content-Length: 42\r\n"),
    MockRead("Content-Type: text/html\r\n\r\n"),
    MockRead("You are not authorized to view this page\r\n"),

    // Lastly we get the desired content.
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=utf-8\r\n"),
    MockRead("Content-Length: 13\r\n\r\n"),
    MockRead("Please Login\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                 data_writes3, arraysize(data_writes3));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);
  session_deps.socket_factory.AddSocketDataProvider(&data3);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"172.22.68.17:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"", response->auth_challenge->realm);
  EXPECT_EQ(L"ntlm", response->auth_challenge->scheme);

  TestCompletionCallback callback3;

  // Enter the wrong password.
  rv = trans->RestartWithAuth(L"testing-ntlm", L"wrongpassword", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback4;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback4);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback4.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"172.22.68.17:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"", response->auth_challenge->realm);
  EXPECT_EQ(L"ntlm", response->auth_challenge->scheme);

  TestCompletionCallback callback5;

  // Now enter the right password.
  rv = trans->RestartWithAuth(L"testing-ntlm", L"testing-ntlm", &callback5);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback5.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(13, response->headers->GetContentLength());
}
#endif  // NTLM_PORTABLE

// Test reading a server response which has only headers, and no body.
// After some maximum number of bytes is consumed, the transaction should
// fail with ERR_RESPONSE_HEADERS_TOO_BIG.
TEST_F(HttpNetworkTransactionTest, LargeHeadersNoBody) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  // Respond with 300 kb of headers (we should fail after 256 kb).
  std::string large_headers_string;
  FillLargeHeadersString(&large_headers_string, 300 * 1024);

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead(true, large_headers_string.data(), large_headers_string.size()),
    MockRead("\r\nBODY"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_RESPONSE_HEADERS_TOO_BIG, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response == NULL);
}

// Make sure that we don't try to reuse a TCPClientSocket when failing to
// establish tunnel.
// http://code.google.com/p/chromium/issues/detail?id=3772
TEST_F(HttpNetworkTransactionTest, DontRecycleTCPSocketForSSLTunnel) {
  // Configure against proxy server "myproxy:70".
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  // Since we have proxy, should try to establish tunnel.
  MockWrite data_writes1[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  // The proxy responds to the connect with a 404, using a persistent
  // connection. Usually a proxy would return 501 (not implemented),
  // or 200 (tunnel established).
  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 404 Not Found\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_UNEXPECTED),  // Should not be reached.
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(ERR_TUNNEL_CONNECTION_FAILED, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response == NULL);

  // Empty the current queue.  This is necessary because idle sockets are
  // added to the connection pool asynchronously with a PostTask.
  MessageLoop::current()->RunAllPending();

  // We now check to make sure the TCPClientSocket was not added back to
  // the pool.
  EXPECT_EQ(0, session->tcp_socket_pool()->IdleSocketCount());
  trans.reset();
  MessageLoop::current()->RunAllPending();
  // Make sure that the socket didn't get recycled after calling the destructor.
  EXPECT_EQ(0, session->tcp_socket_pool()->IdleSocketCount());
}

// Make sure that we recycle a socket after reading all of the response body.
TEST_F(HttpNetworkTransactionTest, RecycleSocket) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    // A part of the response body is received with the response headers.
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhel"),
    // The rest of the response body is received in two parts.
    MockRead("lo"),
    MockRead(" world"),
    MockRead("junk"),  // Should not be read!!
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  std::string status_line = response->headers->GetStatusLine();
  EXPECT_EQ("HTTP/1.1 200 OK", status_line);

  EXPECT_EQ(0, session->tcp_socket_pool()->IdleSocketCount());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("hello world", response_data);

  // Empty the current queue.  This is necessary because idle sockets are
  // added to the connection pool asynchronously with a PostTask.
  MessageLoop::current()->RunAllPending();

  // We now check to make sure the socket was added back to the pool.
  EXPECT_EQ(1, session->tcp_socket_pool()->IdleSocketCount());
}

// Make sure that we recycle a socket after a zero-length response.
// http://crbug.com/9880
TEST_F(HttpNetworkTransactionTest, RecycleSocketAfterZeroContentLength) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/csi?v=3&s=web&action=&"
                     "tran=undefined&ei=mAXcSeegAo-SMurloeUN&"
                     "e=17259,18167,19592,19773,19981,20133,20173,20233&"
                     "rt=prt.2642,ol.2649,xjs.2951");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 204 No Content\r\n"
             "Content-Length: 0\r\n"
             "Content-Type: text/html\r\n\r\n"),
    MockRead("junk"),  // Should not be read!!
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  std::string status_line = response->headers->GetStatusLine();
  EXPECT_EQ("HTTP/1.1 204 No Content", status_line);

  EXPECT_EQ(0, session->tcp_socket_pool()->IdleSocketCount());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("", response_data);

  // Empty the current queue.  This is necessary because idle sockets are
  // added to the connection pool asynchronously with a PostTask.
  MessageLoop::current()->RunAllPending();

  // We now check to make sure the socket was added back to the pool.
  EXPECT_EQ(1, session->tcp_socket_pool()->IdleSocketCount());
}

TEST_F(HttpNetworkTransactionTest, ResendRequestOnWriteBodyError) {
  HttpRequestInfo request[2];
  // Transaction 1: a GET request that succeeds.  The socket is recycled
  // after use.
  request[0].method = "GET";
  request[0].url = GURL("http://www.google.com/");
  request[0].load_flags = 0;
  // Transaction 2: a POST request.  Reuses the socket kept alive from
  // transaction 1.  The first attempts fails when writing the POST data.
  // This causes the transaction to retry with a new socket.  The second
  // attempt succeeds.
  request[1].method = "POST";
  request[1].url = GURL("http://www.google.com/login.cgi");
  request[1].upload_data = new UploadData;
  request[1].upload_data->AppendBytes("foo", 3);
  request[1].load_flags = 0;

  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  // The first socket is used for transaction 1 and the first attempt of
  // transaction 2.

  // The response of transaction 1.
  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  // The mock write results of transaction 1 and the first attempt of
  // transaction 2.
  MockWrite data_writes1[] = {
    MockWrite(false, 64),  // GET
    MockWrite(false, 93),  // POST
    MockWrite(false, ERR_CONNECTION_ABORTED),  // POST data
  };
  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));

  // The second socket is used for the second attempt of transaction 2.

  // The response of transaction 2.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n"),
    MockRead("welcome"),
    MockRead(false, OK),
  };
  // The mock write results of the second attempt of transaction 2.
  MockWrite data_writes2[] = {
    MockWrite(false, 93),  // POST
    MockWrite(false, 3),  // POST data
  };
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));

  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  const char* kExpectedResponseData[] = {
    "hello world", "welcome"
  };

  for (int i = 0; i < 2; ++i) {
    scoped_ptr<HttpTransaction> trans(
        new HttpNetworkTransaction(session));

    TestCompletionCallback callback;

    int rv = trans->Start(&request[i], &callback, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_TRUE(response != NULL);

    EXPECT_TRUE(response->headers != NULL);
    EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

    std::string response_data;
    rv = ReadTransaction(trans.get(), &response_data);
    EXPECT_EQ(OK, rv);
    EXPECT_EQ(kExpectedResponseData[i], response_data);
  }
}

// Test the request-challenge-retry sequence for basic auth when there is
// an identity in the URL. The request should be sent as normal, but when
// it fails the identity from the URL is used to answer the challenge.
TEST_F(HttpNetworkTransactionTest, AuthIdentityInURL) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  // Note: the URL has a username:password in it.
  request.url = GURL("http://foo:b@r@www.google.com/");

  // The password contains an escaped character -- for this test to pass it
  // will need to be unescaped by HttpNetworkTransaction.
  EXPECT_EQ("b%40r", request.url.password());

  request.load_flags = LOAD_NORMAL;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After the challenge above, the transaction will be restarted using the
  // identity from the url (foo, b@r) to answer the challenge.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJAcg==\r\n\r\n"),
  };

  MockRead data_reads2[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // There is no challenge info, since the identity in URL worked.
  EXPECT_TRUE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(100, response->headers->GetContentLength());

  // Empty the current queue.
  MessageLoop::current()->RunAllPending();
}

// Test the request-challenge-retry sequence for basic auth when there is
// an incorrect identity in the URL. The identity from the URL should be used
// only once.
TEST_F(HttpNetworkTransactionTest, WrongAuthIdentityInURL) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  // Note: the URL has a username:password in it.  The password "baz" is
  // wrong (should be "bar").
  request.url = GURL("http://foo:baz@www.google.com/");

  request.load_flags = LOAD_NORMAL;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After the challenge above, the transaction will be restarted using the
  // identity from the url (foo, baz) to answer the challenge.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJheg==\r\n\r\n"),
  };

  MockRead data_reads2[] = {
    MockRead("HTTP/1.0 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 10\r\n\r\n"),
    MockRead(false, ERR_FAILED),
  };

  // After the challenge above, the transaction will be restarted using the
  // identity supplied by the user (foo, bar) to answer the challenge.
  MockWrite data_writes3[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  MockRead data_reads3[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                 data_writes3, arraysize(data_writes3));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);
  session_deps.socket_factory.AddSocketDataProvider(&data3);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  EXPECT_TRUE(trans->IsReadyToRestartForAuth());
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback3;
  rv = trans->RestartWithAuth(L"foo", L"bar", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);
  EXPECT_FALSE(trans->IsReadyToRestartForAuth());

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // There is no challenge info, since the identity worked.
  EXPECT_TRUE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(100, response->headers->GetContentLength());

  // Empty the current queue.
  MessageLoop::current()->RunAllPending();
}

// Test that previously tried username/passwords for a realm get re-used.
TEST_F(HttpNetworkTransactionTest, BasicAuthCacheAndPreauth) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  // Transaction 1: authenticate (foo, bar) on MyRealm1
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/x/y/z");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/y/z HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n\r\n"),
    };

    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // Resend with authorization (username=foo, password=bar)
    MockWrite data_writes2[] = {
      MockWrite("GET /x/y/z HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    // The password prompt info should have been set in
    // response->auth_challenge.
    EXPECT_FALSE(response->auth_challenge.get() == NULL);

    EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
    EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
    EXPECT_EQ(L"basic", response->auth_challenge->scheme);

    TestCompletionCallback callback2;

    rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);

    response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }

  // ------------------------------------------------------------------------

  // Transaction 2: authenticate (foo2, bar2) on MyRealm2
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    // Note that Transaction 1 was at /x/y/z, so this is in the same
    // protection space as MyRealm1.
    request.url = GURL("http://www.google.com/x/y/a/b");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/y/a/b HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                // Send preemptive authorization for MyRealm1
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // The server didn't like the preemptive authorization, and
    // challenges us for a different realm (MyRealm2).
    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm2\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // Resend with authorization for MyRealm2 (username=foo2, password=bar2)
    MockWrite data_writes2[] = {
      MockWrite("GET /x/y/a/b HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vMjpiYXIy\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    // The password prompt info should have been set in
    // response->auth_challenge.
    EXPECT_FALSE(response->auth_challenge.get() == NULL);

    EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
    EXPECT_EQ(L"MyRealm2", response->auth_challenge->realm);
    EXPECT_EQ(L"basic", response->auth_challenge->scheme);

    TestCompletionCallback callback2;

    rv = trans->RestartWithAuth(L"foo2", L"bar2", &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);

    response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }

  // ------------------------------------------------------------------------

  // Transaction 3: Resend a request in MyRealm's protection space --
  // succeed with preemptive authorization.
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/x/y/z2");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/y/z2 HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                // The authorization for MyRealm1 gets sent preemptively
                // (since the url is in the same protection space)
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // Sever accepts the preemptive authorization
    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    session_deps.socket_factory.AddSocketDataProvider(&data1);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }

  // ------------------------------------------------------------------------

  // Transaction 4: request another URL in MyRealm (however the
  // url is not known to belong to the protection space, so no pre-auth).
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/x/1");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /x/1 HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n\r\n"),
    };

    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // Resend with authorization from MyRealm's cache.
    MockWrite data_writes2[] = {
      MockWrite("GET /x/1 HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    EXPECT_TRUE(trans->IsReadyToRestartForAuth());
    TestCompletionCallback callback2;
    rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);
    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);
    EXPECT_FALSE(trans->IsReadyToRestartForAuth());

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }

  // ------------------------------------------------------------------------

  // Transaction 5: request a URL in MyRealm, but the server rejects the
  // cached identity. Should invalidate and re-prompt.
  {
    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://www.google.com/p/q/t");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET /p/q/t HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n\r\n"),
    };

    MockRead data_reads1[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // Resend with authorization from cache for MyRealm.
    MockWrite data_writes2[] = {
      MockWrite("GET /p/q/t HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    };

    // Sever rejects the authorization.
    MockRead data_reads2[] = {
      MockRead("HTTP/1.0 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
      MockRead("Content-Length: 10000\r\n\r\n"),
      MockRead(false, ERR_FAILED),
    };

    // At this point we should prompt for new credentials for MyRealm.
    // Restart with username=foo3, password=foo4.
    MockWrite data_writes3[] = {
      MockWrite("GET /p/q/t HTTP/1.1\r\n"
                "Host: www.google.com\r\n"
                "Connection: keep-alive\r\n"
                "Authorization: Basic Zm9vMzpiYXIz\r\n\r\n"),
    };

    // Sever accepts the authorization.
    MockRead data_reads3[] = {
      MockRead("HTTP/1.0 200 OK\r\n"),
      MockRead("Content-Length: 100\r\n\r\n"),
      MockRead(false, OK),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                   data_writes2, arraysize(data_writes2));
    StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                   data_writes3, arraysize(data_writes3));
    session_deps.socket_factory.AddSocketDataProvider(&data1);
    session_deps.socket_factory.AddSocketDataProvider(&data2);
    session_deps.socket_factory.AddSocketDataProvider(&data3);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    EXPECT_TRUE(trans->IsReadyToRestartForAuth());
    TestCompletionCallback callback2;
    rv = trans->RestartWithAuth(std::wstring(), std::wstring(), &callback2);
    EXPECT_EQ(ERR_IO_PENDING, rv);
    rv = callback2.WaitForResult();
    EXPECT_EQ(OK, rv);
    EXPECT_FALSE(trans->IsReadyToRestartForAuth());

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    // The password prompt info should have been set in
    // response->auth_challenge.
    EXPECT_FALSE(response->auth_challenge.get() == NULL);

    EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
    EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
    EXPECT_EQ(L"basic", response->auth_challenge->scheme);

    TestCompletionCallback callback3;

    rv = trans->RestartWithAuth(L"foo3", L"bar3", &callback3);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback3.WaitForResult();
    EXPECT_EQ(OK, rv);

    response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);
    EXPECT_TRUE(response->auth_challenge.get() == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }
}

// Test the ResetStateForRestart() private method.
TEST_F(HttpNetworkTransactionTest, ResetStateForRestart) {
  // Create a transaction (the dependencies aren't important).
  SessionDependencies session_deps;
  scoped_ptr<HttpNetworkTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  // Setup some state (which we expect ResetStateForRestart() will clear).
  trans->read_buf_ = new IOBuffer(15);
  trans->read_buf_len_ = 15;
  trans->request_headers_ = "Authorization: NTLM";

  // Setup state in response_
  HttpResponseInfo* response = &trans->response_;
  response->auth_challenge = new AuthChallengeInfo();
  response->ssl_info.cert_status = -15;
  response->response_time = base::Time::Now();
  response->was_cached = true;  // (Wouldn't ever actually be true...)

  { // Setup state for response_.vary_data
    HttpRequestInfo request;
    std::string temp("HTTP/1.1 200 OK\nVary: foo, bar\n\n");
    std::replace(temp.begin(), temp.end(), '\n', '\0');
    scoped_refptr<HttpResponseHeaders> headers = new HttpResponseHeaders(temp);
    request.extra_headers = "Foo: 1\nbar: 23";
    EXPECT_TRUE(response->vary_data.Init(request, *headers));
  }

  // Cause the above state to be reset.
  trans->ResetStateForRestart();

  // Verify that the state that needed to be reset, has been reset.
  EXPECT_TRUE(trans->read_buf_.get() == NULL);
  EXPECT_EQ(0, trans->read_buf_len_);
  EXPECT_EQ(0U, trans->request_headers_.size());
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_TRUE(response->headers.get() == NULL);
  EXPECT_EQ(false, response->was_cached);
  EXPECT_EQ(0, response->ssl_info.cert_status);
  EXPECT_FALSE(response->vary_data.is_valid());
}

// Test HTTPS connections to a site with a bad certificate
TEST_F(HttpNetworkTransactionTest, HTTPSBadCertificate) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider ssl_bad_certificate;
  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  SSLSocketDataProvider ssl_bad(true, ERR_CERT_AUTHORITY_INVALID);
  SSLSocketDataProvider ssl(true, OK);

  session_deps.socket_factory.AddSocketDataProvider(&ssl_bad_certificate);
  session_deps.socket_factory.AddSocketDataProvider(&data);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl_bad);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_CERT_AUTHORITY_INVALID, rv);

  rv = trans->RestartIgnoringLastError(&callback);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();

  EXPECT_FALSE(response == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test HTTPS connections to a site with a bad certificate, going through a
// proxy
TEST_F(HttpNetworkTransactionTest, HTTPSBadCertificateViaProxy) {
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  MockWrite proxy_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
  };

  MockRead proxy_reads[] = {
    MockRead("HTTP/1.0 200 Connected\r\n\r\n"),
    MockRead(false, OK)
  };

  MockWrite data_writes[] = {
    MockWrite("CONNECT www.google.com:443 HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Proxy-Connection: keep-alive\r\n\r\n"),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 Connected\r\n\r\n"),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider ssl_bad_certificate(
      proxy_reads, arraysize(proxy_reads),
      proxy_writes, arraysize(proxy_writes));
  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  SSLSocketDataProvider ssl_bad(true, ERR_CERT_AUTHORITY_INVALID);
  SSLSocketDataProvider ssl(true, OK);

  session_deps.socket_factory.AddSocketDataProvider(&ssl_bad_certificate);
  session_deps.socket_factory.AddSocketDataProvider(&data);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl_bad);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  for (int i = 0; i < 2; i++) {
    session_deps.socket_factory.ResetNextMockIndexes();

    scoped_ptr<HttpTransaction> trans(
        new HttpNetworkTransaction(CreateSession(&session_deps)));

    int rv = trans->Start(&request, &callback, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(ERR_CERT_AUTHORITY_INVALID, rv);

    rv = trans->RestartIgnoringLastError(&callback);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();

    EXPECT_FALSE(response == NULL);
    EXPECT_EQ(100, response->headers->GetContentLength());
  }
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_UserAgent) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.user_agent = "Chromium Ultra Awesome X Edition";

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "User-Agent: Chromium Ultra Awesome X Edition\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_Referer) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;
  request.referrer = GURL("http://the.previous.site.com/");

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Referer: http://the.previous.site.com/\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_PostContentLengthZero) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.google.com/");

  MockWrite data_writes[] = {
    MockWrite("POST / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_PutContentLengthZero) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "PUT";
  request.url = GURL("http://www.google.com/");

  MockWrite data_writes[] = {
    MockWrite("PUT / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_HeadContentLengthZero) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "HEAD";
  request.url = GURL("http://www.google.com/");

  MockWrite data_writes[] = {
    MockWrite("HEAD / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_CacheControlNoCache) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = LOAD_BYPASS_CACHE;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Pragma: no-cache\r\n"
              "Cache-Control: no-cache\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest,
       BuildRequest_CacheControlValidateCache) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = LOAD_VALIDATE_CACHE;

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Cache-Control: max-age=0\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_ExtraHeaders) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.extra_headers = "FooHeader: Bar\r\n";

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "FooHeader: Bar\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, BuildRequest_ExtraHeadersStripped) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.extra_headers = "referer: www.foo.com\nhEllo: Kitty\rFoO: bar\r\n";

  MockWrite data_writes[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "hEllo: Kitty\r\n"
              "FoO: bar\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);
}

TEST_F(HttpNetworkTransactionTest, SOCKS4_HTTP_GET) {
  SessionDependencies session_deps(
      CreateFixedProxyService("socks4://myproxy:1080"));

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  char write_buffer[] = { 0x04, 0x01, 0x00, 0x50, 127, 0, 0, 1, 0 };
  char read_buffer[] = { 0x00, 0x5A, 0x00, 0x00, 0, 0, 0, 0 };

  MockWrite data_writes[] = {
    MockWrite(true, write_buffer, arraysize(write_buffer)),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n")
  };

  MockRead data_reads[] = {
    MockRead(true, read_buffer, arraysize(read_buffer)),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n\r\n"),
    MockRead("Payload"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  std::string response_text;
  rv = ReadTransaction(trans.get(), &response_text);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("Payload", response_text);
}

TEST_F(HttpNetworkTransactionTest, SOCKS4_SSL_GET) {
  SessionDependencies session_deps(
      CreateFixedProxyService("socks4://myproxy:1080"));

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  unsigned char write_buffer[] = { 0x04, 0x01, 0x01, 0xBB, 127, 0, 0, 1, 0 };
  unsigned char read_buffer[] = { 0x00, 0x5A, 0x00, 0x00, 0, 0, 0, 0 };

  MockWrite data_writes[] = {
    MockWrite(true, reinterpret_cast<char*>(write_buffer),
              arraysize(write_buffer)),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n")
  };

  MockRead data_reads[] = {
    MockWrite(true, reinterpret_cast<char*>(read_buffer),
              arraysize(read_buffer)),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n\r\n"),
    MockRead("Payload"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  SSLSocketDataProvider ssl(true, OK);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  std::string response_text;
  rv = ReadTransaction(trans.get(), &response_text);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("Payload", response_text);
}

TEST_F(HttpNetworkTransactionTest, SOCKS5_HTTP_GET) {
  SessionDependencies session_deps(
      CreateFixedProxyService("socks5://myproxy:1080"));

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  const char kSOCKS5GreetRequest[] = { 0x05, 0x01, 0x00 };
  const char kSOCKS5GreetResponse[] = { 0x05, 0x00 };
  const char kSOCKS5OkRequest[] = {
    0x05,  // Version
    0x01,  // Command (CONNECT)
    0x00,  // Reserved.
    0x03,  // Address type (DOMAINNAME).
    0x0E,  // Length of domain (14)
    // Domain string:
    'w', 'w', 'w', '.', 'g', 'o', 'o', 'g', 'l', 'e', '.', 'c', 'o', 'm',
    0x00, 0x50,  // 16-bit port (80)
  };
  const char kSOCKS5OkResponse[] =
      { 0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0x00, 0x50 };

  MockWrite data_writes[] = {
    MockWrite(true, kSOCKS5GreetRequest, arraysize(kSOCKS5GreetRequest)),
    MockWrite(true, kSOCKS5OkRequest, arraysize(kSOCKS5OkRequest)),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n")
  };

  MockRead data_reads[] = {
    MockWrite(true, kSOCKS5GreetResponse, arraysize(kSOCKS5GreetResponse)),
    MockWrite(true, kSOCKS5OkResponse, arraysize(kSOCKS5OkResponse)),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n\r\n"),
    MockRead("Payload"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  std::string response_text;
  rv = ReadTransaction(trans.get(), &response_text);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("Payload", response_text);
}

TEST_F(HttpNetworkTransactionTest, SOCKS5_SSL_GET) {
  SessionDependencies session_deps(
      CreateFixedProxyService("socks5://myproxy:1080"));

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  const char kSOCKS5GreetRequest[] = { 0x05, 0x01, 0x00 };
  const char kSOCKS5GreetResponse[] = { 0x05, 0x00 };
  const unsigned char kSOCKS5OkRequest[] = {
    0x05,  // Version
    0x01,  // Command (CONNECT)
    0x00,  // Reserved.
    0x03,  // Address type (DOMAINNAME).
    0x0E,  // Length of domain (14)
    // Domain string:
    'w', 'w', 'w', '.', 'g', 'o', 'o', 'g', 'l', 'e', '.', 'c', 'o', 'm',
    0x01, 0xBB,  // 16-bit port (443)
  };

  const char kSOCKS5OkResponse[] =
      { 0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0x00, 0x00 };

  MockWrite data_writes[] = {
    MockWrite(true, kSOCKS5GreetRequest, arraysize(kSOCKS5GreetRequest)),
    MockWrite(true, reinterpret_cast<const char*>(kSOCKS5OkRequest),
              arraysize(kSOCKS5OkRequest)),
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n")
  };

  MockRead data_reads[] = {
    MockWrite(true, kSOCKS5GreetResponse, arraysize(kSOCKS5GreetResponse)),
    MockWrite(true, kSOCKS5OkResponse, arraysize(kSOCKS5OkResponse)),
    MockRead("HTTP/1.0 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n\r\n"),
    MockRead("Payload"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads),
                                data_writes, arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  SSLSocketDataProvider ssl(true, OK);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  std::string response_text;
  rv = ReadTransaction(trans.get(), &response_text);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("Payload", response_text);
}

// Tests that for connection endpoints the group names are correctly set.
TEST_F(HttpNetworkTransactionTest, GroupNameForProxyConnections) {
  const struct {
    const std::string proxy_server;
    const std::string url;
    const std::string expected_group_name;
  } tests[] = {
    {
      "",  // no proxy (direct)
      "http://www.google.com/direct",
      "http://www.google.com/",
    },
    {
      "http_proxy",
      "http://www.google.com/http_proxy_normal",
      "proxy/http_proxy:80/",
    },
    {
      "socks4://socks_proxy:1080",
      "http://www.google.com/socks4_direct",
      "proxy/socks4://socks_proxy:1080/http://www.google.com/",
    },

    // SSL Tests
    {
      "",
      "https://www.google.com/direct_ssl",
      "https://www.google.com/",
    },
    {
      "http_proxy",
      "https://www.google.com/http_connect_ssl",
      "proxy/http_proxy:80/https://www.google.com/",
    },
    {
      "socks4://socks_proxy:1080",
      "https://www.google.com/socks4_ssl",
      "proxy/socks4://socks_proxy:1080/https://www.google.com/",
    },
  };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(tests); ++i) {
    SessionDependencies session_deps(
        CreateFixedProxyService(tests[i].proxy_server));

    scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

    scoped_refptr<CaptureGroupNameTCPSocketPool> tcp_conn_pool(
        new CaptureGroupNameTCPSocketPool(session.get(),
                                          session->socket_factory()));
    session->tcp_socket_pool_ = tcp_conn_pool.get();
    scoped_refptr<CaptureGroupNameSOCKSSocketPool> socks_conn_pool(
        new CaptureGroupNameSOCKSSocketPool(session.get(),
                                            tcp_conn_pool.get()));
    session->socks_socket_pool_ = socks_conn_pool.get();

    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL(tests[i].url);
    request.load_flags = 0;

    TestCompletionCallback callback;

    // We do not complete this request, the dtor will clean the transaction up.
    EXPECT_EQ(ERR_IO_PENDING, trans->Start(&request, &callback, NULL));
    std::string allgroups = tcp_conn_pool->last_group_name_received() +
                            socks_conn_pool->last_group_name_received();
    EXPECT_EQ(tests[i].expected_group_name, allgroups);
  }
}

TEST_F(HttpNetworkTransactionTest, ReconsiderProxyAfterFailedConnection) {
  SessionDependencies session_deps(
      CreateFixedProxyService("myproxy:70;foobar:80"));

  // This simulates failure resolving all hostnames; that means we will fail
  // connecting to both proxies (myproxy:70 and foobar:80).
  session_deps.host_resolver->rules()->AddSimulatedFailure("*");

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, rv);
}

// Host resolution observer used by
// HttpNetworkTransactionTest.ResolveMadeWithReferrer to check that host
// resovle requests are issued with a referrer of |expected_referrer|.
class ResolutionReferrerObserver : public HostResolver::Observer {
 public:
  explicit ResolutionReferrerObserver(const GURL& expected_referrer)
      : expected_referrer_(expected_referrer),
        called_start_with_referrer_(false),
        called_finish_with_referrer_(false) {
  }

  virtual void OnStartResolution(int id,
                                 const HostResolver::RequestInfo& info) {
    if (info.referrer() == expected_referrer_)
      called_start_with_referrer_ = true;
  }

  virtual void OnFinishResolutionWithStatus(
      int id, bool was_resolved, const HostResolver::RequestInfo& info ) {
    if (info.referrer() == expected_referrer_)
      called_finish_with_referrer_ = true;
  }

  virtual void OnCancelResolution(int id,
                                  const HostResolver::RequestInfo& info ) {
    FAIL() << "Should not be cancelling any requests!";
  }

  bool did_complete_with_expected_referrer() const {
    return called_start_with_referrer_ && called_finish_with_referrer_;
  }

 private:
  GURL expected_referrer_;
  bool called_start_with_referrer_;
  bool called_finish_with_referrer_;

  DISALLOW_COPY_AND_ASSIGN(ResolutionReferrerObserver);
};

// Make sure that when HostResolver::Resolve() is invoked, it passes through
// the "referrer". This is depended on by the DNS prefetch observer.
TEST_F(HttpNetworkTransactionTest, ResolveMadeWithReferrer) {
  GURL referrer = GURL("http://expected-referrer/");
  EXPECT_TRUE(referrer.is_valid());
  ResolutionReferrerObserver resolution_observer(referrer);

  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(
      CreateSession(&session_deps)));

  // Attach an observer to watch the host resolutions being made.
  session_deps.host_resolver->AddObserver(&resolution_observer);

  // Connect up a mock socket which will fail when reading.
  MockRead data_reads[] = {
    MockRead(false, ERR_FAILED),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  // Issue a request, containing an HTTP referrer.
  HttpRequestInfo request;
  request.method = "GET";
  request.referrer = referrer;
  request.url = GURL("http://www.google.com/");

  // Run the request until it fails reading from the socket.
  TestCompletionCallback callback;
  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_FAILED, rv);

  // Check that the host resolution observer saw |referrer|.
  EXPECT_TRUE(resolution_observer.did_complete_with_expected_referrer());
}

// Make sure that when the load flags contain LOAD_BYPASS_CACHE, the resolver's
// host cache is bypassed.
TEST_F(HttpNetworkTransactionTest, BypassHostCacheOnRefresh) {
  SessionDependencies session_deps;

  // Select a host resolver that does caching.
  session_deps.host_resolver = new MockCachingHostResolver;

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(
      CreateSession(&session_deps)));

  // Warm up the host cache so it has an entry for "www.google.com" (by doing
  // a synchronous lookup.)
  AddressList addrlist;
  int rv = session_deps.host_resolver->Resolve(
      HostResolver::RequestInfo("www.google.com", 80), &addrlist,
      NULL, NULL, NULL);
  EXPECT_EQ(OK, rv);

  // Verify that it was added to host cache, by doing a subsequent async lookup
  // and confirming it completes synchronously.
  TestCompletionCallback resolve_callback;
  rv = session_deps.host_resolver->Resolve(
      HostResolver::RequestInfo("www.google.com", 80), &addrlist,
      &resolve_callback, NULL, NULL);
  ASSERT_EQ(OK, rv);

  // Inject a failure the next time that "www.google.com" is resolved. This way
  // we can tell if the next lookup hit the cache, or the "network".
  // (cache --> success, "network" --> failure).
  session_deps.host_resolver->rules()->AddSimulatedFailure("www.google.com");

  // Connect up a mock socket which will fail with ERR_UNEXPECTED during the
  // first read -- this won't be reached as the host resolution will fail first.
  MockRead data_reads[] = { MockRead(false, ERR_UNEXPECTED) };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  // Issue a request, asking to bypass the cache(s).
  HttpRequestInfo request;
  request.method = "GET";
  request.load_flags = LOAD_BYPASS_CACHE;
  request.url = GURL("http://www.google.com/");

  // Run the request.
  TestCompletionCallback callback;
  rv = trans->Start(&request, &callback, NULL);
  ASSERT_EQ(ERR_IO_PENDING, rv);
  rv = callback.WaitForResult();

  // If we bypassed the cache, we would have gotten a failure while resolving
  // "www.google.com".
  EXPECT_EQ(ERR_NAME_NOT_RESOLVED, rv);
}

// Make sure we can handle an error when writing the request.
TEST_F(HttpNetworkTransactionTest, RequestWriteError) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockWrite write_failure[] = {
    MockWrite(true, ERR_CONNECTION_RESET),
  };
  StaticSocketDataProvider data(NULL, 0,
                                write_failure, arraysize(write_failure));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_CONNECTION_RESET, rv);
}

// Check that a connection closed after the start of the headers finishes ok.
TEST_F(HttpNetworkTransactionTest, ConnectionClosedAfterStartOfHeaders) {
  SessionDependencies session_deps;
  scoped_refptr<HttpNetworkSession> session = CreateSession(&session_deps);

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.foo.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1."),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("", response_data);
}

// Make sure that a dropped connection while draining the body for auth
// restart does the right thing.
TEST_F(HttpNetworkTransactionTest, DrainResetOK) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n\r\n"),
  };

  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 14\r\n\r\n"),
    MockRead("Unauth"),
    MockRead(true, ERR_CONNECTION_RESET),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  session_deps.socket_factory.AddSocketDataProvider(&data1);

  // After calling trans->RestartWithAuth(), this is the request we should
  // be issuing -- the final header line contains the credentials.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
  };

  // Lastly, the server responds with the actual content.
  MockRead data_reads2[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
    MockRead("Content-Length: 100\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  session_deps.socket_factory.AddSocketDataProvider(&data2);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_FALSE(response->auth_challenge.get() == NULL);

  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(100, response->headers->GetContentLength());
}

// Test HTTPS connections going through a proxy that sends extra data.
TEST_F(HttpNetworkTransactionTest, HTTPSViaProxyWithExtraData) {
  SessionDependencies session_deps(CreateFixedProxyService("myproxy:70"));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("https://www.google.com/");
  request.load_flags = 0;

  MockRead proxy_reads[] = {
    MockRead("HTTP/1.0 200 Connected\r\n\r\nExtra data"),
    MockRead(false, OK)
  };

  StaticSocketDataProvider data(proxy_reads, arraysize(proxy_reads), NULL, 0);
  SSLSocketDataProvider ssl(true, OK);

  session_deps.socket_factory.AddSocketDataProvider(&data);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  TestCompletionCallback callback;

  session_deps.socket_factory.ResetNextMockIndexes();

  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(ERR_TUNNEL_CONNECTION_FAILED, rv);
}

TEST_F(HttpNetworkTransactionTest, LargeContentLengthThenClose) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\nContent-Length:6719476739\r\n\r\n"),
    MockRead(false, OK),
  };

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(ERR_CONNECTION_CLOSED, rv);
}

TEST_F(HttpNetworkTransactionTest, UploadFileSmallerThanLength) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.google.com/upload");
  request.upload_data = new UploadData;
  request.load_flags = 0;

  FilePath temp_file_path;
  ASSERT_TRUE(file_util::CreateTemporaryFile(&temp_file_path));
  const uint64 kFakeSize = 100000;  // file is actually blank

  std::vector<UploadData::Element> elements;
  UploadData::Element element;
  element.SetToFilePath(temp_file_path);
  element.SetContentLength(kFakeSize);
  elements.push_back(element);
  request.upload_data->set_elements(elements);
  EXPECT_EQ(kFakeSize, request.upload_data->GetContentLength());

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);

  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  rv = ReadTransaction(trans.get(), &response_data);
  EXPECT_EQ(OK, rv);
  EXPECT_EQ("hello world", response_data);

  file_util::Delete(temp_file_path, false);
}

TEST_F(HttpNetworkTransactionTest, UploadUnreadableFile) {
  // If we try to upload an unreadable file, the network stack should report
  // the file size as zero and upload zero bytes for that file.
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  FilePath temp_file;
  ASSERT_TRUE(file_util::CreateTemporaryFile(&temp_file));
  std::string temp_file_content("Unreadable file.");
  ASSERT_TRUE(file_util::WriteFile(temp_file, temp_file_content.c_str(),
                                   temp_file_content.length()));
  ASSERT_TRUE(file_util::MakeFileUnreadable(temp_file));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.google.com/upload");
  request.upload_data = new UploadData;
  request.load_flags = 0;

  std::vector<UploadData::Element> elements;
  UploadData::Element element;
  element.SetToFilePath(temp_file);
  elements.push_back(element);
  request.upload_data->set_elements(elements);

  MockRead data_reads[] = {
    MockRead("HTTP/1.0 200 OK\r\n\r\n"),
    MockRead(false, OK),
  };
  MockWrite data_writes[] = {
    MockWrite("POST /upload HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 0\r\n\r\n"),
    MockWrite(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), data_writes,
                                arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);
  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.0 200 OK", response->headers->GetStatusLine());

  file_util::Delete(temp_file, false);
}

TEST_F(HttpNetworkTransactionTest, UnreadableUploadFileAfterAuthRestart) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  FilePath temp_file;
  ASSERT_TRUE(file_util::CreateTemporaryFile(&temp_file));
  std::string temp_file_contents("Unreadable file.");
  std::string unreadable_contents(temp_file_contents.length(), '\0');
  ASSERT_TRUE(file_util::WriteFile(temp_file, temp_file_contents.c_str(),
                                   temp_file_contents.length()));

  HttpRequestInfo request;
  request.method = "POST";
  request.url = GURL("http://www.google.com/upload");
  request.upload_data = new UploadData;
  request.load_flags = 0;

  std::vector<UploadData::Element> elements;
  UploadData::Element element;
  element.SetToFilePath(temp_file);
  elements.push_back(element);
  request.upload_data->set_elements(elements);

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"),
    MockRead("WWW-Authenticate: Basic realm=\"MyRealm1\"\r\n"),
    MockRead("Content-Length: 0\r\n\r\n"),  // No response body.

    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Content-Length: 0\r\n\r\n"),
    MockRead(false, OK),
  };
  MockWrite data_writes[] = {
    MockWrite("POST /upload HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 16\r\n\r\n"),
    MockWrite(false, temp_file_contents.c_str()),

    MockWrite("POST /upload HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Content-Length: 16\r\n"
              "Authorization: Basic Zm9vOmJhcg==\r\n\r\n"),
    MockWrite(false, unreadable_contents.c_str(), temp_file_contents.length()),
    MockWrite(false, OK),
  };
  StaticSocketDataProvider data(data_reads, arraysize(data_reads), data_writes,
                                arraysize(data_writes));
  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback1;

  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);

  const HttpResponseInfo* response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);
  EXPECT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 401 Unauthorized", response->headers->GetStatusLine());

  // The password prompt info should have been set in response->auth_challenge.
  EXPECT_TRUE(response->auth_challenge.get() != NULL);
  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"MyRealm1", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Now make the file unreadable and try again.
  ASSERT_TRUE(file_util::MakeFileUnreadable(temp_file));

  TestCompletionCallback callback2;

  rv = trans->RestartWithAuth(L"foo", L"bar", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);

  response = trans->GetResponseInfo();
  EXPECT_TRUE(response != NULL);
  EXPECT_TRUE(response->headers != NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  file_util::Delete(temp_file, false);
}

// Tests that changes to Auth realms are treated like auth rejections.
TEST_F(HttpNetworkTransactionTest, ChangeAuthRealms) {
  SessionDependencies session_deps;
  scoped_ptr<HttpTransaction> trans(
      new HttpNetworkTransaction(CreateSession(&session_deps)));

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  // First transaction will request a resource and receive a Basic challenge
  // with realm="first_realm".
  MockWrite data_writes1[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "\r\n"),
  };
  MockRead data_reads1[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"
             "WWW-Authenticate: Basic realm=\"first_realm\"\r\n"
             "\r\n"),
  };

  // After calling trans->RestartWithAuth(), provide an Authentication header
  // for first_realm. The server will reject and provide a challenge with
  // second_realm.
  MockWrite data_writes2[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zmlyc3Q6YmF6\r\n"
              "\r\n"),
  };
  MockRead data_reads2[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"
             "WWW-Authenticate: Basic realm=\"second_realm\"\r\n"
             "\r\n"),
  };

  // This again fails, and goes back to first_realm. Make sure that the
  // entry is removed from cache.
  MockWrite data_writes3[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic c2Vjb25kOmZvdQ==\r\n"
              "\r\n"),
  };
  MockRead data_reads3[] = {
    MockRead("HTTP/1.1 401 Unauthorized\r\n"
             "WWW-Authenticate: Basic realm=\"first_realm\"\r\n"
             "\r\n"),
  };

  // Try one last time (with the correct password) and get the resource.
  MockWrite data_writes4[] = {
    MockWrite("GET / HTTP/1.1\r\n"
              "Host: www.google.com\r\n"
              "Connection: keep-alive\r\n"
              "Authorization: Basic Zmlyc3Q6YmFy\r\n"
              "\r\n"),
  };
  MockRead data_reads4[] = {
    MockRead("HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=iso-8859-1\r\n"
             "Content-Length: 100\r\n"
             "\r\n"),
  };

  StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                 data_writes1, arraysize(data_writes1));
  StaticSocketDataProvider data2(data_reads2, arraysize(data_reads2),
                                 data_writes2, arraysize(data_writes2));
  StaticSocketDataProvider data3(data_reads3, arraysize(data_reads3),
                                 data_writes3, arraysize(data_writes3));
  StaticSocketDataProvider data4(data_reads4, arraysize(data_reads4),
                                 data_writes4, arraysize(data_writes4));
  session_deps.socket_factory.AddSocketDataProvider(&data1);
  session_deps.socket_factory.AddSocketDataProvider(&data2);
  session_deps.socket_factory.AddSocketDataProvider(&data3);
  session_deps.socket_factory.AddSocketDataProvider(&data4);

  TestCompletionCallback callback1;

  // Issue the first request with Authorize headers. There should be a
  // password prompt for first_realm waiting to be filled in after the
  // transaction completes.
  int rv = trans->Start(&request, &callback1, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback1.WaitForResult();
  EXPECT_EQ(OK, rv);
  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  ASSERT_FALSE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"first_realm", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Issue the second request with an incorrect password. There should be a
  // password prompt for second_realm waiting to be filled in after the
  // transaction completes.
  TestCompletionCallback callback2;
  rv = trans->RestartWithAuth(L"first", L"baz", &callback2);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback2.WaitForResult();
  EXPECT_EQ(OK, rv);
  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  ASSERT_FALSE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"second_realm", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Issue the third request with another incorrect password. There should be
  // a password prompt for first_realm waiting to be filled in. If the password
  // prompt is not present, it indicates that the HttpAuthCacheEntry for
  // first_realm was not correctly removed.
  TestCompletionCallback callback3;
  rv = trans->RestartWithAuth(L"second", L"fou", &callback3);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback3.WaitForResult();
  EXPECT_EQ(OK, rv);
  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  ASSERT_FALSE(response->auth_challenge.get() == NULL);
  EXPECT_EQ(L"www.google.com:80", response->auth_challenge->host_and_port);
  EXPECT_EQ(L"first_realm", response->auth_challenge->realm);
  EXPECT_EQ(L"basic", response->auth_challenge->scheme);

  // Issue the fourth request with the correct password and username.
  TestCompletionCallback callback4;
  rv = trans->RestartWithAuth(L"first", L"bar", &callback4);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  rv = callback4.WaitForResult();
  EXPECT_EQ(OK, rv);
  response = trans->GetResponseInfo();
  ASSERT_FALSE(response == NULL);
  EXPECT_TRUE(response->auth_challenge.get() == NULL);
}

TEST_F(HttpNetworkTransactionTest, HonorAlternateProtocolHeader) {
  HttpNetworkTransaction::SetNextProtos("needs_to_be_set_for_this_test");
  HttpNetworkTransaction::SetUseAlternateProtocols(true);

  SessionDependencies session_deps;

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n"),
    MockRead("Alternate-Protocol: 443:npn-spdy/1\r\n\r\n"),
    MockRead("hello world"),
    MockRead(false, OK),
  };

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);

  session_deps.socket_factory.AddSocketDataProvider(&data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);

  HostPortPair http_host_port_pair;
  http_host_port_pair.host = "www.google.com";
  http_host_port_pair.port = 80;
  const HttpAlternateProtocols& alternate_protocols =
      session->alternate_protocols();
  EXPECT_FALSE(
      alternate_protocols.HasAlternateProtocolFor(http_host_port_pair));

  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);

  ASSERT_TRUE(alternate_protocols.HasAlternateProtocolFor(http_host_port_pair));
  const HttpAlternateProtocols::PortProtocolPair alternate =
      alternate_protocols.GetAlternateProtocolFor(http_host_port_pair);
  HttpAlternateProtocols::PortProtocolPair expected_alternate;
  expected_alternate.port = 443;
  expected_alternate.protocol = HttpAlternateProtocols::NPN_SPDY_1;
  EXPECT_TRUE(expected_alternate.Equals(alternate));

  HttpNetworkTransaction::SetUseAlternateProtocols(false);
  HttpNetworkTransaction::SetNextProtos("");
}

TEST_F(HttpNetworkTransactionTest, MarkBrokenAlternateProtocol) {
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  SessionDependencies session_deps;

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  MockConnect mock_connect(true, ERR_CONNECTION_REFUSED);
  StaticSocketDataProvider first_data;
  first_data.set_connect_data(mock_connect);
  session_deps.socket_factory.AddSocketDataProvider(&first_data);

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(true, OK),
  };
  StaticSocketDataProvider second_data(
      data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&second_data);

  // TODO(willchan): Delete this extra data provider.  It's necessary due to a
  // ClientSocketPoolBaseHelper bug that starts up too many ConnectJobs:
  // http://crbug.com/37454.
  session_deps.socket_factory.AddSocketDataProvider(&second_data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  HostPortPair http_host_port_pair;
  http_host_port_pair.host = "www.google.com";
  http_host_port_pair.port = 80;
  HttpAlternateProtocols* alternate_protocols =
      session->mutable_alternate_protocols();
  alternate_protocols->SetAlternateProtocolFor(
      http_host_port_pair, 1234 /* port is ignored by MockConnect anyway */,
      HttpAlternateProtocols::NPN_SPDY_1);

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);

  ASSERT_TRUE(
      alternate_protocols->HasAlternateProtocolFor(http_host_port_pair));
  const HttpAlternateProtocols::PortProtocolPair alternate =
      alternate_protocols->GetAlternateProtocolFor(http_host_port_pair);
  EXPECT_EQ(HttpAlternateProtocols::BROKEN, alternate.protocol);
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

// TODO(willchan): Redo this test to use TLS/NPN=>SPDY.  Currently, the code
// says that it does SPDY, but it just does the TLS handshake, but the NPN
// response does not indicate SPDY, so we just do standard HTTPS over the port.
// We should add code such that we don't fallback to HTTPS, but fallback to HTTP
// on the original port.
//  TEST_F(HttpNetworkTransactionTest, UseAlternateProtocol) {
//    SessionDependencies session_deps;
//
//    HttpRequestInfo request;
//    request.method = "GET";
//    request.url = GURL("http://www.google.com/");
//    request.load_flags = 0;
//
//    MockRead data_reads[] = {
//      MockRead("HTTP/1.1 200 OK\r\n\r\n"),
//      MockRead("hello world"),
//      MockRead(true, OK),
//    };
//    StaticSocketDataProvider data(data_reads, arraysize(data_reads), NULL, 0);
//    session_deps.socket_factory.AddSocketDataProvider(&data);
//
//    SSLSocketDataProvider ssl(true, OK);
//    session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);
//
//    TestCompletionCallback callback;
//
//    scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));
//
//    HostPortPair http_host_port_pair;
//    http_host_port_pair.host = "www.google.com";
//    http_host_port_pair.port = 80;
//    HttpAlternateProtocols* alternate_protocols =
//        session->mutable_alternate_protocols();
//    alternate_protocols->SetAlternateProtocolFor(
//        http_host_port_pair, 1234 /* port is ignored */,
//        HttpAlternateProtocols::NPN_SPDY_1);
//
//    scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));
//
//    int rv = trans->Start(&request, &callback, NULL);
//    EXPECT_EQ(ERR_IO_PENDING, rv);
//    EXPECT_EQ(OK, callback.WaitForResult());
//
//    const HttpResponseInfo* response = trans->GetResponseInfo();
//    ASSERT_TRUE(response != NULL);
//    ASSERT_TRUE(response->headers != NULL);
//    EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());
//
//    std::string response_data;
//    ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
//    EXPECT_EQ("hello world", response_data);
//  }

TEST_F(HttpNetworkTransactionTest, FailNpnSpdyAndFallback) {
  HttpNetworkTransaction::SetUseAlternateProtocols(true);
  SessionDependencies session_deps;

  HttpRequestInfo request;
  request.method = "GET";
  request.url = GURL("http://www.google.com/");
  request.load_flags = 0;

  StaticSocketDataProvider first_tcp_connect;
  session_deps.socket_factory.AddSocketDataProvider(&first_tcp_connect);

  SSLSocketDataProvider ssl(true, OK);
  session_deps.socket_factory.AddSSLSocketDataProvider(&ssl);

  MockRead data_reads[] = {
    MockRead("HTTP/1.1 200 OK\r\n\r\n"),
    MockRead("hello world"),
    MockRead(true, OK),
  };
  StaticSocketDataProvider fallback_data(
      data_reads, arraysize(data_reads), NULL, 0);
  session_deps.socket_factory.AddSocketDataProvider(&fallback_data);

  TestCompletionCallback callback;

  scoped_refptr<HttpNetworkSession> session(CreateSession(&session_deps));

  HostPortPair http_host_port_pair;
  http_host_port_pair.host = "www.google.com";
  http_host_port_pair.port = 80;
  HttpAlternateProtocols* alternate_protocols =
      session->mutable_alternate_protocols();
  alternate_protocols->SetAlternateProtocolFor(
      http_host_port_pair, 1234 /* port is ignored */,
      HttpAlternateProtocols::NPN_SPDY_1);

  scoped_ptr<HttpTransaction> trans(new HttpNetworkTransaction(session));

  int rv = trans->Start(&request, &callback, NULL);
  EXPECT_EQ(ERR_IO_PENDING, rv);
  EXPECT_EQ(OK, callback.WaitForResult());

  const HttpResponseInfo* response = trans->GetResponseInfo();
  ASSERT_TRUE(response != NULL);
  ASSERT_TRUE(response->headers != NULL);
  EXPECT_EQ("HTTP/1.1 200 OK", response->headers->GetStatusLine());

  std::string response_data;
  ASSERT_EQ(OK, ReadTransaction(trans.get(), &response_data));
  EXPECT_EQ("hello world", response_data);
  HttpNetworkTransaction::SetUseAlternateProtocols(false);
}

// MockAuthHandlerCanonical is used by the ResolveCanonicalName
// HttpNetworkTransaction unit test below. Callers set up expectations for
// whether the canonical name needs to be resolved.
class MockAuthHandlerCanonical : public HttpAuthHandler {
 public:
  enum Resolve {
    RESOLVE_INIT,
    RESOLVE_SKIP,
    RESOLVE_SYNC,
    RESOLVE_ASYNC,
    RESOLVE_TESTED,
  };

  MockAuthHandlerCanonical() : resolve_(RESOLVE_INIT), user_callback_(NULL) {}
  virtual ~MockAuthHandlerCanonical() {}

  void SetResolveExpectation(Resolve resolve) {
    EXPECT_EQ(RESOLVE_INIT, resolve_);
    resolve_ = resolve;
  }

  void ResetResolveExpectation() {
    EXPECT_EQ(RESOLVE_TESTED, resolve_);
    resolve_ = RESOLVE_INIT;
  }

  virtual bool NeedsCanonicalName() {
    switch (resolve_) {
      case RESOLVE_SYNC:
      case RESOLVE_ASYNC:
        return true;
      case RESOLVE_SKIP:
        resolve_ = RESOLVE_TESTED;
        return false;
      default:
        NOTREACHED();
        return false;
    }
  }

  virtual int ResolveCanonicalName(HostResolver* host_resolver,
                                   CompletionCallback* callback,
                                   const BoundNetLog& net_log) {
    EXPECT_NE(RESOLVE_TESTED, resolve_);
    int rv = OK;
    switch (resolve_) {
      case RESOLVE_SYNC:
        resolve_ = RESOLVE_TESTED;
        break;
      case RESOLVE_ASYNC:
        EXPECT_TRUE(user_callback_ == NULL);
        rv = ERR_IO_PENDING;
        user_callback_ = callback;
        MessageLoop::current()->PostTask(
            FROM_HERE,
            NewRunnableMethod(
                this, &MockAuthHandlerCanonical::OnResolveCanonicalName));
        break;
      default:
        NOTREACHED();
        break;
    }
    return rv;
  }

  void OnResolveCanonicalName() {
    EXPECT_EQ(RESOLVE_ASYNC, resolve_);
    EXPECT_TRUE(user_callback_ != NULL);
    resolve_ = RESOLVE_TESTED;
    CompletionCallback* callback = user_callback_;
    user_callback_ = NULL;
    callback->Run(OK);
  }

  virtual bool Init(HttpAuth::ChallengeTokenizer* challenge) {
    scheme_ = "mock";
    score_ = 1;
    properties_ = 0;
    return true;
  }

  virtual int GenerateAuthToken(const std::wstring& username,
                                const std::wstring& password,
                                const HttpRequestInfo* request,
                                const ProxyInfo* proxy,
                                std::string* auth_token) {
    auth_token->assign("Mock AUTH myserver.example.com");
    return OK;
  }

  virtual int GenerateDefaultAuthToken(const HttpRequestInfo* request,
                                       const ProxyInfo* proxy,
                                       std::string* auth_token) {
    auth_token->assign("Mock DEFAULT_AUTH myserver.example.com");
    return OK;
  }

  // The Factory class simply returns the same handler each time
  // CreateAuthHandler is called.
  class Factory : public HttpAuthHandlerFactory {
   public:
    Factory() {}
    virtual ~Factory() {}

    void set_mock_handler(MockAuthHandlerCanonical* mock_handler) {
      mock_handler_ = mock_handler;
    }
    MockAuthHandlerCanonical* mock_handler() const {
      return mock_handler_.get();
    }

    virtual int CreateAuthHandler(HttpAuth::ChallengeTokenizer* challenge,
                                  HttpAuth::Target target,
                                  const GURL& origin,
                                  scoped_refptr<HttpAuthHandler>* handler) {
      *handler = mock_handler_;
      return OK;
    }

   private:
    scoped_refptr<MockAuthHandlerCanonical> mock_handler_;
  };

 private:
  Resolve resolve_;
  CompletionCallback* user_callback_;
};

// Tests that ResolveCanonicalName is handled correctly by the
// HttpNetworkTransaction.
TEST_F(HttpNetworkTransactionTest, ResolveCanonicalName) {
  SessionDependencies session_deps;
  scoped_refptr<MockAuthHandlerCanonical> auth_handler(
      new MockAuthHandlerCanonical());
  auth_handler->Init(NULL);
  MockAuthHandlerCanonical::Factory* auth_factory(
      new MockAuthHandlerCanonical::Factory());
  auth_factory->set_mock_handler(auth_handler);
  session_deps.http_auth_handler_factory.reset(auth_factory);

  for (int i = 0; i < 2; ++i) {
    scoped_ptr<HttpTransaction> trans(
        new HttpNetworkTransaction(CreateSession(&session_deps)));

    // Set up expectations for this pass of the test. Many of the EXPECT calls
    // are contained inside the MockAuthHandlerCanonical codebase in response to
    // the expectations.
    MockAuthHandlerCanonical::Resolve resolve = (i == 0) ?
        MockAuthHandlerCanonical::RESOLVE_SYNC :
        MockAuthHandlerCanonical::RESOLVE_ASYNC;
    auth_handler->SetResolveExpectation(resolve);
    HttpRequestInfo request;
    request.method = "GET";
    request.url = GURL("http://myserver/");
    request.load_flags = 0;

    MockWrite data_writes1[] = {
      MockWrite("GET / HTTP/1.1\r\n"
                "Host: myserver\r\n"
                "Connection: keep-alive\r\n\r\n"),
    };

    MockRead data_reads1[] = {
      MockRead("HTTP/1.1 401 Unauthorized\r\n"),
      MockRead("WWW-Authenticate: Mock myserver.example.com\r\n"),
      MockRead("Content-Type: text/html; charset=iso-8859-1\r\n"),
      MockRead("Content-Length: 14\r\n\r\n"),
      MockRead("Unauthorized\r\n"),
    };

    StaticSocketDataProvider data1(data_reads1, arraysize(data_reads1),
                                   data_writes1, arraysize(data_writes1));
    session_deps.socket_factory.AddSocketDataProvider(&data1);

    TestCompletionCallback callback1;

    int rv = trans->Start(&request, &callback1, NULL);
    EXPECT_EQ(ERR_IO_PENDING, rv);

    rv = callback1.WaitForResult();
    EXPECT_EQ(OK, rv);

    const HttpResponseInfo* response = trans->GetResponseInfo();
    EXPECT_FALSE(response == NULL);

    // The password prompt is set after the canonical name is resolved.
    // If it isn't present or is incorrect, it indicates that the scheme
    // did not complete correctly.
    EXPECT_FALSE(response->auth_challenge.get() == NULL);

    EXPECT_EQ(L"myserver:80", response->auth_challenge->host_and_port);
    EXPECT_EQ(L"", response->auth_challenge->realm);
    EXPECT_EQ(L"mock", response->auth_challenge->scheme);
    auth_handler->ResetResolveExpectation();
  }
}

}  // namespace net
