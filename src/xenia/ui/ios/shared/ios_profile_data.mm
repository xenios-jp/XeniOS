/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_profile_data.h"

#include <optional>
#include <utility>

namespace {

std::optional<XeniaIOSProfileDataHost>& Host() {
  static std::optional<XeniaIOSProfileDataHost> host;
  return host;
}

}  // namespace

NSNotificationName const XeniaProfileDataProgressNotification =
    @"XeniaProfileDataProgressNotification";
NSString* const XeniaProfileDataProgressFractionKey = @"fraction";

void xe_ios_profile_data_install(XeniaIOSProfileDataHost host) { Host() = std::move(host); }

const XeniaIOSProfileDataHost* xe_ios_profile_data_host(void) {
  return Host() ? &*Host() : nullptr;
}

NSError* xe_ios_profile_package_error(xe::kernel::xam::ProfilePackageError error, BOOL exporting) {
  using xe::kernel::xam::ProfilePackageError;
  NSString* text = nil;
  switch (error) {
    case ProfilePackageError::kNone:
      return nil;
    case ProfilePackageError::kOpenFailed:
      text = exporting ? NSLocalizedString(@"Couldn’t make the profile file. Try again.",
                                           @"Profile export: the file couldn't be created")
                       : NSLocalizedString(@"Couldn’t open the profile file.",
                                           @"Profile import: the file couldn't be opened");
      break;
    case ProfilePackageError::kBadFile:
      text = NSLocalizedString(@"This isn’t a XeniOS profile file, or it’s damaged. Export it "
                               @"again on the other device.",
                               @"Profile import: not a profile file or damaged");
      break;
    case ProfilePackageError::kNewerFormat:
      text = NSLocalizedString(@"This profile comes from a newer version of XeniOS. Update "
                               @"XeniOS, then open it again.",
                               @"Profile import: the file is from a newer app");
      break;
    case ProfilePackageError::kNoProfile:
      text = exporting ? NSLocalizedString(@"Sign in to a profile first.",
                                           @"Profile export: no profile signed in")
                       : NSLocalizedString(@"That profile isn’t on this device anymore.",
                                           @"Profile import: the profile to import into is gone");
      break;
    case ProfilePackageError::kProfileInUse:
      text = NSLocalizedString(@"Quit the game first. This profile is playing it.",
                               @"Profile import: the profile is in a running game");
      break;
    case ProfilePackageError::kLowDiskSpace:
      text = NSLocalizedString(@"There isn’t enough free space for this profile. Free up some "
                               @"space and try again.",
                               @"Profile import: not enough free space");
      break;
    case ProfilePackageError::kTooLarge:
      text = NSLocalizedString(@"This profile is too large to move in one file.",
                               @"Profile export or import: over the size limit");
      break;
    case ProfilePackageError::kReadFailed:
      text = NSLocalizedString(@"Some of the profile’s files couldn’t be read. Try again.",
                               @"Profile export: a file couldn't be read");
      break;
    case ProfilePackageError::kWriteFailed:
      text = exporting ? NSLocalizedString(@"Couldn’t write the profile file. Check the free "
                                           @"space and try again.",
                                           @"Profile export: writing failed")
                       : NSLocalizedString(@"Couldn’t write the profile’s files. Check the "
                                           @"free space and try again.",
                                           @"Profile import: writing failed");
      break;
  }
  if (!text) {
    text = NSLocalizedString(@"Something went wrong. Try again.",
                             @"Profile export or import: any other failure");
  }
  return [NSError errorWithDomain:@"XeniaProfileData"
                             code:static_cast<NSInteger>(error)
                         userInfo:@{NSLocalizedDescriptionKey : text}];
}

NSString* xe_ios_profile_package_contents_text(
    const xe::kernel::xam::ProfilePackageManifest& manifest) {
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  const NSUInteger games = manifest.titles.size();
  if (manifest.saves && games) {
    [parts
        addObject:games == 1
                      ? NSLocalizedString(@"saves for 1 game",
                                          @"Profile import: part of a list, saves of one game")
                      : [NSString localizedStringWithFormat:NSLocalizedString(
                                                                @"saves for %lu games",
                                                                @"Profile import: part of a list; "
                                                                @"%lu is a number of games"),
                                                            (unsigned long)games]];
  }
  if (manifest.achievements) {
    [parts addObject:NSLocalizedString(@"achievements", @"Profile import: part of a list")];
  }
  if (manifest.settings) {
    [parts addObject:NSLocalizedString(@"settings", @"Profile import: part of a list")];
  }
  if (!parts.count) {
    return NSLocalizedString(@"The profile only: its gamertag and picture.",
                             @"Profile import: a file with no saves, achievements or settings");
  }
  NSString* list = [NSListFormatter localizedStringByJoiningStrings:parts];
  if (!list.length) {
    return @"";
  }
  return [NSString stringWithFormat:@"%@%@.", [list substringToIndex:1].localizedUppercaseString,
                                    [list substringFromIndex:1]];
}
