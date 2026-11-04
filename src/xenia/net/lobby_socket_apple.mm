/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Foundation/Foundation.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "xenia/net/lobby_socket_apple.h"

using xe::net::secure::LobbySocket;

// Hands a lobby WebSocket's events to System Link, until it's stopped. Called
// on the session's queue.
@interface XeniaLobbySocketDelegate : NSObject <NSURLSessionWebSocketDelegate>
- (instancetype)initWithEvents:(LobbySocket::Events)events;
- (void)receiveFrom:(NSURLSessionWebSocketTask*)task;
- (void)stop;
@end

@implementation XeniaLobbySocketDelegate {
  std::mutex mutex_;
  LobbySocket::Events events_;
  bool done_;
}

- (instancetype)initWithEvents:(LobbySocket::Events)events {
  if ((self = [super init])) {
    events_ = std::move(events);
  }
  return self;
}

- (void)stop {
  std::lock_guard lock(mutex_);
  done_ = true;
  events_ = {};
}

- (void)deliver:(std::string)text {
  std::function<void(std::string)> message;
  {
    std::lock_guard lock(mutex_);
    if (done_) {
      return;
    }
    message = events_.message;
  }
  message(std::move(text));
}

// The reason never has the address, which can name a private room.
- (void)finish:(NSString*)reason {
  std::function<void(std::string)> closed;
  {
    std::lock_guard lock(mutex_);
    if (done_) {
      return;
    }
    done_ = true;
    closed = std::move(events_.closed);
    events_ = {};
  }
  closed(reason.UTF8String);
}

- (void)finishWithError:(NSError*)error task:(NSURLSessionTask*)task {
  // The server answered with an HTTP error instead of upgrading: the lobby, or
  // Cloudflare in front of it (such as a 1027 page at the daily limit).
  if ([task.response isKindOfClass:[NSHTTPURLResponse class]]) {
    NSInteger status = ((NSHTTPURLResponse*)task.response).statusCode;
    if (status >= 400) {
      [self finish:[NSString stringWithFormat:@"http %ld", (long)status]];
      return;
    }
  }
  [self finish:[NSString stringWithFormat:@"%@ %ld", error.domain, (long)error.code]];
}

- (void)receiveFrom:(NSURLSessionWebSocketTask*)task {
  [task
      receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message, NSError* error) {
        if (error) {
          [self finishWithError:error task:task];
          return;
        }
        if (message.type == NSURLSessionWebSocketMessageTypeString && message.string) {
          [self deliver:message.string.UTF8String];
        }
        [self receiveFrom:task];
      }];
}

- (void)URLSession:(NSURLSession* __unused)session
          webSocketTask:(NSURLSessionWebSocketTask* __unused)task
    didOpenWithProtocol:(NSString* __unused)protocol {
  std::function<void()> opened;
  {
    std::lock_guard lock(mutex_);
    if (done_) {
      return;
    }
    opened = events_.opened;
  }
  opened();
}

- (void)URLSession:(NSURLSession* __unused)session
       webSocketTask:(NSURLSessionWebSocketTask* __unused)task
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)code
              reason:(NSData* __unused)reason {
  [self finish:[NSString stringWithFormat:@"closed %ld", (long)code]];
}

- (void)URLSession:(NSURLSession* __unused)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  if (error) {
    [self finishWithError:error task:task];
  } else {
    [self finish:@"ended"];
  }
}

@end

namespace xe::net::secure {
namespace {

// A lobby connection over NSURLSession. Nothing is reported after it's
// destroyed.
class AppleLobbySocket final : public LobbySocket {
 public:
  AppleLobbySocket(NSURL* url, Events events) {
    @autoreleasepool {
      delegate_ = [[XeniaLobbySocketDelegate alloc] initWithEvents:std::move(events)];
      NSOperationQueue* queue = [[[NSOperationQueue alloc] init] autorelease];
      queue.maxConcurrentOperationCount = 1;
      NSURLSessionConfiguration* configuration =
          [NSURLSessionConfiguration ephemeralSessionConfiguration];
      // A lobby that can't be reached is reported now, and tried again later.
      configuration.waitsForConnectivity = NO;
      session_ = [[NSURLSession sessionWithConfiguration:configuration
                                                delegate:delegate_
                                           delegateQueue:queue] retain];
      task_ = [[session_ webSocketTaskWithURL:url] retain];
      [task_ resume];
      [delegate_ receiveFrom:task_];
    }
  }

  ~AppleLobbySocket() override {
    @autoreleasepool {
      [delegate_ stop];
      [task_ cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
      // The session lets go of the delegate once it's invalidated.
      [session_ invalidateAndCancel];
      [task_ release];
      [session_ release];
      [delegate_ release];
    }
  }

  void Send(std::string text) override {
    @autoreleasepool {
      NSString* string = [[[NSString alloc] initWithBytes:text.data()
                                                   length:text.size()
                                                 encoding:NSUTF8StringEncoding] autorelease];
      if (!string) {
        return;
      }
      NSURLSessionWebSocketMessage* message =
          [[[NSURLSessionWebSocketMessage alloc] initWithString:string] autorelease];
      // A failed send ends the connection, which the receive reports.
      [task_ sendMessage:message
          completionHandler:^(NSError* __unused error){
          }];
    }
  }

 private:
  XeniaLobbySocketDelegate* delegate_ = nil;
  NSURLSession* session_ = nil;
  NSURLSessionWebSocketTask* task_ = nil;
};

}  // namespace

std::unique_ptr<LobbySocket> OpenAppleLobbySocket(const std::string& url,
                                                  LobbySocket::Events events) {
  @autoreleasepool {
    NSURL* address = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    NSString* scheme = address.scheme.lowercaseString;
    if (![scheme isEqualToString:@"wss"] && ![scheme isEqualToString:@"ws"]) {
      return nullptr;
    }
    return std::make_unique<AppleLobbySocket>(address, std::move(events));
  }
}

}  // namespace xe::net::secure
