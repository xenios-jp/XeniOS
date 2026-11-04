/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_external_folders_view_controller.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/base/logging.h"

#import "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/shared/ios_theme.h"

NSString* const kXeniaIOSExternalLibraryDidChangeNotification =
    @"XeniaIOSExternalLibraryDidChangeNotification";

@interface XeniaIOSExternalFoldersViewController () <UIDocumentPickerDelegate>
@end

@implementation XeniaIOSExternalFoldersViewController {
  std::vector<xe::ui::IOSExternalLibraryLocation> folders_;
  // Set while the picker is up to re-link an unavailable folder.
  BOOL relinking_;
  std::filesystem::path relink_old_root_;
}

- (instancetype)init {
  return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)dealloc {
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"External Folders", @"external folders view controller: title");
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 64.0;

  self.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                           target:self
                           action:@selector(addFolderTapped:)] autorelease];

  // Presented modally as the root of its own navigation controller (from the
  // launcher's Add-to-Library sheet) we need a Done button; pushed onto an
  // existing stack (from Settings) the back button already handles dismissal.
  if (self.navigationController.viewControllers.firstObject == self) {
    self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
        initWithTitle:NSLocalizedString(@"Close", @"external folders view controller: button")
                style:UIBarButtonItemStylePlain
               target:self
               action:@selector(doneTapped:)] autorelease];
  }

  [self reloadFolders];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self reloadFolders];
}

// Shows the last known list at once and refreshes it in the background:
// resolving the bookmarks can block on an unreachable share or drive.
- (void)reloadFolders {
  std::vector<xe::ui::IOSExternalLibraryLocation> cached;
  if (xe::ui::CachedIOSExternalLibraryLocations(&cached)) {
    folders_ = std::move(cached);
  }
  if (self.isViewLoaded) {
    [self.tableView reloadData];
  }
  [self refreshFoldersInBackground];
}

- (void)refreshFoldersInBackground {
  [self retain];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    std::vector<xe::ui::IOSExternalLibraryLocation> fresh =
        xe::ui::ListIOSExternalLibraryLocations();
    dispatch_async(dispatch_get_main_queue(), ^{
      folders_ = std::move(fresh);
      if (self.isViewLoaded) {
        [self.tableView reloadData];
      }
      [self release];
    });
  });
}

- (void)notifyExternalLibraryChanged {
  [[NSNotificationCenter defaultCenter]
      postNotificationName:kXeniaIOSExternalLibraryDidChangeNotification
                    object:nil];
}

- (void)doneTapped:(__unused id)sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)addFolderTapped:(__unused id)sender {
  relinking_ = NO;
  [self presentFolderPicker];
}

- (void)presentFolderPicker {
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  [self presentViewController:picker animated:YES completion:nil];
  [picker release];
}

- (BOOL)unlinkExternalFolderAtPath:(const std::filesystem::path&)path {
  NSString* removed_name = nil;
  NSError* error = nil;
  if (!xe::ui::RemoveIOSExternalLibraryLocationAtRoot(path, &removed_name, &error)) {
    XEPresentOKAlert(
        self,
        NSLocalizedString(@"Unlink Failed", @"external folders view controller: user-visible text"),
        error.localizedDescription
            ?: NSLocalizedString(@"Could not unlink the selected folder.",
                                 @"external folders view controller: user-visible text"));
    return NO;
  }
  XELOGI("iOS external folders: unlinked '{}'", xe::strip_pii(path.string()));

  // Duplicate records resolving to the same root are all dropped at once, and
  // the list collapses to the empty-state row when nothing is left. Reload
  // rather than animate a single-row delete, which would assert on a row-count
  // mismatch.
  folders_.erase(std::remove_if(folders_.begin(), folders_.end(),
                                [&path](const xe::ui::IOSExternalLibraryLocation& folder) {
                                  return folder.path == path;
                                }),
                 folders_.end());
  [self reloadFolders];
  [self notifyExternalLibraryChanged];
  return YES;
}

#pragma mark - UIDocumentPickerDelegate

- (void)documentPicker:(__unused UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  NSURL* url = urls.firstObject;
  if (!url) {
    return;
  }
  const BOOL relinking = relinking_;
  const std::filesystem::path old_root = relink_old_root_;
  relinking_ = NO;
  relink_old_root_.clear();
  NSError* error = nil;
  // Linking first means a failed pick never loses the old entry.
  if (!xe::ui::SaveIOSExternalLibraryLocation(url, &error)) {
    XEPresentOKAlert(
        self,
        relinking ? NSLocalizedString(@"Re-link Failed",
                                      @"external folders view controller: user-visible text")
                  : NSLocalizedString(@"Link Failed",
                                      @"external folders view controller: user-visible text"),
        error.localizedDescription
            ?: NSLocalizedString(@"Could not link the selected folder.",
                                 @"external folders view controller: user-visible text"));
    return;
  }
  if (relinking && !old_root.empty() &&
      xe::ui::NormalizedIOSLibraryPath(old_root) !=
          xe::ui::NormalizedIOSLibraryPath(std::filesystem::path([url.path UTF8String]))) {
    xe::ui::RemoveIOSExternalLibraryLocationAtRoot(old_root, nullptr, nullptr);
  }
  XELOGI("iOS external folders: {} '{}'", relinking ? "re-linked" : "linked",
         xe::strip_pii(url.path ? [url.path UTF8String] : ""));
  [self notifyExternalLibraryChanged];
  [self reloadFolders];
}

- (void)documentPickerWasCancelled:(__unused UIDocumentPickerViewController*)controller {
  relinking_ = NO;
  relink_old_root_.clear();
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (folders_.empty() || indexPath.row >= static_cast<NSInteger>(folders_.size())) {
    return;
  }
  const xe::ui::IOSExternalLibraryLocation folder = folders_[static_cast<size_t>(indexPath.row)];
  if (folder.available) {
    return;
  }
  NSString* name = folder.name.empty()
                       ? NSLocalizedString(@"this folder",
                                           @"external folders view controller: user-visible text")
                       : ToNSString(folder.name);
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Folder Unavailable",
                                                 @"external folders view controller: title")
                       message:[NSString stringWithFormat:
                                             NSLocalizedString(
                                                 @"XeniOS can't reach \"%@\". If the "
                                                 @"drive or share is connected, pick the "
                                                 @"folder again to re-link it.",
                                                 @"external folders view controller: message"),
                                             name]
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel",
                                                         @"external folders view controller: title")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  const std::filesystem::path old_root = folder.path;
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Re-link",
                                                         @"external folders view controller: title")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 relinking_ = YES;
                                 relink_old_root_ = old_root;
                                 [self presentFolderPicker];
                               }]];
  [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark - Table view

- (NSInteger)numberOfSectionsInTableView:(__unused UITableView*)tableView {
  return 1;
}

- (NSInteger)tableView:(__unused UITableView*)tableView
    numberOfRowsInSection:(__unused NSInteger)section {
  return folders_.empty() ? 1 : static_cast<NSInteger>(folders_.size());
}

- (NSString*)tableView:(__unused UITableView*)tableView
    titleForHeaderInSection:(__unused NSInteger)section {
  return NSLocalizedString(@"Linked Folders",
                           @"external folders view controller: user-visible text");
}

- (NSString*)tableView:(__unused UITableView*)tableView
    titleForFooterInSection:(__unused NSInteger)section {
  return NSLocalizedString(
      @"Games in linked folders appear in your library without being copied. Unlinking a "
      @"folder only removes it from XeniOS; the files are not deleted. A folder shows as "
      @"Unavailable when its drive or network share is disconnected.",
      @"external folders view controller: user-visible text");
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (folders_.empty()) {
    static NSString* const kEmptyCellId = @"XeniaIOSExternalFoldersEmptyCell";
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kEmptyCellId];
    if (!cell) {
      cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                     reuseIdentifier:kEmptyCellId] autorelease];
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
      cell.textLabel.numberOfLines = 0;
      cell.detailTextLabel.numberOfLines = 0;
    }
    cell.textLabel.text = NSLocalizedString(@"No external folders linked",
                                            @"external folders view controller: label text");
    cell.textLabel.textColor = [XeniaTheme textPrimary];
    cell.detailTextLabel.text =
        NSLocalizedString(@"Tap + to link a folder from Files, a USB drive, or a network share.",
                          @"external folders view controller: detail text");
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    cell.imageView.image = nil;
    cell.accessoryType = UITableViewCellAccessoryNone;
    return cell;
  }

  static NSString* const kFolderCellId = @"XeniaIOSExternalFoldersFolderCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kFolderCellId];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kFolderCellId] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
  }
  const xe::ui::IOSExternalLibraryLocation& folder = folders_[static_cast<size_t>(indexPath.row)];
  cell.textLabel.text =
      folder.name.empty()
          ? NSLocalizedString(@"External Library", @"external folders view controller: label text")
          : ToNSString(folder.name);
  cell.textLabel.textColor = [XeniaTheme textPrimary];

  NSString* path_text = ToNSString(folder.path.string());
  cell.detailTextLabel.text =
      folder.available
          ? path_text
          : [NSString stringWithFormat:NSLocalizedString(
                                           @"Unavailable - tap to re-link\n%@",
                                           @"external folders view controller: detail text"),
                                       path_text];
  cell.selectionStyle =
      folder.available ? UITableViewCellSelectionStyleNone : UITableViewCellSelectionStyleDefault;
  cell.detailTextLabel.textColor =
      folder.available ? [XeniaTheme textSecondary] : [XeniaTheme statusWarning];

  UIImageSymbolConfiguration* config =
      [UIImageSymbolConfiguration configurationWithPointSize:22.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* symbol =
      [UIImage systemImageNamed:(folder.available ? @"externaldrive.fill"
                                                  : @"externaldrive.badge.exclamationmark")
              withConfiguration:config];
  cell.imageView.image = [symbol imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor = folder.available ? [XeniaTheme accent] : [XeniaTheme statusWarning];
  cell.accessoryType = UITableViewCellAccessoryNone;
  return cell;
}

- (BOOL)tableView:(__unused UITableView*)tableView canEditRowAtIndexPath:(NSIndexPath*)indexPath {
  return !folders_.empty() && indexPath.row < static_cast<NSInteger>(folders_.size());
}

- (UISwipeActionsConfiguration*)tableView:(__unused UITableView*)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (folders_.empty() || indexPath.row >= static_cast<NSInteger>(folders_.size())) {
    return nil;
  }
  // Capture the folder path now, not the index path: a reload between opening the
  // swipe and confirming it could otherwise unlink the wrong row.
  const std::filesystem::path path = folders_[static_cast<size_t>(indexPath.row)].path;
  __unsafe_unretained XeniaIOSExternalFoldersViewController* unsafe_self = self;
  UIContextualAction* unlink = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:NSLocalizedString(@"Unlink",
                                                  @"external folders view controller: title")
                        handler:^(__unused UIContextualAction* action, __unused UIView* sourceView,
                                  void (^completionHandler)(BOOL)) {
                          completionHandler([unsafe_self unlinkExternalFolderAtPath:path]);
                        }];
  return [UISwipeActionsConfiguration configurationWithActions:@[ unlink ]];
}

@end
