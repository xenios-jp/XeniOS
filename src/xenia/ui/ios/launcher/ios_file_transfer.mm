/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_file_transfer.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <system_error>

#include "xenia/base/logging.h"
#include "xenia/ui/ios/shared/ios_idle_timer.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

NSString* const XeniaIOSFileTransferErrorDomain = @"XeniaIOSFileTransfer";

namespace {

constexpr size_t kCopyBufferSize = 4 * 1024 * 1024;
// Left free after an import so saves, shader caches and logs can still be
// written.
constexpr uint64_t kFreeSpaceReserveBytes = 64ull * 1024 * 1024;
constexpr CFTimeInterval kProgressIntervalSeconds = 0.1;

std::mutex g_active_part_mutex;
std::set<std::string>& ActivePartPaths() {
  static std::set<std::string>* paths = new std::set<std::string>();
  return *paths;
}

NSError* TransferError(NSInteger code, NSString* message) {
  return [NSError errorWithDomain:XeniaIOSFileTransferErrorDomain
                             code:code
                         userInfo:@{NSLocalizedDescriptionKey : message}];
}

NSError* CancelledError() {
  return TransferError(XeniaIOSFileTransferErrorCancelled,
                       NSLocalizedString(@"Cancelled.", @"file transfer: error message"));
}

// "<path>.<8 hex>.part": unique per writer, so two writers of the same final name (an import and
// a ZAR conversion, say) never share or delete each other's in-progress file.
NSString* PartPath(NSString* path) {
  NSString* token = [[[NSUUID UUID] UUIDString] substringToIndex:8];
  return [NSString stringWithFormat:@"%@.%@.part", path, token];
}

void RemovePath(NSString* path) {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path([path UTF8String]), ec);
}

struct CopyState {
  std::atomic<bool>* cancelled = nullptr;
  uint64_t copied = 0;
  uint64_t total = 0;
  // Entries in a folder that aren't files or folders (links, devices): not copied.
  NSUInteger skipped = 0;
  XeniaIOSFileTransferProgress progress = nil;
  CFAbsoluteTime last_report = 0;

  void Report(bool force) {
    if (!progress) {
      return;
    }
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (!force && now - last_report < kProgressIntervalSeconds) {
      return;
    }
    last_report = now;
    progress(copied, total);
  }
};

BOOL CopyOneFile(NSString* source, NSString* destination, CopyState* state, NSError** error) {
  const int in_fd = open([source fileSystemRepresentation], O_RDONLY);
  if (in_fd < 0) {
    const int err = errno;
    if (error) {
      *error = XeniaIOSErrorFromErrno(err, NSLocalizedString(@"Could not read the selected file.",
                                                             @"file transfer: error message"));
    }
    return NO;
  }
  const int out_fd = open([destination fileSystemRepresentation], O_WRONLY | O_CREAT | O_TRUNC,
  0644);
  if (out_fd < 0) {
    const int err = errno;
    close(in_fd);
    if (error) {
      *error =
          XeniaIOSErrorFromErrno(err, NSLocalizedString(@"Could not create the destination file.",
                                                        @"file transfer: error message"));
    }
    return NO;
  }
  // Multi-GB streams shouldn't evict everything else from the page cache.
  fcntl(in_fd, F_NOCACHE, 1);
  fcntl(out_fd, F_NOCACHE, 1);

  char* buffer = static_cast<char*>(malloc(kCopyBufferSize));
  BOOL ok = YES;
  while (ok) {
    if (state->cancelled->load()) {
      if (error) {
        *error = CancelledError();
      }
      ok = NO;
      break;
    }
    const ssize_t read_count = read(in_fd, buffer, kCopyBufferSize);
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int err = errno;
      if (error) {
        *error = XeniaIOSErrorFromErrno(err, NSLocalizedString(@"Reading the selected file failed.",
                                                               @"file transfer: error message"));
      }
      ok = NO;
      break;
    }
    if (read_count == 0) {
      break;
    }
    ssize_t written = 0;
    while (written < read_count) {
      const ssize_t n = write(out_fd, buffer + written, static_cast<size_t>(read_count - written));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        const int err = errno;
        if (error) {
          *error = XeniaIOSErrorFromErrno(
              err, NSLocalizedString(@"Writing the copy failed.", @"file transfer: error message"));
        }
        ok = NO;
        break;
      }
      written += n;
    }
    if (!ok) {
      break;
    }
    state->copied += static_cast<uint64_t>(read_count);
    state->Report(false);
  }
  free(buffer);
  close(in_fd);
  // Flush before the caller renames the file into place, so a power loss can't leave a short
  // file under the final name. Plain fsync: F_FULLFSYNC on multi-GB files is far too slow.
  if (ok && fsync(out_fd) != 0) {
    const int err = errno;
    if (error) {
      *error = XeniaIOSErrorFromErrno(
          err, NSLocalizedString(@"Flushing the copy failed.", @"file transfer: error message"));
    }
    ok = NO;
  }
  if (close(out_fd) != 0 && ok) {
    const int err = errno;
    if (error) {
      *error = XeniaIOSErrorFromErrno(
          err, NSLocalizedString(@"Finishing the copy failed.", @"file transfer: error message"));
    }
    ok = NO;
  }
  return ok;
}

BOOL CopyOneItem(NSString* source, NSString* destination_part, CopyState* state, NSError** error) {
  NSFileManager* file_manager = [NSFileManager defaultManager];
  BOOL is_directory = NO;
  if (![file_manager fileExistsAtPath:source isDirectory:&is_directory]) {
    if (error) {
      *error = TransferError(XeniaIOSFileTransferErrorFailed,
                             NSLocalizedString(@"The selected file is no longer available.",
                                               @"file transfer: user-visible text"));
    }
    return NO;
  }
  if (!is_directory) {
    return CopyOneFile(source, destination_part, state, error);
  }

  NSError* local_error = nil;
  if (![file_manager createDirectoryAtPath:destination_part
               withIntermediateDirectories:YES
                                attributes:nil
                                     error:&local_error]) {
    if (error) {
      *error = local_error;
    }
    return NO;
  }
  NSDirectoryEnumerator* enumerator = [file_manager enumeratorAtPath:source];
  for (NSString* relative in enumerator) {
    NSString* type = enumerator.fileAttributes[NSFileType];
    NSString* from = [source stringByAppendingPathComponent:relative];
    NSString* to = [destination_part stringByAppendingPathComponent:relative];
    if ([type isEqualToString:NSFileTypeDirectory]) {
      if (![file_manager createDirectoryAtPath:to
                   withIntermediateDirectories:YES
                                    attributes:nil
                                         error:&local_error]) {
        if (error) {
          *error = local_error;
        }
        return NO;
      }
    } else if ([type isEqualToString:NSFileTypeRegular]) {
      if (!CopyOneFile(from, to, state, error)) {
        return NO;
      }
    } else {
      ++state->skipped;
    }
  }
  return YES;
}

}  // namespace

bool XeniaIOSVolumeAvailableCapacity(NSString* path, uint64_t* available_out) {
  NSString* probe = path;
  NSFileManager* file_manager = [NSFileManager defaultManager];
  while (probe.length > 1 && ![file_manager fileExistsAtPath:probe]) {
    probe = [probe stringByDeletingLastPathComponent];
  }
  NSURL* url = [NSURL fileURLWithPath:probe];
  NSNumber* important = nil;
  if ([url getResourceValue:&important
                     forKey:NSURLVolumeAvailableCapacityForImportantUsageKey
                      error:nil] &&
      important) {
    if (available_out) {
      *available_out = important.longLongValue > 0 ? important.unsignedLongLongValue : 0;
    }
    return true;
  }
  NSNumber* plain = nil;
  if ([url getResourceValue:&plain forKey:NSURLVolumeAvailableCapacityKey error:nil] && plain) {
    if (available_out) {
      *available_out = plain.longLongValue > 0 ? plain.unsignedLongLongValue : 0;
    }
    return true;
  }
  return false;
}

uint64_t XeniaIOSItemByteSize(NSString* path) {
  NSFileManager* file_manager = [NSFileManager defaultManager];
  BOOL is_directory = NO;
  if (![file_manager fileExistsAtPath:path isDirectory:&is_directory]) {
    return 0;
  }
  if (!is_directory) {
    return [[file_manager attributesOfItemAtPath:path error:nil] fileSize];
  }
  uint64_t total = 0;
  NSDirectoryEnumerator* enumerator = [file_manager enumeratorAtPath:path];
  for (NSString* relative in enumerator) {
    (void)relative;
    NSDictionary* attributes = enumerator.fileAttributes;
    if ([attributes[NSFileType] isEqualToString:NSFileTypeRegular]) {
      total += [attributes fileSize];
    }
  }
  return total;
}

NSString* XeniaIOSFormatByteCount(uint64_t bytes) {
  return [NSByteCountFormatter stringFromByteCount:static_cast<long long>(bytes)
                                        countStyle:NSByteCountFormatterCountStyleFile];
}

BOOL XeniaIOSCheckFreeSpace(NSString* path, uint64_t needed_bytes, NSError** error) {
  uint64_t available = 0;
  if (!XeniaIOSVolumeAvailableCapacity(path, &available)) {
    return YES;  // Unknown: let the copy itself report ENOSPC.
  }
  if (needed_bytes + kFreeSpaceReserveBytes <= available) {
    return YES;
  }
  if (error) {
    *error = TransferError(
        XeniaIOSFileTransferErrorInsufficientSpace,
        [NSString
            stringWithFormat:NSLocalizedString(
                                 @"Not enough storage: need %@, have %@. Free up space on this "
                                 @"device, or use Link Folder to play from the original "
                                 @"location without copying.",
                                 @"file transfer: user-visible text"),
                             XeniaIOSFormatByteCount(needed_bytes + kFreeSpaceReserveBytes),
                             XeniaIOSFormatByteCount(available)]);
  }
  return NO;
}

NSError* XeniaIOSErrorFromErrno(int err, NSString* fallback) {
  if (err == ENOSPC || err == EDQUOT) {
    return TransferError(
        XeniaIOSFileTransferErrorInsufficientSpace,
        NSLocalizedString(@"Storage is full. Free up space on this device and try again.",
                          @"file transfer: user-visible text"));
  }
  return TransferError(XeniaIOSFileTransferErrorFailed,
                       [NSString stringWithFormat:@"%@ (%s)", fallback, strerror(err)]);
}

bool XeniaIOSExcludeFromBackup(NSString* path) {
  if (path.length == 0 || ![[NSFileManager defaultManager] fileExistsAtPath:path]) {
    return false;
  }
  NSURL* url = [NSURL fileURLWithPath:path];
  NSNumber* excluded = nil;
  if ([url getResourceValue:&excluded forKey:NSURLIsExcludedFromBackupKey error:nil] &&
      excluded.boolValue) {
    return true;
  }
  NSError* error = nil;
  if (![url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error]) {
    XELOGW("iOS: Could not exclude {} from backup: {}", xe::strip_pii([path UTF8String]),
           error.localizedDescription ? [error.localizedDescription UTF8String] : "unknown");
    return false;
  }
  return true;
}

void XeniaIOSRegisterActivePartPath(NSString* path) {
  std::lock_guard<std::mutex> lock(g_active_part_mutex);
  ActivePartPaths().insert(std::string([path UTF8String]));
}

void XeniaIOSUnregisterActivePartPath(NSString* path) {
  std::lock_guard<std::mutex> lock(g_active_part_mutex);
  ActivePartPaths().erase(std::string([path UTF8String]));
}

size_t XeniaIOSSweepStalePartFiles(const std::filesystem::path& directory) {
  size_t removed = 0;
  std::error_code ec;
  std::filesystem::directory_iterator it(directory, ec);
  std::filesystem::directory_iterator end;
  std::vector<std::filesystem::path> stale;
  for (; !ec && it != end; it.increment(ec)) {
    const std::filesystem::path& path = it->path();
    if (path.extension() != ".part") {
      continue;
    }
    std::lock_guard<std::mutex> lock(g_active_part_mutex);
    if (ActivePartPaths().count(path.string())) {
      continue;
    }
    stale.push_back(path);
  }
  for (const std::filesystem::path& path : stale) {
    std::error_code remove_ec;
    std::filesystem::remove_all(path, remove_ec);
    if (!remove_ec) {
      ++removed;
      XELOGI("iOS: Removed stale partial transfer {}", xe::strip_pii(path.filename().string()));
    }
  }
  return removed;
}

@implementation XeniaIOSFileTransfer {
  std::atomic<bool> _cancelled;
  std::atomic<NSUInteger> _skippedItemCount;
}

@synthesize replacesExisting = _replacesExisting;

- (instancetype)init {
  if (!(self = [super init])) {
    return nil;
  }
  _cancelled.store(false);
  _skippedItemCount.store(0);
  return self;
}

- (void)cancel {
  _cancelled.store(true);
}

- (BOOL)isCancelled {
  return _cancelled.load();
}

- (NSUInteger)skippedItemCount {
  return _skippedItemCount.load();
}

- (BOOL)runPairs:(NSArray<NSArray<NSString*>*>*)pairs
        progress:(XeniaIOSFileTransferProgress)progress
           error:(NSError**)error {
  NSFileManager* file_manager = [NSFileManager defaultManager];
  CopyState state;
  state.cancelled = &_cancelled;
  state.progress = progress;
  for (NSArray<NSString*>* pair in pairs) {
    state.total += XeniaIOSItemByteSize(pair[0]);
  }
  if (!XeniaIOSCheckFreeSpace([pairs[0][1] stringByDeletingLastPathComponent], state.total,
                              error)) {
    return NO;
  }

  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  for (NSArray<NSString*>* pair in pairs) {
    NSString* part = PartPath(pair[1]);
    XeniaIOSRegisterActivePartPath(part);
    [parts addObject:part];
  }

  state.Report(true);
  BOOL ok = YES;
  for (NSUInteger i = 0; ok && i < pairs.count; ++i) {
    ok = CopyOneItem(pairs[i][0], parts[i], &state, error);
  }
  if (ok && _cancelled.load()) {
    if (error) {
      *error = CancelledError();
    }
    ok = NO;
  }

  NSMutableArray<NSString*>* renamed = [NSMutableArray array];
  // [final, backup] for items a replacing transfer moved out of the way. The backup carries a
  // ".part" suffix and is registered, so a crash leaves something the stale sweep removes.
  NSMutableArray<NSArray<NSString*>*>* backups = [NSMutableArray array];
  if (ok) {
    // The main item is pairs[0]; rename it last so it only appears once its
    // sidecars are in place.
    for (NSInteger i = static_cast<NSInteger>(pairs.count) - 1; ok && i >= 0; --i) {
      NSString* final_path = pairs[static_cast<NSUInteger>(i)][1];
      NSError* move_error = nil;
      if (_replacesExisting && [file_manager fileExistsAtPath:final_path]) {
        NSString* backup = PartPath(final_path);
        XeniaIOSRegisterActivePartPath(backup);
        if ([file_manager moveItemAtPath:final_path toPath:backup error:&move_error]) {
          [backups addObject:@[ final_path, backup ]];
        } else {
          XeniaIOSUnregisterActivePartPath(backup);
          if (error) {
            *error = move_error;
          }
          ok = NO;
          break;
        }
      }
      if ([file_manager moveItemAtPath:parts[static_cast<NSUInteger>(i)]
                                toPath:final_path
                                 error:&move_error]) {
        [renamed addObject:final_path];
      } else {
        if (error) {
          *error = move_error;
        }
        ok = NO;
      }
    }
  }

  if (!ok) {
    for (NSString* final_path in renamed) {
      RemovePath(final_path);
    }
    // Put back what the transfer was replacing.
    for (NSArray<NSString*>* backup in backups) {
      RemovePath(backup[0]);
      [file_manager moveItemAtPath:backup[1] toPath:backup[0] error:nil];
    }
    for (NSString* part in parts) {
      RemovePath(part);
    }
  } else {
    for (NSArray<NSString*>* backup in backups) {
      RemovePath(backup[1]);
    }
    state.copied = state.total;
    state.Report(true);
  }
  for (NSArray<NSString*>* backup in backups) {
    XeniaIOSUnregisterActivePartPath(backup[1]);
  }
  for (NSString* part in parts) {
    XeniaIOSUnregisterActivePartPath(part);
  }
  _skippedItemCount.store(ok ? state.skipped : 0);
  return ok;
}

- (BOOL)copyPairs:(NSArray<NSArray<NSString*>*>*)pairs
         progress:(XeniaIOSFileTransferProgress)progress
            error:(NSError**)error {
  if (pairs.count == 0) {
    return YES;
  }
  __block BOOL result = NO;
  __block BOOL ran = NO;
  __block NSError* run_error = nil;
  NSFileCoordinator* coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
  NSError* coordination_error = nil;
  NSURL* first_url = [NSURL fileURLWithPath:pairs[0][0]];
  [coordinator coordinateReadingItemAtURL:first_url
                                  options:NSFileCoordinatorReadingWithoutChanges
                                    error:&coordination_error
                               byAccessor:^(NSURL* coordinated_url) {
                                 ran = YES;
                                 NSMutableArray* adjusted = [NSMutableArray arrayWithArray:pairs];
                                 adjusted[0] = @[ coordinated_url.path, pairs[0][1] ];
                                 NSError* local_error = nil;
                                 result = [self runPairs:adjusted
                                                progress:progress
                                                   error:&local_error];
                                 run_error = [local_error retain];
                               }];
  [coordinator release];
  if (!ran) {
    // Coordination isn't available for this location (or was refused): read
    // the file directly and let any real access error surface from open().
    NSError* local_error = nil;
    result = [self runPairs:pairs progress:progress error:&local_error];
    run_error = [local_error retain];
  }
  if (!result && error) {
    *error = [run_error autorelease];
  } else {
    [run_error release];
  }
  return result;
}

@end

@implementation XeniaIOSTransferProgressAlert {
  UIAlertController* _alert;
  UIProgressView* _progressView;
  UIAlertAction* _cancelAction;
  NSString* _baseMessage;
  void (^_cancelHandler)(void);
  BOOL _presented;
  BOOL _dismissRequested;
  BOOL _cancelTapped;
  BOOL _dismissed;
  NSString* _activityName;
  UIBackgroundTaskIdentifier _backgroundTask;
  void (^_dismissCompletion)(void);
}

- (instancetype)init {
  if (!(self = [super init])) {
    return nil;
  }
  _backgroundTask = UIBackgroundTaskInvalid;
  return self;
}

- (void)dealloc {
  [self endActivity];
  [_activityName release];
  [_alert release];
  [_progressView release];
  [_baseMessage release];
  [_cancelHandler release];
  [_dismissCompletion release];
  [super dealloc];
}

+ (instancetype)presentFrom:(UIViewController*)presenter
                      title:(NSString*)title
                    message:(NSString*)message
              cancelHandler:(void (^)(void))cancelHandler {
  XeniaIOSTransferProgressAlert* progress_alert = [[[self alloc] init] autorelease];
  progress_alert->_baseMessage = [message copy];
  progress_alert->_cancelHandler = [cancelHandler copy];

  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:title
                       message:[message stringByAppendingString:@"\n\n"]
                preferredStyle:UIAlertControllerStyleAlert];
  progress_alert->_alert = [alert retain];
  // Not retained by the block: the caller owns the object until it has called
  // dismissWithCompletion: (or the user tapped Cancel and the transfer ended).
  __block XeniaIOSTransferProgressAlert* unretained_alert = progress_alert;
  progress_alert->_cancelAction =
      [UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"file transfer: title")
                               style:UIAlertActionStyleCancel
                             handler:^(__unused UIAlertAction* action) {
                               // UIKit dismisses the alert itself after the tap.
                               unretained_alert->_cancelTapped = YES;
                               void (^handler)(void) = unretained_alert->_cancelHandler;
                               unretained_alert->_cancelHandler = nil;
                               if (handler) {
                                 handler();
                                 [handler release];
                               }
                             }];
  [alert addAction:progress_alert->_cancelAction];

  UIProgressView* bar =
      [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
  bar.translatesAutoresizingMaskIntoConstraints = NO;
  [alert.view addSubview:bar];
  [NSLayoutConstraint activateConstraints:@[
    [bar.leadingAnchor constraintEqualToAnchor:alert.view.leadingAnchor constant:20.0],
    [bar.trailingAnchor constraintEqualToAnchor:alert.view.trailingAnchor constant:-20.0],
    [bar.bottomAnchor constraintEqualToAnchor:alert.view.bottomAnchor constant:-74.0],
  ]];
  progress_alert->_progressView = bar;

  [progress_alert beginActivity];
  [progress_alert tryPresentFrom:presenter attemptsLeft:6];
  return progress_alert;
}

// Keeps the screen on and asks iOS for background time for the length of the transfer.
- (void)beginActivity {
  _activityName =
      [[NSString stringWithFormat:@"transfer-%p", static_cast<void*>(self)] retain];
  xe_ios_idle_timer_hold(_activityName);
  __block XeniaIOSTransferProgressAlert* unretained_self = self;
  _backgroundTask = [[UIApplication sharedApplication]
      beginBackgroundTaskWithName:@"file-transfer"
                expirationHandler:^{
                  XELOGW("iOS: Background time expired; cancelling the transfer");
                  if (unretained_self->_cancelHandler) {
                    unretained_self->_cancelHandler();
                  }
                  [unretained_self endBackgroundTask];
                }];
}

- (void)endBackgroundTask {
  if (_backgroundTask != UIBackgroundTaskInvalid) {
    [[UIApplication sharedApplication] endBackgroundTask:_backgroundTask];
    _backgroundTask = UIBackgroundTaskInvalid;
  }
}

- (void)endActivity {
  [self endBackgroundTask];
  if (_activityName) {
    xe_ios_idle_timer_release(_activityName);
    [_activityName release];
    _activityName = nil;
  }
}

- (void)tryPresentFrom:(UIViewController*)presenter attemptsLeft:(int)attempts {
  if (_dismissRequested) {
    return;
  }
  UIViewController* top = presenter;
  while (top.presentedViewController && !top.presentedViewController.isBeingDismissed) {
    top = top.presentedViewController;
  }
  if ((top.isBeingDismissed || top.presentedViewController) && attempts > 0) {
    [presenter retain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.3 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     [self tryPresentFrom:presenter attemptsLeft:attempts - 1];
                     [presenter release];
                   });
    return;
  }
  [top presentViewController:_alert
                    animated:YES
                  completion:^{
                    _presented = YES;
                    if (_dismissRequested) {
                      [self finishDismiss];
                    }
                  }];
}

- (void)updateCopiedBytes:(uint64_t)copied totalBytes:(uint64_t)total {
  if (_dismissed || _dismissRequested) {
    return;
  }
  if (total > 0) {
    _progressView.progress = static_cast<float>(static_cast<double>(copied) / total);
    _alert.message =
        [NSString stringWithFormat:NSLocalizedString(@"%@\n%@ of %@\n", @"file transfer: message"),
                                   _baseMessage, XeniaIOSFormatByteCount(copied),
                                   XeniaIOSFormatByteCount(total)];
  }
}

- (void)setCancelAvailable:(BOOL)available {
  _cancelAction.enabled = available;
}

- (void)dismissWithCompletion:(void (^)(void))completion {
  if (_dismissed) {
    if (completion) {
      dispatch_async(dispatch_get_main_queue(), completion);
    }
    return;
  }
  _dismissRequested = YES;
  [self endActivity];
  [_dismissCompletion release];
  _dismissCompletion = [completion copy];
  if (_presented || !_alert.beingPresented) {
    [self finishDismiss];
  }
  // Otherwise the present completion calls finishDismiss.
}

- (void)finishDismiss {
  if (_dismissed) {
    return;
  }
  _dismissed = YES;
  void (^completion)(void) = _dismissCompletion;
  _dismissCompletion = nil;
  void (^done)(void) = ^{
    if (completion) {
      completion();
      [completion release];
    }
  };
  if (_cancelTapped || !_alert.presentingViewController) {
    // UIKit is already dismissing it (or it never appeared): give the
    // transition time to finish before the caller presents anything else.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 static_cast<int64_t>((_cancelTapped ? 0.4 : 0.0) * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), done);
    return;
  }
  [_alert dismissViewControllerAnimated:YES completion:done];
}

@end
