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
#include <vector>

#include "xenia/ui/ios/launcher/ios_dashboard.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

enum SystemSoftwareSection : NSInteger {
  kSectionStatus,
  // Every installed version, once there is more to choose from than the one games use.
  kSectionVersions,
  kSectionInstall,
  // The one version there is, as a whole.
  kSectionRemove,
};

enum InstallRow : NSInteger {
  kRowGetUpdate,
  kRowInstall,
};

}  // namespace

@interface XeniaSystemSoftwareViewController () <UIDocumentPickerDelegate>
@end

@implementation XeniaSystemSoftwareViewController {
  BOOL game_running_;
  BOOL loaded_;
  BOOL busy_;
  xe::ui::IOSSystemSoftwareInfo info_;
  // The sections shown, in order.
  std::vector<SystemSoftwareSection> sections_;
  UIAlertController* progress_alert_;
  XEStatusHeaderView* header_;
}

- (instancetype)initWithGameRunning:(BOOL)game_running {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    game_running_ = game_running;
    // On the wash like every page (rule 1), its rows in glass groups.
    self.glassStyle = YES;
    // Short enough for a large title; the Settings row says what it is for.
    self.title = NSLocalizedString(@"System Software", @"system software view controller: title");
  }
  return self;
}

- (void)dealloc {
  [progress_alert_ release];
  [header_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  if (self.showsDoneButton) {
    XESetSheetClose(self, self, @selector(doneTapped:));
  }
  // The status is the headline; the title stays small and centred so the two don't compete.
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  header_ = [[XEStatusHeaderView alloc] init];
  [self rebuildSections];
  [self refreshHeader];
  [self reloadInfo];
}

// A single version that games use is the whole page, as it was before versions could be kept
// side by side. Anything else lists the versions.
- (BOOL)showsVersions {
  return info_.versions.size() > 1 || (info_.versions.size() == 1 && !info_.versions[0].is_default);
}

// Whether the update Download & Install fetches, Microsoft's last, is installed.
- (BOOL)hasDownloadedVersion {
  for (const auto& version : info_.versions) {
    if (version.build == xe::ui::kIOSDownloadedSystemBuild) {
      return YES;
    }
  }
  return NO;
}

- (void)rebuildSections {
  sections_ = {kSectionStatus};
  const BOOL shows_versions = [self showsVersions];
  if (shows_versions) {
    sections_.push_back(kSectionVersions);
  }
  sections_.push_back(kSectionInstall);
  // Remove also clears the "unrestored" copy a failed install left, which is the
  // only way back in the app when the dashboard itself is missing.
  if (info_.installed ? !shows_versions : info_.has_unrestored_files) {
    sections_.push_back(kSectionRemove);
  }
}

- (SystemSoftwareSection)sectionAt:(NSInteger)section {
  return sections_[static_cast<size_t>(section)];
}

- (NSString*)nameOfVersion:(const xe::ui::IOSSystemVersion&)version {
  return version.version.empty()
             ? NSLocalizedString(@"Unknown Version",
                                 @"system software view controller: a version that can't be read")
             : [NSString stringWithUTF8String:version.version.c_str()];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [header_ fitAsHeaderOfTable:self.tableView];
}

// "● Installed" over its version and size, or "● Not Installed" over what it is for: status is
// a dot and words (rule 5), never a row.
- (void)refreshHeader {
  if (!header_) {
    return;
  }
  if (!loaded_) {
    header_.titleLabel.text =
        NSLocalizedString(@"Checking…", @"system software view controller: status");
    header_.dot.backgroundColor = [XeniaTheme textMuted];
    header_.detailLabel.text = nil;
  } else if (info_.versions.size() > 1) {
    header_.titleLabel.text =
        NSLocalizedString(@"Installed", @"system software view controller: status");
    header_.dot.backgroundColor = [XeniaTheme accent];
    uint64_t size = 0;
    for (const auto& version : info_.versions) {
      size += version.size;
    }
    header_.detailLabel.text = [NSString
        stringWithFormat:NSLocalizedString(@"%lu versions · %@",
                                           @"system software view controller: header; the number "
                                           @"of installed versions and their size together"),
                         static_cast<unsigned long>(info_.versions.size()),
                         [NSByteCountFormatter
                             stringFromByteCount:static_cast<long long>(size)
                                      countStyle:NSByteCountFormatterCountStyleFile]];
  } else if (info_.versions.size() == 1 && !info_.installed) {
    // One version, which games don't use.
    const auto& version = info_.versions[0];
    header_.titleLabel.text =
        NSLocalizedString(@"Installed", @"system software view controller: status");
    header_.dot.backgroundColor = [XeniaTheme accent];
    header_.detailLabel.text =
        [NSString stringWithFormat:@"%@ · %@", [self nameOfVersion:version],
                                   [NSByteCountFormatter
                                       stringFromByteCount:static_cast<long long>(version.size)
                                                countStyle:NSByteCountFormatterCountStyleFile]];
  } else if (info_.installed) {
    header_.titleLabel.text =
        NSLocalizedString(@"Installed", @"system software view controller: status");
    header_.dot.backgroundColor = [XeniaTheme accent];
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    if (!info_.version.empty()) {
      [parts addObject:[NSString stringWithFormat:NSLocalizedString(
                                                      @"Version %@",
                                                      @"system software view controller: "
                                                      @"header; %@ is the version"),
                                                  [NSString stringWithUTF8String:
                                                                info_.version.c_str()]]];
    }
    if (info_.size) {
      [parts addObject:[NSByteCountFormatter
                           stringFromByteCount:static_cast<long long>(info_.size)
                                    countStyle:NSByteCountFormatterCountStyleFile]];
    }
    header_.detailLabel.text = [parts componentsJoinedByString:@" · "];
  } else {
    header_.titleLabel.text =
        NSLocalizedString(@"Not Installed", @"system software view controller: status");
    header_.dot.backgroundColor = [XeniaTheme statusWarning];
    header_.detailLabel.text = NSLocalizedString(
        @"The Xbox 360 Dashboard comes with the system software. XeniOS keeps it in its system "
        @"folder in Files.",
        @"system software view controller: footer");
  }
  [self.view setNeedsLayout];
}

- (void)doneTapped:(id)__unused sender {
  UIViewController* sheet = self.navigationController ?: self;
  [sheet.presentingViewController dismissViewControllerAnimated:YES completion:nil];
}

- (void)reloadInfo {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    auto info = std::make_shared<xe::ui::IOSSystemSoftwareInfo>(xe::ui::ReadIOSSystemSoftwareInfo());
    dispatch_async(dispatch_get_main_queue(), ^{
      info_ = *info;
      loaded_ = YES;
      [self rebuildSections];
      [self refreshHeader];
      [self.tableView reloadData];
    });
  });
}

- (BOOL)canChangeSystemSoftware {
  return loaded_ && !busy_ && !game_running_;
}

#pragma mark - Table

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return static_cast<NSInteger>(sections_.size());
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  switch ([self sectionAt:section]) {
    case kSectionStatus:
      return 0;  // The header says it.
    case kSectionVersions:
      return static_cast<NSInteger>(info_.versions.size());
    case kSectionInstall:
      return 2;
    case kSectionRemove:
      return 1;
  }
  return 0;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return [self sectionAt:section] == kSectionVersions
             ? NSLocalizedString(@"Versions", @"system software view controller: section title")
             : nil;
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger)section {
  switch ([self sectionAt:section]) {
    case kSectionVersions:
      if (game_running_) {
        return NSLocalizedString(
            @"Quit the game to change the version games use, or to remove one.",
            @"system software view controller: footer");
      }
      return info_.installed
                 ? NSLocalizedString(
                       @"Games use the checked version. Each version has its own Dashboard in the "
                       @"Library.",
                       @"system software view controller: footer")
                 : NSLocalizedString(
                       @"Games use none of these yet. Tap a version to use it for games. Each "
                       @"version has its own Dashboard in the Library.",
                       @"system software view controller: footer");
    case kSectionStatus:
      if (info_.has_unrestored_files) {
        return NSLocalizedString(
            @"A failed install left the previous system software in the \"unrestored\" folder in "
            @"XeniOS's system folder in Files. Move it back or delete it to install again.",
            @"system software view controller: footer");
      }
      return nil;
    case kSectionInstall:
      if (game_running_) {
        return NSLocalizedString(@"Quit the game to install the system software.",
                                 @"system software view controller: footer");
      }
      if (info_.versions.empty()) {
        return NSLocalizedString(
            @"XeniOS downloads the system update from Microsoft and installs it. Or "
            @"install a USB system update you have in Files: the zip, or the folder "
            @"Files expands it to.",
            @"system software view controller: footer");
      }
      return [self hasDownloadedVersion]
                 ? NSLocalizedString(
                       @"Reinstall downloads the system update from Microsoft and installs it "
                       @"again. To add another version, install its USB system update from "
                       @"Files: the zip, or the folder Files expands it to. Each version gets its "
                       @"own Dashboard in the Library.",
                       @"system software view controller: footer")
                 : NSLocalizedString(
                       @"Download & Install adds the latest system update from Microsoft. To add "
                       @"another version, install its USB system update from Files: the zip, or "
                       @"the folder Files expands it to. Each version gets its own Dashboard in "
                       @"the Library.",
                       @"system software view controller: footer");
    case kSectionRemove:
      return game_running_ ? NSLocalizedString(@"Quit the game to remove the system software.",
                                                @"system software view controller: footer")
                           : nil;
  }
  return nil;
}

// A version: its number over what it is used for and its size, checked when games use it.
- (UITableViewCell*)versionCellForRow:(NSInteger)row {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                                  reuseIdentifier:nil] autorelease];
  const auto& version = info_.versions[static_cast<size_t>(row)];
  const BOOL enabled = [self canChangeSystemSoftware];
  cell.textLabel.text = [self nameOfVersion:version];
  cell.textLabel.textColor = enabled ? [XeniaTheme textPrimary] : [XeniaTheme textMuted];
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  if (version.is_default) {
    [parts addObject:NSLocalizedString(@"Used for Games",
                                       @"system software view controller: the version games use")];
  }
  [parts addObject:[NSByteCountFormatter stringFromByteCount:static_cast<long long>(version.size)
                                                  countStyle:NSByteCountFormatterCountStyleFile]];
  cell.detailTextLabel.text = [parts componentsJoinedByString:@" · "];
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.accessoryType =
      version.is_default ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
  cell.userInteractionEnabled = enabled;
  cell.accessibilityTraits = UIAccessibilityTraitButton |
                             (enabled ? UIAccessibilityTraitNone : UIAccessibilityTraitNotEnabled);
  cell.accessibilityHint =
      NSLocalizedString(@"Offers to use this version for games, or to remove it.",
                        @"system software view controller: accessibility hint");
  return cell;
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
  // A plain row symbol, as on every other list; only Remove reads in red.
  UIColor* symbol_color = enabled && destructive ? [XeniaTheme statusError] : nil;
  cell.textLabel.text = title;
  // Wrap long titles instead of cutting them off.
  cell.textLabel.numberOfLines = 0;
  cell.textLabel.textColor = !enabled      ? [XeniaTheme textMuted]
                             : destructive ? [XeniaTheme statusError]
                                           : [XeniaTheme textPrimary];
  cell.imageView.image = XERowSymbolImage(symbol_name, symbol_color);
  cell.userInteractionEnabled = enabled;
  cell.accessibilityTraits =
      UIAccessibilityTraitButton | (enabled ? UIAccessibilityTraitNone : UIAccessibilityTraitNotEnabled);
  cell.accessibilityHint = hint;
  return cell;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  const SystemSoftwareSection section = [self sectionAt:indexPath.section];
  if (section == kSectionStatus) {
    return [self statusCellForRow:indexPath.row];
  }
  if (section == kSectionVersions) {
    return [self versionCellForRow:indexPath.row];
  }
  if (section == kSectionInstall) {
    if (indexPath.row == kRowGetUpdate) {
      const BOOL reinstalls = [self hasDownloadedVersion];
      return [self
          actionCellWithTitle:reinstalls
                                  ? NSLocalizedString(@"Reinstall",
                                                      @"system software view controller: action; "
                                                      @"downloads and installs again")
                                  : NSLocalizedString(@"Download & Install",
                                                      @"system software view controller: action")
                   symbolName:reinstalls ? @"arrow.clockwise" : @"arrow.down"
                         hint:NSLocalizedString(
                                  @"Downloads the system update from Microsoft and "
                                  @"installs it.",
                                  @"system software view controller: accessibility hint")
                      enabled:[self canChangeSystemSoftware]
                  destructive:NO];
    }
    return [self
        actionCellWithTitle:!info_.versions.empty()
                                ? NSLocalizedString(@"Install Another Version from Files",
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
                        symbolName:@"trash.fill"
                              hint:info_.has_unrestored_files
                                       ? NSLocalizedString(
                                             @"Removes the system software and the unrestored "
                                             @"copy a failed install left. Asks first.",
                                             @"system software view controller: accessibility hint")
                                       : NSLocalizedString(
                                             @"Removes the system software. Asks first.",
                                             @"system software view controller: accessibility hint")
                           enabled:[self canChangeSystemSoftware]
                       destructive:YES];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (![self canChangeSystemSoftware]) {
    return;
  }
  switch ([self sectionAt:indexPath.section]) {
    case kSectionStatus:
      break;
    case kSectionVersions:
      [self presentActionsForVersionAtRow:indexPath.row];
      break;
    case kSectionInstall:
      if (indexPath.row == kRowGetUpdate) {
        [self downloadAndInstall];
      } else {
        [self presentInstallPicker];
      }
      break;
    case kSectionRemove:
      [self confirmRemove];
      break;
  }
}

#pragma mark - Install

- (void)downloadAndInstall {
  busy_ = YES;
  [self.tableView reloadData];

  auto cancel = std::make_shared<std::atomic<bool>>(false);
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Downloading the System Update",
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

  xe::ui::DownloadAndInstallIOSSystemUpdateAsync(
      cancel,
      ^(int percent, BOOL installing) {
        if (installing) {
          progress_alert_.title = NSLocalizedString(@"Installing System Software",
                                                    @"system software view controller: title");
        }
        [self setProgressPercent:percent];
      },
      ^(xe::ui::SystemUpdateDownload download, xe::ui::SystemFilesResult install) {
        if (download == xe::ui::SystemUpdateDownload::kDownloaded) {
          [self finishInstallWithResult:install pickedURL:nil];
          return;
        }
        busy_ = NO;
        [self reloadInfo];
        NSString* message = xe::ui::IOSSystemUpdateDownloadFailureMessage(download);
        void (^report)(void) = ^{
          if (message) {
            XEPresentOKAlert(self,
                             NSLocalizedString(@"Couldn't Download the System Update",
                                               @"system software view controller: title"),
                             message);
          }
        };
        // Cancel has already closed it.
        UIAlertController* progress = [progress_alert_ autorelease];
        progress_alert_ = nil;
        if (progress.presentingViewController) {
          [progress dismissViewControllerAnimated:YES completion:report];
        } else {
          report();
        }
      });
}

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

  xe::ui::InstallIOSSystemSoftwareAsync(
      url, cancel,
      ^(int percent) {
        [self setProgressPercent:percent];
      },
      ^(NSString* version, void (^answer)(BOOL replace)) {
        // Cancel reached the alert after the update was unpacked.
        if (cancel->load()) {
          answer(NO);
          return;
        }
        [self confirmReplacingVersion:version answer:answer];
      },
      ^(xe::ui::SystemFilesResult result) {
        [self finishInstallWithResult:result pickedURL:url];
      });
}

// The update picked is a version that is installed already: it only replaces that one once
// the user says so. Until then nothing installed has changed.
- (void)confirmReplacingVersion:(NSString*)version answer:(void (^)(BOOL replace))answer {
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:[NSString
                                   stringWithFormat:NSLocalizedString(
                                                        @"Replace Version %@?",
                                                        @"system software view controller: title; "
                                                        @"%@ is like 2.0.17559.0"),
                                                    version]
                       message:NSLocalizedString(
                                   @"This version is already installed. Installing it again "
                                   @"replaces it.",
                                   @"system software view controller: message")
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:^(UIAlertAction* __unused action) {
                                         answer(NO);
                                       }]];
  [confirm addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(
                                             @"Replace", @"system software view controller: action")
                                   style:UIAlertActionStyleDefault
                                 handler:^(UIAlertAction* __unused action) {
                                   answer(YES);
                                 }]];
  // In place of the progress, which is done.
  UIAlertController* progress = [progress_alert_ autorelease];
  progress_alert_ = nil;
  if (progress.presentingViewController) {
    [progress dismissViewControllerAnimated:YES
                                 completion:^{
                                   [self presentViewController:confirm animated:YES completion:nil];
                                 }];
  } else {
    [self presentViewController:confirm animated:YES completion:nil];
  }
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
    } else if (NSString* message = xe::ui::IOSSystemSoftwareInstallFailureMessage(result)) {
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

#pragma mark - Versions

// What can be done with one version: use it for games, or remove it.
- (void)presentActionsForVersionAtRow:(NSInteger)row {
  if (row < 0 || row >= static_cast<NSInteger>(info_.versions.size())) {
    return;
  }
  const xe::ui::IOSSystemVersion version = info_.versions[static_cast<size_t>(row)];
  NSString* name = [self nameOfVersion:version];
  UIAlertController* actions = [UIAlertController
      alertControllerWithTitle:name
                       message:[NSByteCountFormatter
                                   stringFromByteCount:static_cast<long long>(version.size)
                                            countStyle:NSByteCountFormatterCountStyleFile]
                preferredStyle:UIAlertControllerStyleAlert];
  if (!version.is_default) {
    [actions addAction:[UIAlertAction
                           actionWithTitle:NSLocalizedString(
                                               @"Use for Games",
                                               @"system software view controller: action; makes "
                                               @"this the version every game runs on")
                                     style:UIAlertActionStyleDefault
                                   handler:^(UIAlertAction* __unused action) {
                                     [self useVersionForGames:version];
                                   }]];
  }
  [actions addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(
                                             @"Remove", @"system software view controller: action")
                                   style:UIAlertActionStyleDestructive
                                 handler:^(UIAlertAction* __unused action) {
                                   [self confirmRemoveVersion:version];
                                 }]];
  [actions
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:nil]];
  [self presentViewController:actions animated:YES completion:nil];
}

- (void)useVersionForGames:(const xe::ui::IOSSystemVersion&)version {
  if (![self canChangeSystemSoftware]) {
    return;
  }
  busy_ = YES;
  [self.tableView reloadData];
  const std::filesystem::path version_path = version.path;
  const BOOL has_unrestored_files = info_.has_unrestored_files;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const bool changed = xe::ui::UseIOSSystemVersionForGames(version_path);
    dispatch_async(dispatch_get_main_queue(), ^{
      busy_ = NO;
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaIOSSystemSoftwareDidChangeNotification
                        object:nil];
      [self reloadInfo];
      if (!changed) {
        XEPresentOKAlert(self,
                         NSLocalizedString(@"Couldn't Change the Version",
                                           @"system software view controller: title"),
                         has_unrestored_files
                             ? xe::ui::IOSSystemSoftwareInstallFailureMessage(
                                   xe::ui::SystemFilesResult::kRestoreFailed)
                             : NSLocalizedString(@"Games still use the version they did.",
                                                 @"system software view controller: message"));
      }
    });
  });
}

- (void)confirmRemoveVersion:(const xe::ui::IOSSystemVersion&)version {
  NSString* message = nil;
  if (!version.is_default) {
    message =
        NSLocalizedString(@"Its Dashboard leaves the Library. Your games, profiles and saves stay.",
                          @"system software view controller: message");
  } else if (info_.has_unrestored_files) {
    message = NSLocalizedString(
        @"This also deletes the \"unrestored\" copy of the previous system software that a "
        @"failed install left. Games have no system software until you choose another version "
        @"for them. Your games, profiles and saves stay.",
        @"system software view controller: message");
  } else {
    message = NSLocalizedString(
        @"Games have no system software until you choose another version for them. Your "
        @"games, profiles and saves stay.",
        @"system software view controller: message");
  }
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:[NSString
                                   stringWithFormat:NSLocalizedString(
                                                        @"Remove %@?",
                                                        @"system software view controller: title; "
                                                        @"%@ is a version like 2.0.17559.0"),
                                                    [self nameOfVersion:version]]
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:nil]];
  const xe::ui::IOSSystemVersion removed = version;
  [confirm addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(
                                             @"Remove", @"system software view controller: action")
                                   style:UIAlertActionStyleDestructive
                                 handler:^(UIAlertAction* __unused action) {
                                   [self removeVersion:removed];
                                 }]];
  [self presentViewController:confirm animated:YES completion:nil];
}

- (void)removeVersion:(const xe::ui::IOSSystemVersion&)version {
  if (![self canChangeSystemSoftware]) {
    return;
  }
  busy_ = YES;
  [self.tableView reloadData];
  const std::filesystem::path version_path = version.path;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const bool removed = xe::ui::RemoveIOSSystemVersion(version_path);
    dispatch_async(dispatch_get_main_queue(), ^{
      busy_ = NO;
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaIOSSystemSoftwareDidChangeNotification
                        object:nil];
      [self reloadInfo];
      if (!removed) {
        XEPresentOKAlert(self,
                         NSLocalizedString(@"Couldn't Remove the Version",
                                           @"system software view controller: title"),
                         NSLocalizedString(@"Delete its folder in XeniOS's system-versions folder "
                                           @"in Files instead.",
                                           @"system software view controller: message"));
      }
    });
  });
}

#pragma mark - Remove

- (void)confirmRemove {
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Remove System Software?",
                                                 @"system software view controller: title")
                       message:info_.has_unrestored_files
                                   ? NSLocalizedString(
                                         @"This also deletes the \"unrestored\" copy of the "
                                         @"previous system software that a failed install left. "
                                         @"The Xbox 360 Dashboard won't start until you install "
                                         @"the system update again. Your games, profiles and "
                                         @"saves stay.",
                                         @"system software view controller: message")
                                   : NSLocalizedString(
                                         @"The Xbox 360 Dashboard won't start until you install "
                                         @"the system update again. Your games, profiles and "
                                         @"saves stay.",
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
