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
}

- (instancetype)init {
  return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)dealloc {
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = @"External Folders";
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
    self.navigationItem.leftBarButtonItem =
        [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                       target:self
                                                       action:@selector(doneTapped:)] autorelease];
  }

  [self reloadFolders];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self reloadFolders];
}

- (void)reloadFolders {
  folders_ = xe::ui::ListIOSExternalLibraryLocations();
  if (self.isViewLoaded) {
    [self.tableView reloadData];
  }
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
    XEPresentOKAlert(self, @"Unlink Failed",
                     error.localizedDescription ?: @"Could not unlink the selected folder.");
    return NO;
  }
  XELOGI("iOS external folders: unlinked '{}'", path.string());

  // Duplicate records resolving to the same root are all dropped at once, and
  // the list collapses to the empty-state row when nothing is left. Reload
  // rather than animate a single-row delete, which would assert on a row-count
  // mismatch.
  folders_ = xe::ui::ListIOSExternalLibraryLocations();
  [self.tableView reloadData];
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
  NSError* error = nil;
  if (!xe::ui::SaveIOSExternalLibraryLocation(url, &error)) {
    XEPresentOKAlert(self, @"Link Failed",
                     error.localizedDescription ?: @"Could not link the selected folder.");
    return;
  }
  XELOGI("iOS external folders: linked '{}'", url.path ? [url.path UTF8String] : "");
  [self notifyExternalLibraryChanged];
  [self reloadFolders];
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
  return @"Linked Folders";
}

- (NSString*)tableView:(__unused UITableView*)tableView
    titleForFooterInSection:(__unused NSInteger)section {
  return @"Games in linked folders appear in your library without being copied. Unlinking a "
         @"folder only removes it from XeniOS; the files are not deleted. A folder shows as "
         @"Unavailable when its drive or network share is disconnected.";
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
    cell.textLabel.text = @"No external folders linked";
    cell.textLabel.textColor = [XeniaTheme textPrimary];
    cell.detailTextLabel.text =
        @"Tap + to link a folder from Files, a USB drive, or a network share.";
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
  cell.textLabel.text = folder.name.empty() ? @"External Library" : ToNSString(folder.name);
  cell.textLabel.textColor = [XeniaTheme textPrimary];

  NSString* path_text = ToNSString(folder.path.string());
  cell.detailTextLabel.text =
      folder.available ? path_text : [NSString stringWithFormat:@"Unavailable - %@", path_text];
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
                          title:@"Unlink"
                        handler:^(__unused UIContextualAction* action, __unused UIView* sourceView,
                                  void (^completionHandler)(BOOL)) {
                          completionHandler([unsafe_self unlinkExternalFolderAtPath:path]);
                        }];
  return [UISwipeActionsConfiguration configurationWithActions:@[ unlink ]];
}

@end
