/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_FILE_TRANSFER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_FILE_TRANSFER_H_

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <cstdint>
#include <filesystem>

// Shared building blocks for the multi-GB copies the app does (game, DLC and
// title-update imports): a cancellable copy with progress that writes to
// "<final name>.part" and renames when everything is on disk, a free-space
// check, and a progress alert with a Cancel button.

extern NSString* const XeniaIOSFileTransferErrorDomain;
enum {
  XeniaIOSFileTransferErrorCancelled = 1,
  XeniaIOSFileTransferErrorInsufficientSpace = 2,
  XeniaIOSFileTransferErrorFailed = 3,
};

typedef void (^XeniaIOSFileTransferProgress)(uint64_t copied_bytes, uint64_t total_bytes);

// Free capacity (NSURLVolumeAvailableCapacityForImportantUsageKey) of the
// volume holding `path` (or its closest existing parent). Returns false when
// the system doesn't report one.
bool XeniaIOSVolumeAvailableCapacity(NSString* path, uint64_t* available_out);
// Total size of a file, or of every regular file under a directory.
uint64_t XeniaIOSItemByteSize(NSString* path);
// "1.4 GB" style text.
NSString* XeniaIOSFormatByteCount(uint64_t bytes);
// Fails with a "not enough storage: need X, have Y" error when the volume
// holding `path` can't take `needed_bytes` plus a small reserve for saves.
BOOL XeniaIOSCheckFreeSpace(NSString* path, uint64_t needed_bytes, NSError** error);
// Maps ENOSPC/EDQUOT to the same user-facing storage error; other errnos get
// `fallback` and the strerror text.
NSError* XeniaIOSErrorFromErrno(int err, NSString* fallback);

// Sets NSURLIsExcludedFromBackupKey on a file or folder if it isn't set yet
// (idempotent; does nothing for a missing path). Returns true if the item is
// excluded afterwards.
bool XeniaIOSExcludeFromBackup(NSString* path);

// Paths of in-flight ".part" outputs. The library scan's stale-part sweep skips
// them. Thread-safe.
void XeniaIOSRegisterActivePartPath(NSString* path);
void XeniaIOSUnregisterActivePartPath(NSString* path);
// Deletes "*.part" files and folders directly inside `directory` that aren't
// registered as active. Returns the count removed.
size_t XeniaIOSSweepStalePartFiles(const std::filesystem::path& directory);

@interface XeniaIOSFileTransfer : NSObject

// Copies each [source, destination] pair to "<destination>.part" (a file or a
// directory tree), then renames them all, last pair first, so a half-finished
// transfer is never visible under a final name. Blocking: call it from a
// background queue. The first source is read through NSFileCoordinator so
// iCloud placeholders download first (that wait can't be cancelled). On
// failure or cancel every ".part" is deleted. The progress block is throttled
// and called on the calling thread.
- (BOOL)copyPairs:(NSArray<NSArray<NSString*>*>*)pairs
         progress:(XeniaIOSFileTransferProgress)progress
            error:(NSError**)error;

// When YES an existing item at a destination is replaced by the finished copy
// (removed only after the copy succeeded). Default NO: the rename fails.
@property(nonatomic) BOOL replacesExisting;

// Folder entries that were neither files nor folders (symbolic links and the like) and so were
// not copied, as of the last successful copy. Callers should tell the user when it is not 0.
@property(atomic, readonly) NSUInteger skippedItemCount;

// Callable from any thread.
- (void)cancel;
@property(atomic, readonly, getter=isCancelled) BOOL cancelled;

@end

// A modal alert with a progress bar and a Cancel button. Main thread only. While it is up the
// screen stays awake and the app holds a background task; if iOS ends that time the transfer is
// cancelled through `cancelHandler`.
@interface XeniaIOSTransferProgressAlert : NSObject

// `cancelHandler` runs once on the main thread if the user taps Cancel.
+ (instancetype)presentFrom:(UIViewController*)presenter
                      title:(NSString*)title
                    message:(NSString*)message
              cancelHandler:(void (^)(void))cancelHandler;
// Pass total_bytes == 0 for an indeterminate wait (the bar stays empty).
- (void)updateCopiedBytes:(uint64_t)copied totalBytes:(uint64_t)total;
- (void)dismissWithCompletion:(void (^)(void))completion;
// Hides the Cancel button, e.g. for an installer phase that can't be cancelled.
- (void)setCancelAvailable:(BOOL)available;

@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_FILE_TRANSFER_H_
