// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_HTTP_HTTP_STREAM_FACTORY_IMPL_H_
#define NET_HTTP_HTTP_STREAM_FACTORY_IMPL_H_

#include <map>
#include <set>

#include "base/memory/ref_counted.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_log.h"
#include "net/http/http_pipelined_host_pool.h"
#include "net/http/http_stream_factory.h"
#include "net/proxy/proxy_server.h"
#include "net/socket/ssl_client_socket.h"

namespace net {

class HttpNetworkSession;
class HttpPipelinedHost;
class SpdySession;

class NET_EXPORT_PRIVATE HttpStreamFactoryImpl :
    public HttpStreamFactory,
    public HttpPipelinedHostPool::Delegate {
 public:
  explicit HttpStreamFactoryImpl(HttpNetworkSession* session);
  virtual ~HttpStreamFactoryImpl();

  // HttpStreamFactory interface
  virtual HttpStreamRequest* RequestStream(
      const HttpRequestInfo& info,
      const SSLConfig& server_ssl_config,
      const SSLConfig& proxy_ssl_config,
      HttpStreamRequest::Delegate* delegate,
      const BoundNetLog& net_log) OVERRIDE;

  virtual void PreconnectStreams(int num_streams,
                                 const HttpRequestInfo& info,
                                 const SSLConfig& server_ssl_config,
                                 const SSLConfig& proxy_ssl_config) OVERRIDE;
  virtual void AddTLSIntolerantServer(const HostPortPair& server) OVERRIDE;
  virtual bool IsTLSIntolerantServer(const HostPortPair& server) const OVERRIDE;
  virtual base::Value* PipelineInfoToValue() const OVERRIDE;

  // HttpPipelinedHostPool::Delegate interface
  virtual void OnHttpPipelinedHostHasAdditionalCapacity(
      HttpPipelinedHost* host) OVERRIDE;

 private:
  class Request;
  class Job;

  typedef std::set<Request*> RequestSet;
  typedef std::map<HostPortProxyPair, RequestSet> SpdySessionRequestMap;
  typedef std::map<HttpPipelinedHost::Key, RequestSet> HttpPipeliningRequestMap;

  bool GetAlternateProtocolRequestFor(const GURL& original_url,
                                      GURL* alternate_url) const;

  // Detaches |job| from |request|.
  void OrphanJob(Job* job, const Request* request);

  // Called when a SpdySession is ready. It will find appropriate Requests and
  // fulfill them. |direct| indicates whether or not |spdy_session| uses a
  // proxy.
  void OnSpdySessionReady(scoped_refptr<SpdySession> spdy_session,
                          bool direct,
                          const SSLConfig& used_ssl_config,
                          const ProxyInfo& used_proxy_info,
                          bool was_npn_negotiated,
                          SSLClientSocket::NextProto protocol_negotiated,
                          bool using_spdy,
                          const BoundNetLog& net_log);

  // Called when the Job detects that the endpoint indicated by the
  // Alternate-Protocol does not work. Lets the factory update
  // HttpAlternateProtocols with the failure and resets the SPDY session key.
  void OnBrokenAlternateProtocol(const Job*, const HostPortPair& origin);

  // Invoked when an orphaned Job finishes.
  void OnOrphanedJobComplete(const Job* job);

  // Invoked when the Job finishes preconnecting sockets.
  void OnPreconnectsComplete(const Job* job);

  // Called when the Preconnect completes. Used for testing.
  virtual void OnPreconnectsCompleteInternal() {}

  void AbortPipelinedRequestsWithKey(const Job* job,
                                     const HttpPipelinedHost::Key& key,
                                     int status,
                                     const SSLConfig& used_ssl_config);

  HttpNetworkSession* const session_;

  std::set<HostPortPair> tls_intolerant_servers_;

  // All Requests are handed out to clients. By the time HttpStreamFactoryImpl
  // is destroyed, all Requests should be deleted (which should remove them from
  // |request_map_|. The Requests will delete the corresponding job.
  std::map<const Job*, Request*> request_map_;

  SpdySessionRequestMap spdy_session_request_map_;
  HttpPipeliningRequestMap http_pipelining_request_map_;

  HttpPipelinedHostPool http_pipelined_host_pool_;

  // These jobs correspond to jobs orphaned by Requests and now owned by
  // HttpStreamFactoryImpl. Since they are no longer tied to Requests, they will
  // not be canceled when Requests are canceled. Therefore, in
  // ~HttpStreamFactoryImpl, it is possible for some jobs to still exist in this
  // set. Leftover jobs will be deleted when the factory is destroyed.
  std::set<const Job*> orphaned_job_set_;

  // These jobs correspond to preconnect requests and have no associated Request
  // object. They're owned by HttpStreamFactoryImpl. Leftover jobs will be
  // deleted when the factory is destroyed.
  std::set<const Job*> preconnect_job_set_;

  DISALLOW_COPY_AND_ASSIGN(HttpStreamFactoryImpl);
};

}  // namespace net

#endif  // NET_HTTP_HTTP_STREAM_FACTORY_IMPL_H_
