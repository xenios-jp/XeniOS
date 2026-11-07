/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Foundation/Foundation.h>

#include "xenia/kernel/xam/xhttp_host.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xhttp {

// NSURLSession, made synchronous. A request goes out once its whole body is
// written, ReceiveResponse waits for the status line and headers, and Read
// takes the body as it arrives.

// WinHTTP's codes, which XHttp uses, beyond those xbox.h names.
constexpr uint32_t kErrorTimeout = 12002;
constexpr uint32_t kErrorNameNotResolved = 12007;
constexpr uint32_t kErrorSecureFailure = 12175;

static uint32_t ErrorFromNSError(NSError* error) {
  if (![error.domain isEqualToString:NSURLErrorDomain]) {
    return X_ERROR_WINHTTP_INTERNAL_ERROR;
  }
  switch (error.code) {
    case NSURLErrorCancelled:
      return X_ERROR_WINHTTP_OPERATION_CANCELLED;
    case NSURLErrorTimedOut:
      return kErrorTimeout;
    case NSURLErrorCannotFindHost:
    case NSURLErrorDNSLookupFailed:
      return kErrorNameNotResolved;
    case NSURLErrorCannotConnectToHost:
    case NSURLErrorNotConnectedToInternet:
      return X_ERROR_WINHTTP_CANNOT_CONNECT;
    case NSURLErrorNetworkConnectionLost:
      return X_ERROR_WINHTTP_CONNECTION_ERROR;
    case NSURLErrorBadURL:
    case NSURLErrorUnsupportedURL:
      return X_ERROR_WINHTTP_INVALID_URL;
    case NSURLErrorSecureConnectionFailed:
    case NSURLErrorServerCertificateHasBadDate:
    case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasUnknownRoot:
    case NSURLErrorServerCertificateNotYetValid:
    case NSURLErrorClientCertificateRejected:
    case NSURLErrorClientCertificateRequired:
      return kErrorSecureFailure;
    default:
      return X_ERROR_WINHTTP_CONNECTION_ERROR;
  }
}

class HostSession {
 public:
  explicit HostSession(std::string user_agent)
      : user_agent_(std::move(user_agent)) {}
  const std::string& user_agent() const { return user_agent_; }

 private:
  std::string user_agent_;
};

class HostConnection {
 public:
  HostConnection(std::shared_ptr<HostSession> session, std::string server,
                 uint16_t port, bool secure)
      : session_(std::move(session)),
        server_(std::move(server)),
        port_(port),
        secure_(secure) {}
  const HostSession& session() const { return *session_; }
  // scheme://host:port, with an IPv6 host in brackets.
  std::string origin() const {
    const bool ipv6 = server_.find(':') != std::string::npos;
    return std::string(secure_ ? "https://" : "http://") +
           (ipv6 ? "[" + server_ + "]" : server_) + ":" +
           std::to_string(port_);
  }

 private:
  std::shared_ptr<HostSession> session_;
  std::string server_;
  uint16_t port_;
  bool secure_;
};

// What the session's delegate fills in for the request waiting on it.
struct RequestState {
  std::mutex mutex;
  std::condition_variable changed;
  NSHTTPURLResponse* response = nil;  // Retained.
  std::deque<uint8_t> data;
  bool complete = false;
  uint32_t error = 0;
  std::vector<std::string> redirects;
  // Copied from the request's options for the delegate.
  uint32_t max_redirects = 0;
  uint32_t redirect_policy = 3;
  bool ignore_certificate_errors = false;
  Credentials credentials;
  bool credentials_tried = false;
};

}  // namespace xhttp
}  // namespace xam
}  // namespace kernel
}  // namespace xe

using xe::kernel::xam::xhttp::RequestState;

@interface XeXHttpDelegate : NSObject <NSURLSessionDataDelegate> {
 @public
  std::shared_ptr<RequestState> state_;
}
@end

@implementation XeXHttpDelegate

- (void)URLSession:(NSURLSession*)session
                    dataTask:(NSURLSessionDataTask*)task
          didReceiveResponse:(NSURLResponse*)response
           completionHandler:
               (void (^)(NSURLSessionResponseDisposition))completion {
  {
    std::lock_guard lock(state_->mutex);
    if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
      [state_->response release];
      state_->response = [(NSHTTPURLResponse*)response retain];
    }
  }
  state_->changed.notify_all();
  completion(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data {
  {
    std::lock_guard lock(state_->mutex);
    const auto* bytes = static_cast<const uint8_t*>(data.bytes);
    state_->data.insert(state_->data.end(), bytes, bytes + data.length);
  }
  state_->changed.notify_all();
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  {
    std::lock_guard lock(state_->mutex);
    state_->complete = true;
    if (error) {
      state_->error = xe::kernel::xam::xhttp::ErrorFromNSError(error);
    }
  }
  state_->changed.notify_all();
}

- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completion {
  std::lock_guard lock(state_->mutex);
  NSString* from = task.currentRequest.URL.scheme.lowercaseString;
  NSString* to = request.URL.scheme.lowercaseString;
  bool allowed;
  switch (state_->redirect_policy) {
    case 0:
      allowed = false;
      break;
    case 1:
      allowed = !([from isEqualToString:@"https"] &&
                  [to isEqualToString:@"http"]);
      break;
    case 2:
      allowed = true;
      break;
    default:
      allowed = [from isEqualToString:to];
      break;
  }
  if (!allowed || state_->redirects.size() >= state_->max_redirects) {
    // The redirect response is the response.
    completion(nil);
    return;
  }
  state_->redirects.push_back(request.URL.absoluteString.UTF8String);
  completion(request);
}

- (void)URLSession:(NSURLSession*)session
                   task:(NSURLSessionTask*)task
    didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
      completionHandler:
          (void (^)(NSURLSessionAuthChallengeDisposition,
                    NSURLCredential*))completion {
  NSString* method = challenge.protectionSpace.authenticationMethod;
  std::lock_guard lock(state_->mutex);
  if ([method isEqualToString:NSURLAuthenticationMethodServerTrust]) {
    if (state_->ignore_certificate_errors) {
      completion(NSURLSessionAuthChallengeUseCredential,
                 [NSURLCredential
                     credentialForTrust:challenge.protectionSpace.serverTrust]);
    } else {
      completion(NSURLSessionAuthChallengePerformDefaultHandling, nil);
    }
    return;
  }
  const bool basic = [method isEqualToString:NSURLAuthenticationMethodHTTPBasic];
  const bool digest =
      [method isEqualToString:NSURLAuthenticationMethodHTTPDigest];
  const auto& credentials = state_->credentials;
  // Once only: a refused login comes back to the title as the 401.
  if (credentials.scheme && !state_->credentials_tried &&
      (credentials.scheme == 1 ? basic : digest)) {
    state_->credentials_tried = true;
    completion(NSURLSessionAuthChallengeUseCredential,
               [NSURLCredential
                   credentialWithUser:@(credentials.user_name.c_str())
                             password:@(credentials.password.c_str())
                          persistence:NSURLCredentialPersistenceNone]);
    return;
  }
  completion(NSURLSessionAuthChallengeRejectProtectionSpace, nil);
}

@end

namespace xe {
namespace kernel {
namespace xam {
namespace xhttp {

class AppleRequest : public HostRequest {
 public:
  AppleRequest(std::shared_ptr<HostConnection> connection,
               RequestConfig config)
      : connection_(std::move(connection)), config_(std::move(config)) {}

  ~AppleRequest() override {
    Cancel();
    std::lock_guard lock(state_->mutex);
    [state_->response release];
    state_->response = nil;
  }

  uint32_t Send(const RequestOptions& options, const std::string& headers,
                std::span<const uint8_t> body,
                uint32_t total_length) override {
    // Sending again, as for an authentication challenge, starts over.
    Cancel();
    auto state = std::make_shared<RequestState>();
    state->max_redirects = options.max_redirects;
    state->redirect_policy = options.redirect_policy;
    state->ignore_certificate_errors = options.ignore_certificate_errors;
    state->credentials = options.credentials;
    std::shared_ptr<RequestState> old_state;
    {
      std::lock_guard lock(task_mutex_);
      old_state = std::exchange(state_, std::move(state));
    }
    {
      std::lock_guard lock(old_state->mutex);
      [old_state->response release];
      old_state->response = nil;
    }
    options_ = options;
    headers_ = headers;
    body_.assign(body.begin(), body.end());
    total_length_ = std::max<uint32_t>(total_length, uint32_t(body.size()));
    return 0;
  }

  uint32_t Write(std::span<const uint8_t> data) override {
    if (body_.size() + data.size() > total_length_) {
      return X_ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    }
    body_.insert(body_.end(), data.begin(), data.end());
    return 0;
  }

  uint32_t ReceiveResponse() override {
    if (uint32_t error = Start()) {
      return error;
    }
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, [&] {
      return state_->response != nil || state_->complete;
    });
    if (state_->response == nil) {
      return state_->error ? state_->error : X_ERROR_WINHTTP_CONNECTION_ERROR;
    }
    return 0;
  }

  uint32_t Read(std::span<uint8_t> buffer, uint32_t* bytes_read) override {
    *bytes_read = 0;
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, [&] {
      return !state_->data.empty() || state_->complete;
    });
    const size_t count = std::min(buffer.size(), state_->data.size());
    std::copy_n(state_->data.begin(), count, buffer.begin());
    state_->data.erase(state_->data.begin(), state_->data.begin() + count);
    *bytes_read = uint32_t(count);
    if (!count && state_->error) {
      return state_->error;
    }
    return 0;
  }

  std::string response_headers() override {
    std::lock_guard lock(state_->mutex);
    NSHTTPURLResponse* response = state_->response;
    if (!response) {
      return {};
    }
    const NSInteger status = response.statusCode;
    std::string text =
        "HTTP/1.1 " + std::to_string(status) + " " +
        [NSHTTPURLResponse localizedStringForStatusCode:status].UTF8String +
        "\r\n";
    for (NSString* name in response.allHeaderFields) {
      NSString* value = response.allHeaderFields[name];
      text += std::string(name.UTF8String) + ": " + value.UTF8String + "\r\n";
    }
    return text + "\r\n";
  }

  std::vector<std::string> TakeRedirects() override {
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->redirects, {});
  }

  void Abort() override {
    NSURLSessionDataTask* task;
    std::shared_ptr<RequestState> state;
    {
      std::lock_guard lock(task_mutex_);
      aborted_ = true;
      task = [task_ retain];
      state = state_;
    }
    [task cancel];
    [task release];
    // A call waiting before the task started has nothing to cancel.
    {
      std::lock_guard lock(state->mutex);
      if (!state->complete) {
        state->complete = true;
        state->error = X_ERROR_WINHTTP_OPERATION_CANCELLED;
      }
    }
    state->changed.notify_all();
  }

 private:
  uint32_t Start() {
    @autoreleasepool {
      NSURL* url = [NSURL
          URLWithString:@((connection_->origin() + config_.object_name)
                              .c_str())];
      if (!url) {
        return X_ERROR_WINHTTP_INVALID_URL;
      }
      NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
      request.HTTPMethod =
          @(config_.verb.empty() ? "GET" : config_.verb.c_str());
      request.timeoutInterval = options_.receive_timeout_ms / 1000.0;
      request.HTTPShouldHandleCookies = NO;
      const std::string& user_agent = connection_->session().user_agent();
      if (!user_agent.empty()) {
        [request setValue:@(user_agent.c_str()) forHTTPHeaderField:@"User-Agent"];
      }
      if (!config_.referrer.empty()) {
        [request setValue:@(config_.referrer.c_str())
            forHTTPHeaderField:@"Referer"];
      }
      if (!options_.keep_alive) {
        [request setValue:@"close" forHTTPHeaderField:@"Connection"];
      }
      // NSURLSession would ask for a compressed body and decode it, leaving a
      // Content-Encoding header that no longer describes the body. The
      // console doesn't ask, so neither does this, unless the title does.
      // TODO(xenios-jp): A title's own Accept-Encoding still gets the body
      // decoded.
      [request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
      // One "Name: value" per CRLF-separated line.
      size_t begin = 0;
      while (begin < headers_.size()) {
        size_t end = headers_.find("\r\n", begin);
        if (end == std::string::npos) {
          end = headers_.size();
        }
        const std::string line = headers_.substr(begin, end - begin);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
          const size_t value = line.find_first_not_of(' ', colon + 1);
          [request setValue:@(value == std::string::npos
                                  ? ""
                                  : line.substr(value).c_str())
              forHTTPHeaderField:@(line.substr(0, colon).c_str())];
        }
        begin = end + 2;
      }
      if (!body_.empty()) {
        request.HTTPBody = [NSData dataWithBytes:body_.data()
                                          length:body_.size()];
      }

      NSURLSessionConfiguration* configuration =
          [NSURLSessionConfiguration ephemeralSessionConfiguration];
      configuration.HTTPShouldUsePipelining = NO;
      configuration.URLCache = nil;
      XeXHttpDelegate* delegate = [[[XeXHttpDelegate alloc] init] autorelease];
      delegate->state_ = state_;
      NSOperationQueue* queue = [[[NSOperationQueue alloc] init] autorelease];
      queue.maxConcurrentOperationCount = 1;
      std::lock_guard lock(task_mutex_);
      if (aborted_) {
        return X_ERROR_WINHTTP_OPERATION_CANCELLED;
      }
      // The session keeps its delegate until it's invalidated.
      session_ = [[NSURLSession sessionWithConfiguration:configuration
                                                delegate:delegate
                                           delegateQueue:queue] retain];
      task_ = [[session_ dataTaskWithRequest:request] retain];
      [task_ resume];
    }
    return 0;
  }

  void Cancel() {
    std::lock_guard lock(task_mutex_);
    [task_ cancel];
    [task_ release];
    task_ = nil;
    [session_ invalidateAndCancel];
    [session_ release];
    session_ = nil;
  }

  std::shared_ptr<HostConnection> connection_;
  RequestConfig config_;
  RequestOptions options_;
  std::string headers_;
  std::vector<uint8_t> body_;
  uint32_t total_length_ = 0;
  std::shared_ptr<RequestState> state_ = std::make_shared<RequestState>();

  std::mutex task_mutex_;
  bool aborted_ = false;
  NSURLSession* session_ = nil;
  NSURLSessionDataTask* task_ = nil;
};

std::shared_ptr<HostSession> OpenSession(const SessionConfig& config,
                                         uint32_t* error) {
  // NSURLSession uses the system's proxy settings.
  if (!config.proxy.empty()) {
    XELOGW("XHttp: a named proxy isn't supported here, using the system's");
  }
  *error = 0;
  return std::make_shared<HostSession>(config.user_agent);
}

std::shared_ptr<HostConnection> Connect(
    const std::shared_ptr<HostSession>& session, const std::string& server,
    uint16_t port, bool secure, uint32_t* error) {
  *error = 0;
  return std::make_shared<HostConnection>(session, server, port, secure);
}

std::unique_ptr<HostRequest> OpenRequest(
    const std::shared_ptr<HostConnection>& connection,
    const RequestConfig& config, uint32_t* error) {
  *error = 0;
  return std::make_unique<AppleRequest>(connection, config);
}

}  // namespace xhttp
}  // namespace xam
}  // namespace kernel
}  // namespace xe
