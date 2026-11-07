/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/http_download.h"

#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

// iOS has no wx: the downloader's session member is never set here.
class wxWebSessionSync {};

namespace {

constexpr NSTimeInterval kConnectTimeout = 15;
// URLSession's request timeout is the most time between two reads, which ends
// a transfer that stops sending, as wx's stall check does.
constexpr NSTimeInterval kStallTimeout = 30;
constexpr NSTimeInterval kMemoryTimeout = 60;

NSURLSessionConfiguration* MakeConfiguration(NSTimeInterval resource_timeout) {
  NSURLSessionConfiguration* configuration =
      [NSURLSessionConfiguration ephemeralSessionConfiguration];
  configuration.timeoutIntervalForRequest = kStallTimeout;
  configuration.timeoutIntervalForResource = resource_timeout;
  // The body as sent, so the received byte count matches Content-Length.
  configuration.HTTPAdditionalHeaders =
      @{@"User-Agent" : @"xenia-edge", @"Accept-Encoding" : @"identity"};
  return configuration;
}

}  // namespace

// Writes a transfer to a file. Its fields are set on the session's queue and
// read by the thread waiting in DownloadFile.
@interface XeniaFileDownloadDelegate : NSObject <NSURLSessionDataDelegate> {
 @public
  FILE* file_;
  std::atomic<uint64_t> received_;
  std::atomic<uint64_t> size_;
  std::atomic<NSInteger> status_;
  std::atomic<bool> write_failed_;
  std::atomic<bool> ended_;
}
@end

@implementation XeniaFileDownloadDelegate

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
  if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
    status_ = static_cast<NSHTTPURLResponse*>(response).statusCode;
  }
  const long long length = response.expectedContentLength;
  size_ = length > 0 ? uint64_t(length) : 0;
  completion(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data {
  if (write_failed_) {
    return;
  }
  [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
    if (std::fwrite(bytes, 1, range.length, file_) != range.length) {
      write_failed_ = true;
      *stop = YES;
      return;
    }
    received_ += range.length;
  }];
  if (write_failed_) {
    [task cancel];
  }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  if (error && error.code != NSURLErrorCancelled) {
    XELOGE("Download: {}", error.localizedDescription.UTF8String);
  }
  ended_ = true;
}

@end

namespace xe {
namespace ui {

// Blocks the calling thread, which gets |progress| as on the other platforms;
// call it off the main thread.
DownloadResult DownloadFile(
    const std::string& url, const std::filesystem::path& path,
    const std::function<bool(uint64_t received, uint64_t size)>& progress) {
  NSURL* ns_url = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
  if (!ns_url) {
    XELOGE("Download: not a URL");
    return DownloadResult::kFailed;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (!file) {
    XELOGE("Download: failed to create {}", xe::strip_pii(xe::path_to_utf8(path)));
    return DownloadResult::kWriteFailed;
  }

  XeniaFileDownloadDelegate* delegate = [[XeniaFileDownloadDelegate alloc] init];
  delegate->file_ = file;
  NSOperationQueue* queue = [[NSOperationQueue alloc] init];
  queue.maxConcurrentOperationCount = 1;
  NSURLSession* session =
      [NSURLSession sessionWithConfiguration:MakeConfiguration(NSURLSessionConfiguration
                                                                   .defaultSessionConfiguration
                                                                   .timeoutIntervalForResource)
                                    delegate:delegate
                               delegateQueue:queue];
  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:ns_url];
  request.timeoutInterval = kConnectTimeout;
  NSURLSessionDataTask* task = [session dataTaskWithRequest:request];
  [task resume];

  bool cancelled = false;
  bool stopping = false;
  while (!delegate->ended_) {
    if (!stopping && !progress(delegate->received_, delegate->size_)) {
      cancelled = true;
      stopping = true;
      [task cancel];
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  [session finishTasksAndInvalidate];
  // The delegate queue is done with the file once the task has ended.
  [queue waitUntilAllOperationsAreFinished];

  bool write_failed = delegate->write_failed_;
  if (std::fclose(file) != 0 && !write_failed) {
    write_failed = true;
  }
  if (write_failed) {
    XELOGE("Download: failed to write {}", xe::strip_pii(xe::path_to_utf8(path)));
  }
  const NSInteger status = delegate->status_;
  const bool complete = task.state == NSURLSessionTaskStateCompleted && !task.error;
  [delegate release];
  [queue release];

  DownloadResult result = DownloadResult::kFailed;
  if (cancelled) {
    result = DownloadResult::kCancelled;
  } else if (write_failed) {
    result = DownloadResult::kWriteFailed;
  } else if (complete) {
    if (status == 200) {
      result = DownloadResult::kSuccess;
    } else {
      XELOGE("Download: HTTP status {}", status);
    }
  }
  if (result != DownloadResult::kSuccess) {
    std::filesystem::remove(path, ec);
  }
  return result;
}

HttpDownloader::HttpDownloader() = default;

HttpDownloader::~HttpDownloader() = default;

bool HttpDownloader::DownloadToMemory(const std::string& url, std::vector<uint8_t>& out) {
  out.clear();
  NSURL* ns_url = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
  if (!ns_url) {
    XELOGE("Download: not a URL");
    return false;
  }
  NSURLSession* session =
      [NSURLSession sessionWithConfiguration:MakeConfiguration(kMemoryTimeout)];
  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:ns_url];
  request.timeoutInterval = kConnectTimeout;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  __block NSData* body = nil;
  __block NSInteger status = 0;
  NSURLSessionDataTask* task =
      [session dataTaskWithRequest:request
                 completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                   if (error) {
                     XELOGE("Download: {}", error.localizedDescription.UTF8String);
                   } else {
                     body = [data retain];
                     if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                       status = static_cast<NSHTTPURLResponse*>(response).statusCode;
                     }
                   }
                   dispatch_semaphore_signal(done);
                 }];
  [task resume];
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  dispatch_release(done);
  [session finishTasksAndInvalidate];
  bool ok = false;
  if (body) {
    if (status == 200) {
      const auto* bytes = static_cast<const uint8_t*>(body.bytes);
      out.assign(bytes, bytes + body.length);
      ok = true;
    } else {
      XELOGE("Download: HTTP status {}", status);
    }
    [body release];
  }
  return ok;
}

}  // namespace ui
}  // namespace xe
