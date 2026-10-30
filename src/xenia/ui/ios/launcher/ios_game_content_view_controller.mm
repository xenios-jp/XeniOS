/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_content_view_controller.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <filesystem>
#include <system_error>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/xbox.h"

#import "xenia/ui/ios/launcher/ios_content_management.h"
#import "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/shared/ios_theme.h"

@implementation XeniaGameContentViewController {
  uint32_t title_id_;
  NSString* game_title_;
  id<XeniaGameContentHost> host_;  // not retained; presenter owns this sheet
  std::vector<IOSInstalledContentEntry> installed_content_;
}

- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                           host:(id<XeniaGameContentHost>)host {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    title_id_ = title_id;
    game_title_ = [title copy];
    host_ = host;
    self.title = @"Manage Content";
  }
  return self;
}

- (void)dealloc {
  [game_title_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.separatorInset = UIEdgeInsetsMake(0, 16, 0, 16);
  self.navigationItem.leftBarButtonItem =
      [[[UIBarButtonItem alloc] initWithTitle:@"Close"
                                        style:UIBarButtonItemStylePlain
                                       target:self
                                       action:@selector(doneTapped:)] autorelease];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self reloadInstalledContent];
}

- (void)doneTapped:(id)__unused sender {
  if (self.navigationController.presentingViewController &&
      self.navigationController.viewControllers.firstObject == self) {
    [self.navigationController dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  [self.navigationController popViewControllerAnimated:YES];
}

- (void)reloadInstalledContent {
  installed_content_ = xe_list_installed_content(title_id_);
  [self.tableView reloadData];
}

- (void)refreshLauncherContentState {
  if (host_) {
    [host_ refreshImportedGames];
  }
  [self reloadInstalledContent];
}

- (void)presentAddContentPicker {
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ]];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  picker.shouldShowFileExtensions = YES;
  [self presentViewController:picker animated:YES completion:nil];
  [picker release];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  if (section == 0) {
    return 1;
  }
  return installed_content_.empty() ? 1 : static_cast<NSInteger>(installed_content_.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  if (section == 0) {
    return @"Actions";
  }
  return @"Installed Content";
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (section == 0) {
    if (game_title_.length > 0) {
      return
          [NSString stringWithFormat:@"Install title updates or DLC packages for %@.", game_title_];
    }
    return @"Install title updates or DLC packages for this game.";
  }
  if (installed_content_.empty()) {
    return @"No title updates or DLC are installed for this title.";
  }
  return @"Linked DLC stays in its external folder. Swipe left on an installed entry to delete it.";
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 0) {
    static NSString* const kActionCellIdentifier = @"XeniaGameContentActionCell";
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kActionCellIdentifier];
    if (!cell) {
      cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                     reuseIdentifier:kActionCellIdentifier] autorelease];
    }
    cell.textLabel.text = @"Add Content";
    cell.textLabel.textColor = self.view.tintColor;
    cell.detailTextLabel.text = @"Import a title update or DLC package for this game.";
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    cell.imageView.image = [UIImage systemImageNamed:@"plus.circle.fill"];
    cell.imageView.tintColor = self.view.tintColor;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    return cell;
  }

  if (installed_content_.empty()) {
    static NSString* const kEmptyCellIdentifier = @"XeniaGameContentEmptyCell";
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kEmptyCellIdentifier];
    if (!cell) {
      cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                     reuseIdentifier:kEmptyCellIdentifier] autorelease];
    }
    cell.textLabel.text = @"No content installed";
    cell.detailTextLabel.text = @"Add a title update or DLC package from Files.";
    cell.textLabel.textColor = [XeniaTheme textMuted];
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
  }

  static NSString* const kContentCellIdentifier = @"XeniaGameContentCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kContentCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kContentCellIdentifier] autorelease];
  }

  const IOSInstalledContentEntry& entry = installed_content_[static_cast<size_t>(indexPath.row)];
  cell.textLabel.text = ToNSString(entry.name);
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text =
      entry.linked
          ? [NSString stringWithFormat:@"%@ - Linked", xe_installed_content_kind_label(entry.kind)]
          : xe_installed_content_kind_label(entry.kind);
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  return cell;
}

- (BOOL)tableView:(UITableView* __unused)tableView canEditRowAtIndexPath:(NSIndexPath*)indexPath {
  return indexPath.section == 1 && !installed_content_.empty();
}

- (void)tableView:(UITableView* __unused)tableView
    commitEditingStyle:(UITableViewCellEditingStyle)editingStyle
     forRowAtIndexPath:(NSIndexPath*)indexPath {
  if (editingStyle != UITableViewCellEditingStyleDelete || indexPath.section != 1 ||
      installed_content_.empty()) {
    return;
  }

  const IOSInstalledContentEntry& entry = installed_content_[static_cast<size_t>(indexPath.row)];
  const std::filesystem::path entry_path = entry.path;
  NSString* display_name = ToNSString(entry.name);
  NSString* confirm_title = entry.linked ? @"Unlink Content" : @"Delete Content";
  NSString* confirm_message =
      entry.linked
          ? [NSString stringWithFormat:@"Unlink \"%@\"? The source files in the external folder "
                                       @"will not be deleted.",
                                       display_name]
          : [NSString stringWithFormat:@"Delete \"%@\"? This cannot be undone.", display_name];
  NSString* action_title = entry.linked ? @"Unlink" : @"Delete";
  UIAlertController* confirm =
      [UIAlertController alertControllerWithTitle:confirm_title
                                          message:confirm_message
                                   preferredStyle:UIAlertControllerStyleAlert];
  [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
  [confirm
      addAction:[UIAlertAction
                    actionWithTitle:action_title
                              style:UIAlertActionStyleDestructive
                            handler:^(__unused UIAlertAction* action) {
                              std::error_code ec;
                              std::filesystem::remove_all(entry_path, ec);
                              if (ec) {
                                XEPresentOKAlert(
                                    self, @"Delete Failed",
                                    [NSString stringWithFormat:@"Failed deleting %@: %s",
                                                               display_name, ec.message().c_str()]);
                                return;
                              }
                              xe_remove_linked_content_marker(entry);
                              [self refreshLauncherContentState];
                            }]];
  [self presentViewController:confirm animated:YES completion:nil];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 0) {
    [self presentAddContentPicker];
  }
}

- (void)documentPicker:(UIDocumentPickerViewController* __unused)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  if (urls.count == 0) {
    return;
  }

  NSURL* url = urls[0];
  BOOL access_granted = [url startAccessingSecurityScopedResource];
  IOSSelectedContentPackage package_info;
  NSString* validation_error = nil;
  if (!xe_read_selected_content_package(std::filesystem::path([url.path UTF8String]), &package_info,
                                        &validation_error)) {
    if (access_granted) {
      [url stopAccessingSecurityScopedResource];
    }
    XEPresentOKAlert(self, @"Invalid Package",
                     validation_error ?: @"Could not read the selected content package.");
    return;
  }

  if (package_info.title_id != title_id_) {
    if (access_granted) {
      [url stopAccessingSecurityScopedResource];
    }
    XEPresentOKAlert(
        self, @"Wrong Game",
        [NSString stringWithFormat:@"This package is for title %08X, but the current game is %08X.",
                                   package_info.title_id, title_id_]);
    return;
  }

  BOOL install_success = NO;
  NSString* result_title = @"Unsupported Content";
  NSString* result_message = nil;
  switch (package_info.content_type) {
    case xe::XContentType::kInstaller: {
      NSString* status_message = nil;
      install_success = host_ && [host_ installTitleUpdateAtPath:url.path
                                                          status:&status_message
                                                  notTitleUpdate:nullptr];
      result_title = install_success ? @"Installed" : @"Install Failed";
      result_message = install_success ? (status_message ?: @"Title update installed successfully.")
                                       : (status_message ?: @"Title update installation failed.");
    } break;
    case xe::XContentType::kMarketplaceContent: {
      std::string error_message;
      BOOL matched_external_location = NO;
      NSError* external_access_error = nil;
      std::filesystem::path source_relative_path;
      XeniaIOSExternalLibraryAccess* external_access = xe::ui::StartIOSExternalLibraryAccessForPath(
          package_info.path, &matched_external_location, &source_relative_path,
          &external_access_error);
      if (matched_external_location && !external_access) {
        install_success = NO;
        result_title = @"Link Failed";
        result_message = external_access_error.localizedDescription
                             ?: @"XeniOS could not access the linked external folder.";
      } else if (external_access) {
        install_success = xe_link_content_package_into_root(
            package_info, xe_dlc_content_root(title_id_), source_relative_path, &error_message);
        result_title = install_success ? @"Linked" : @"Link Failed";
        result_message = install_success ? @"DLC linked from the external folder."
                                         : ToNSString(error_message.empty() ? "DLC linking failed."
                                                                            : error_message);
      } else {
        install_success = xe_copy_content_package_into_root(
            package_info, xe_dlc_content_root(title_id_), &error_message);
        result_title = install_success ? @"Installed" : @"Install Failed";
        result_message =
            install_success
                ? @"DLC installed successfully."
                : ToNSString(error_message.empty() ? "DLC installation failed." : error_message);
      }
    } break;
    default:
      result_message =
          [NSString stringWithFormat:@"Content type 0x%08X is not a title update or DLC.",
                                     static_cast<uint32_t>(package_info.content_type)];
      break;
  }

  if (access_granted) {
    [url stopAccessingSecurityScopedResource];
  }

  if (install_success) {
    [self refreshLauncherContentState];
  }
  XEPresentOKAlert(self, result_title, result_message);
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController* __unused)controller {
  XELOGI("iOS: Manage Content picker cancelled");
}

@end
