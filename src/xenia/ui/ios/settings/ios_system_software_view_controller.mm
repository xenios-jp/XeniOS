/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_system_software_view_controller.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <filesystem>
#include <memory>

#include "xenia/ui/ios/launcher/ios_dashboard.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

enum SystemSoftwareSection : NSInteger {
  kSectionStatus,
  kSectionInstall,
  kSectionRemove,
};

enum InstallRow : NSInteger {
  kRowGetUpdate,
  kRowInstall,
};

// What an install that didn't succeed says. Nil for a cancelled one.
NSString* InstallFailureMessage(xe::ui::SystemFilesResult result) {
  using xe::ui::SystemFilesResult;
  switch (result) {
    case SystemFilesResult::kSuccess:
    case SystemFilesResult::kCancelled:
      return nil;
    case SystemFilesResult::kNotSystemUpdate:
      return NSLocalizedString(
          @"This isn't a USB system update. Pick the system update zip from Microsoft, or the "
          @"folder Files expands it to.",
          @"system software view controller: install error");
    case SystemFilesResult::kReadFailed:
      return NSLocalizedString(@"The system update couldn't be read.",
                               @"system software view controller: install error");
    case SystemFilesResult::kWriteFailed:
      return NSLocalizedString(
          @"The system software couldn't be written. Check that there is enough free space.",
          @"system software view controller: install error");
    case SystemFilesResult::kRestoreFailed:
      return NSLocalizedString(
          @"The previous system software couldn't be put back. Its folders are in the "
          @"\"unrestored\" folder in XeniOS's system folder in Files. Move them back or delete "
          @"them to install again.",
          @"system software view controller: install error");
  }
  return nil;
}

}  // namespace

@interface XeniaSystemSoftwareViewController () <UIDocumentPickerDelegate>
@end

@implementation XeniaSystemSoftwareViewController {
  BOOL game_running_;
  BOOL loaded_;
  BOOL busy_;
  xe::ui::IOSSystemSoftwareInfo info_;
  UIAlertController* progress_alert_;
}

- (instancetype)initWithGameRunning:(BOOL)game_running {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    game_running_ = game_running;
    self.title = NSLocalizedString(@"Xbox 360 System Software",
                                   @"system software view controller: title");
  }
  return self;
}

- (void)dealloc {
  [progress_alert_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  [self reloadInfo];
}

- (void)reloadInfo {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    auto info = std::make_shared<xe::ui::IOSSystemSoftwareInfo>(xe::ui::ReadIOSSystemSoftwareInfo());
    dispatch_async(dispatch_get_main_queue(), ^{
      info_ = *info;
      loaded_ = YES;
      [self.tableView reloadData];
    });
  });
}

- (BOOL)canChangeSystemSoftware {
  return loaded_ && !busy_ && !game_running_;
}

#pragma mark - Table

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return info_.installed ? 3 : 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  switch (section) {
    case kSectionStatus:
      return info_.installed ? 2 : 1;
    case kSectionInstall:
      return 2;
    default:
      return 1;
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger)section {
  switch (section) {
    case kSectionStatus:
      if (info_.has_unrestored_files) {
        return NSLocalizedString(
            @"A failed install left the previous system software in the \"unrestored\" folder in "
            @"XeniOS's system folder in Files. Move it back or delete it to install again.",
            @"system software view controller: footer");
      }
      return NSLocalizedString(
          @"The Xbox 360 Dashboard comes with the system software. XeniOS keeps it in its system "
          @"folder in Files.",
          @"system software view controller: footer");
    case kSectionInstall:
      if (game_running_) {
        return NSLocalizedString(@"Quit the game to install the system software.",
                                 @"system software view controller: footer");
      }
      return NSLocalizedString(
          @"Download the USB system update from Microsoft's page to Files, then install it here. "
          @"You can pick the zip or the folder Files expands it to.",
          @"system software view controller: footer");
    default:
      return game_running_ ? NSLocalizedString(@"Quit the game to remove the system software.",
                                                @"system software view controller: footer")
                           : nil;
  }
}

- (UITableViewCell*)statusCellForRow:(NSInteger)row {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  if (!loaded_) {
    cell.textLabel.text = NSLocalizedString(@"Checking…", @"system software view controller: status");
  } else if (!info_.installed) {
    cell.textLabel.text =
        NSLocalizedString(@"Not Installed", @"system software view controller: status");
  } else if (row == 0) {
    cell.textLabel.text = NSLocalizedString(@"Version", @"system software view controller: label");
    cell.detailTextLabel.text =
        info_.version.empty()
            ? NSLocalizedString(@"Installed", @"system software view controller: status")
            : [NSString stringWithUTF8String:info_.version.c_str()];
  } else {
    cell.textLabel.text = NSLocalizedString(@"Size", @"system software view controller: label");
    cell.detailTextLabel.text =
        [NSByteCountFormatter stringFromByteCount:static_cast<long long>(info_.size)
                                       countStyle:NSByteCountFormatterCountStyleFile];
  }
  return cell;
}

- (UITableViewCell*)actionCellWithTitle:(NSString*)title
                             symbolName:(NSString*)symbol_name
                                   hint:(NSString*)hint
                                enabled:(BOOL)enabled
                            destructive:(BOOL)destructive {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  UIColor* color = destructive ? [XeniaTheme statusError] : [XeniaTheme accent];
  cell.textLabel.text = title;
  cell.textLabel.textColor = enabled ? color : [XeniaTheme textMuted];
  cell.imageView.image = [UIImage systemImageNamed:symbol_name];
  cell.imageView.tintColor = enabled ? color : [XeniaTheme textMuted];
  cell.userInteractionEnabled = enabled;
  cell.accessibilityTraits =
      UIAccessibilityTraitButton | (enabled ? UIAccessibilityTraitNone : UIAccessibilityTraitNotEnabled);
  cell.accessibilityHint = hint;
  return cell;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == kSectionStatus) {
    return [self statusCellForRow:indexPath.row];
  }
  if (indexPath.section == kSectionInstall) {
    if (indexPath.row == kRowGetUpdate) {
      return [self actionCellWithTitle:NSLocalizedString(@"Get the Update from Microsoft",
                                                         @"system software view controller: action")
                            symbolName:@"safari"
                                  hint:NSLocalizedString(
                                           @"Opens Microsoft's system update page in Safari.",
                                           @"system software view controller: accessibility hint")
                               enabled:YES
                           destructive:NO];
    }
    return [self
        actionCellWithTitle:info_.installed
                                ? NSLocalizedString(@"Reinstall from Files",
                                                    @"system software view controller: action")
                                : NSLocalizedString(@"Install from Files",
                                                    @"system software view controller: action")
                 symbolName:@"square.and.arrow.down"
                       hint:NSLocalizedString(
                                @"Opens Files to pick the system update zip or its folder.",
                                @"system software view controller: accessibility hint")
                    enabled:[self canChangeSystemSoftware]
                destructive:NO];
  }
  return [self actionCellWithTitle:NSLocalizedString(@"Remove System Software",
                                                     @"system software view controller: action")
                        symbolName:@"trash"
                              hint:NSLocalizedString(
                                       @"Removes the system software. Asks first.",
                                       @"system software view controller: accessibility hint")
                           enabled:[self canChangeSystemSoftware]
                       destructive:YES];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == kSectionInstall && indexPath.row == kRowGetUpdate) {
    NSURL* url = [NSURL URLWithString:XeniaIOSSystemUpdateSupportURL];
    [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
    return;
  }
  if (![self canChangeSystemSoftware]) {
    return;
  }
  if (indexPath.section == kSectionInstall) {
    [self presentInstallPicker];
  } else if (indexPath.section == kSectionRemove) {
    [self confirmRemove];
  }
}

#pragma mark - Install

- (void)presentInstallPicker {
  // A copy, so the install reads local files the picker's access doesn't
  // have to cover.
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeZIP, UTTypeFolder ]
                                                                  asCopy:YES];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  [self presentViewController:picker animated:YES completion:nil];
  [picker release];
}

- (void)documentPicker:(UIDocumentPickerViewController* __unused)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  NSURL* url = urls.firstObject;
  if (url) {
    [self installFromURL:url];
  }
}

- (void)setProgressPercent:(int)percent {
  progress_alert_.message = [NSString
      stringWithFormat:NSLocalizedString(@"%d%% done",
                                         @"system software view controller: install progress"),
                       percent];
}

- (void)installFromURL:(NSURL*)url {
  busy_ = YES;
  [self.tableView reloadData];

  auto cancel = std::make_shared<std::atomic<bool>>(false);
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Installing System Software",
                                                 @"system software view controller: title")
                       message:@" "
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                            style:UIAlertActionStyleCancel
                                          handler:^(UIAlertAction* __unused action) {
                                            cancel->store(true);
                                          }]];
  [progress_alert_ release];
  progress_alert_ = [alert retain];
  [self setProgressPercent:0];
  [self presentViewController:alert animated:YES completion:nil];

  const std::filesystem::path source([url fileSystemRepresentation]);
  NSURL* picked_url = [url retain];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    int last_percent = 0;
    const auto result = xe::ui::InstallIOSSystemSoftware(
        source, [&last_percent, cancel, self](size_t done, size_t total) {
          const int percent = total ? static_cast<int>(done * 100 / total) : 0;
          if (percent != last_percent) {
            last_percent = percent;
            dispatch_async(dispatch_get_main_queue(), ^{
              [self setProgressPercent:percent];
            });
          }
          return !cancel->load();
        });
    dispatch_async(dispatch_get_main_queue(), ^{
      [self finishInstallWithResult:result pickedURL:picked_url];
      [picked_url release];
    });
  });
}

- (void)finishInstallWithResult:(xe::ui::SystemFilesResult)result pickedURL:(NSURL*)picked_url {
  busy_ = NO;
  // The picker's copy is only for the install.
  NSString* temporary = NSTemporaryDirectory().stringByStandardizingPath;
  if ([picked_url.path.stringByStandardizingPath hasPrefix:temporary]) {
    [[NSFileManager defaultManager] removeItemAtURL:picked_url error:nil];
  }
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaIOSSystemSoftwareDidChangeNotification
                    object:nil];
  [self reloadInfo];

  void (^report)(void) = ^{
    if (result == xe::ui::SystemFilesResult::kSuccess) {
      XEPresentOKAlert(self,
                       NSLocalizedString(@"System Software Installed",
                                         @"system software view controller: title"),
                       NSLocalizedString(@"Start the Xbox 360 Dashboard from the Library.",
                                         @"system software view controller: message"));
    } else if (NSString* message = InstallFailureMessage(result)) {
      XEPresentOKAlert(self,
                       NSLocalizedString(@"Couldn't Install the System Software",
                                         @"system software view controller: title"),
                       message);
    }
  };
  UIAlertController* alert = [progress_alert_ autorelease];
  progress_alert_ = nil;
  if (alert.presentingViewController) {
    [alert dismissViewControllerAnimated:YES completion:report];
  } else {
    report();
  }
}

#pragma mark - Remove

- (void)confirmRemove {
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Remove System Software?",
                                                 @"system software view controller: title")
                       message:NSLocalizedString(
                                   @"The Xbox 360 Dashboard won't start until you install the "
                                   @"system update again. Your games, profiles and saves stay.",
                                   @"system software view controller: message")
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
  [confirm addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(@"Remove",
                                                           @"system software view controller: action")
                                   style:UIAlertActionStyleDestructive
                                 handler:^(UIAlertAction* __unused action) {
                                   [self remove];
                                 }]];
  [self presentViewController:confirm animated:YES completion:nil];
}

- (void)remove {
  busy_ = YES;
  [self.tableView reloadData];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const bool removed = xe::ui::RemoveIOSSystemSoftware();
    dispatch_async(dispatch_get_main_queue(), ^{
      busy_ = NO;
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaIOSSystemSoftwareDidChangeNotification
                        object:nil];
      [self reloadInfo];
      if (!removed) {
        XEPresentOKAlert(self,
                         NSLocalizedString(@"Couldn't Remove the System Software",
                                           @"system software view controller: title"),
                         NSLocalizedString(@"Delete XeniOS's system folder in Files instead.",
                                           @"system software view controller: message"));
      }
    });
  });
}

@end
