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
#include "xenia/vfs/xex_metadata.h"
#include "xenia/xbox.h"

#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/launcher/ios_content_management.h"
#import "xenia/ui/ios/launcher/ios_file_transfer.h"
#import "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/shared/ios_theme.h"

NSNotificationName const XeniaGameContentDidChangeNotification =
    @"XeniaGameContentDidChangeNotification";

void XeniaConfirmRemoveInstalledContent(UIViewController* presenter,
                                        const IOSInstalledContentEntry& entry, UIView* source,
                                        void (^removed)(void), void (^cancelled)(void)) {
  const IOSInstalledContentEntry entry_copy = entry;
  void (^removed_copy)(void) = [[removed copy] autorelease];
  void (^cancelled_copy)(void) = [[cancelled copy] autorelease];
  NSString* display_name = ToNSString(entry.name);
  NSString* confirm_title =
      entry.linked ? NSLocalizedString(@"Unlink Content", @"game content view controller: title")
                   : NSLocalizedString(@"Delete Content", @"game content view controller: title");
  NSString* confirm_message =
      entry.linked
          ? [NSString
                stringWithFormat:NSLocalizedString(
                                     @"Unlink \"%@\"? The source files in the external folder "
                                     @"will not be deleted.",
                                     @"game content view controller: message"),
                                 display_name]
          : [NSString stringWithFormat:NSLocalizedString(
                                           @"Delete \"%@\"? This cannot be undone.",
                                           @"game content view controller: user-visible text"),
                                       display_name];
  NSString* action_title =
      entry.linked ? NSLocalizedString(@"Unlink", @"game content view controller: title")
                   : NSLocalizedString(@"Delete", @"game content view controller: title");
  // Destructive confirmations are action sheets, anchored to the row on iPad.
  UIAlertController* confirm =
      [UIAlertController alertControllerWithTitle:confirm_title
                                          message:confirm_message
                                   preferredStyle:UIAlertControllerStyleActionSheet];
  UIPopoverPresentationController* popover = confirm.popoverPresentationController;
  if (popover) {
    popover.sourceView = source ?: presenter.view;
    popover.sourceRect = (source ?: presenter.view).bounds;
  }
  [confirm addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(@"Cancel",
                                                           @"game content view controller: title")
                                   style:UIAlertActionStyleCancel
                                 handler:^(__unused UIAlertAction* action) {
                                   if (cancelled_copy) {
                                     cancelled_copy();
                                   }
                                 }]];
  [confirm
      addAction:[UIAlertAction
                    actionWithTitle:action_title
                              style:UIAlertActionStyleDestructive
                            handler:^(__unused UIAlertAction* action) {
                              std::string remove_error;
                              if (!xe_remove_installed_content(entry_copy, &remove_error)) {
                                XEPresentOKAlert(
                                    presenter,
                                    NSLocalizedString(
                                        @"Delete Failed",
                                        @"game content view controller: user-visible text"),
                                    [NSString
                                        stringWithFormat:
                                            NSLocalizedString(
                                                @"Failed deleting %1$@: %2$s",
                                                @"game content view controller: user-visible text"),
                                            display_name, remove_error.c_str()]);
                                return;
                              }
                              [[NSNotificationCenter defaultCenter]
                                  postNotificationName:XeniaGameContentDidChangeNotification
                                                object:nil];
                              if (removed_copy) {
                                removed_copy();
                              }
                            }]];
  [presenter presentViewController:confirm animated:YES completion:nil];
}
UIImage* XeniaInstalledContentRowImage(const IOSInstalledContentEntry& entry, CGFloat side) {
  UIImage* thumbnail = xe_installed_content_thumbnail(entry);
  if (!thumbnail) {
    return XERowSymbolImage(entry.kind == IOSInstalledContentKind::kTitleUpdate
                                ? @"arrow.down.circle"
                                : @"puzzlepiece.extension",
                            nil);
  }
  const CGRect bounds = CGRectMake(0.0, 0.0, side, side);
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:bounds.size] autorelease];
  return [renderer imageWithActions:^(__unused UIGraphicsImageRendererContext* context) {
    [[UIBezierPath bezierPathWithRoundedRect:bounds cornerRadius:side * 0.18] addClip];
    [thumbnail drawInRect:bounds];
  }];
}

void XeniaPresentInstalledContentDetail(UIViewController* presenter,
                                        const IOSInstalledContentEntry& entry) {
  if (!presenter) {
    return;
  }
  XeniaInstalledContentDetailViewController* detail =
      [[[XeniaInstalledContentDetailViewController alloc] initWithEntry:entry] autorelease];
  XeniaLandscapeNavigationController* sheet =
      [[[XeniaLandscapeNavigationController alloc] initWithRootViewController:detail]
          autorelease];
  XEConfigureTaskSheet(sheet, presenter.view, CGSizeMake(540.0, 620.0), NO);
  [presenter presentViewController:sheet animated:YES completion:nil];
}

namespace {

// The rows of the detail page's second section: a label and its value.
struct XeniaContentDetailRow {
  NSString* label;
  NSString* value;
};

}  // namespace

@implementation XeniaInstalledContentDetailViewController {
  IOSInstalledContentEntry entry_;
  NSString* description_;
  std::vector<XeniaContentDetailRow> rows_;
}

- (instancetype)initWithEntry:(const IOSInstalledContentEntry&)entry {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    entry_ = entry;
    self.title = xe_installed_content_kind_label(entry.kind);
    description_ = entry.description.empty() ? nil : [ToNSString(entry.description) retain];
    auto add_row = [self](NSString* label, NSString* value) {
      if (value.length) {
        rows_.push_back({[label retain], [value retain]});
      }
    };
    if (!entry.publisher.empty()) {
      add_row(NSLocalizedString(@"Publisher", @"content details: row label"),
              ToNSString(entry.publisher));
    }
    if (entry.kind == IOSInstalledContentKind::kTitleUpdate && entry.version) {
      add_row(NSLocalizedString(@"Version", @"content details: row label"),
              ToNSString(xe::vfs::XexVersion::FromValue(entry.version).ToString()));
    }
    add_row(NSLocalizedString(@"File", @"content details: row label"), ToNSString(entry.name));
    if (entry.linked) {
      add_row(NSLocalizedString(@"Linked From", @"content details: row label"),
              ToNSString(entry.linked_relative_path.empty()
                             ? entry.linked_source_path.filename().string()
                             : entry.linked_relative_path.string()));
    }
  }
  return self;
}

- (void)dealloc {
  for (XeniaContentDetailRow& row : rows_) {
    [row.label release];
    [row.value release];
  }
  [description_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.allowsSelection = NO;
  // Pushed in Updates & Add-ons it has the sheet's ✕ already; presented, it gets its own.
  if (self.navigationController.viewControllers.firstObject == self) {
    XESetSheetClose(self, self, @selector(closeTapped:));
  }
}

- (void)closeTapped:(id)__unused sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

// The header, then the description when there is one, then the details.
- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 3;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  switch (section) {
    case 0:
      return 1;
    case 1:
      return description_ ? 1 : 0;
    default:
      return static_cast<NSInteger>(rows_.size());
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  if (section == 1 && description_) {
    return NSLocalizedString(@"Description", @"content details: section header");
  }
  if (section == 2 && !rows_.empty()) {
    return NSLocalizedString(@"Details", @"content details: section header");
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kDetailCellIdentifier = @"XeniaContentDetailCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kDetailCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kDetailCellIdentifier] autorelease];
  }
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  UIListContentConfiguration* content = nil;
  switch (indexPath.section) {
    case 0: {
      content = [UIListContentConfiguration subtitleCellConfiguration];
      content.image = XeniaInstalledContentRowImage(entry_, 86.0);
      content.imageProperties.maximumSize = CGSizeMake(86.0, 86.0);
      content.imageToTextPadding = 16.0;
      content.text = xe_installed_content_display_name(entry_);
      content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
      content.textProperties.color = [XeniaTheme textPrimary];
      content.secondaryText = xe_installed_content_detail(entry_);
      content.secondaryTextProperties.color = [XeniaTheme textSecondary];
      content.textToSecondaryTextVerticalPadding = 4.0;
      content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(12.0, 0.0, 12.0, 0.0);
    } break;
    case 1:
      content = [UIListContentConfiguration cellConfiguration];
      content.text = description_;
      content.textProperties.color = [XeniaTheme textPrimary];
      content.textProperties.numberOfLines = 0;
      break;
    default: {
      const XeniaContentDetailRow& row = rows_[static_cast<size_t>(indexPath.row)];
      content = [UIListContentConfiguration valueCellConfiguration];
      content.text = row.label;
      content.textProperties.color = [XeniaTheme textPrimary];
      content.secondaryText = row.value;
      content.secondaryTextProperties.color = [XeniaTheme textSecondary];
      content.secondaryTextProperties.numberOfLines = 0;
      content.prefersSideBySideTextAndSecondaryText = YES;
    } break;
  }
  cell.contentConfiguration = content;
  return cell;
}

@end

@implementation XeniaGameContentImporter {
  uint32_t title_id_;
  id<XeniaGameContentHost> host_;    // not retained
  UIViewController* presenter_;      // not retained
}

- (instancetype)initWithTitleID:(uint32_t)titleID
                           host:(id<XeniaGameContentHost>)host
                      presenter:(UIViewController*)presenter {
  self = [super init];
  if (self) {
    title_id_ = titleID;
    host_ = host;
    presenter_ = presenter;
  }
  return self;
}

- (void)present {
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ]];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  picker.shouldShowFileExtensions = YES;
  [presenter_ presentViewController:picker animated:YES completion:nil];
  [picker release];
}

- (void)documentPicker:(UIDocumentPickerViewController* __unused)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  if (urls.count == 0) {
    return;
  }

  NSURL* url = urls[0];
  // The scope stays open until the background work below has finished.
  BOOL access_granted = [url startAccessingSecurityScopedResource];
  XeniaIOSFileTransfer* transfer = [[XeniaIOSFileTransfer alloc] init];
  XeniaIOSTransferProgressAlert* progress_alert = [[XeniaIOSTransferProgressAlert
        presentFrom:presenter_
              title:NSLocalizedString(@"Adding Content", @"game content view controller: title")
            message:NSLocalizedString(@"Checking the package...",
                                      @"game content view controller: message")
      cancelHandler:^{
        [transfer cancel];
      }] retain];
  const uint32_t title_id = title_id_;
  id<XeniaGameContentHost> host = host_;
  UIViewController* presenter = presenter_;

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    BOOL install_success = NO;
    BOOL cancelled = NO;
    NSString* result_title =
        NSLocalizedString(@"Unsupported Content", @"game content view controller: title");
    NSString* result_message = nil;
    @autoreleasepool {
      IOSSelectedContentPackage package_info;
      NSString* validation_error = nil;
      if (!xe_read_selected_content_package(std::filesystem::path([url.path UTF8String]),
                                            &package_info, &validation_error)) {
        result_title =
            NSLocalizedString(@"Invalid Package", @"game content view controller: title");
        result_message = validation_error
                             ?: NSLocalizedString(@"Could not read the selected content package.",
                                                  @"game content view controller: message");
      } else if (package_info.title_id != title_id) {
        result_title = NSLocalizedString(@"Wrong Game", @"game content view controller: title");
        result_message = [NSString
            stringWithFormat:NSLocalizedString(
                                 @"This package is for title %1$08X, but the current game is %2$08X.",
                                 @"game content view controller: message"),
                             package_info.title_id, title_id];
      } else {
        switch (package_info.content_type) {
          case xe::XContentType::kInstaller: {
            // The emulator copies and installs the update itself, so there is
            // no byte progress and nothing to cancel here.
            dispatch_async(dispatch_get_main_queue(), ^{
              [progress_alert setCancelAvailable:NO];
              [progress_alert updateCopiedBytes:0 totalBytes:0];
            });
            NSString* status_message = nil;
            install_success = host && [host installTitleUpdateAtPath:url.path
                                                              status:&status_message
                                                      notTitleUpdate:nullptr];
            result_title =
                install_success
                    ? NSLocalizedString(@"Installed", @"game content view controller: title")
                    : NSLocalizedString(@"Install Failed", @"game content view controller: title");
            result_message =
                install_success
                    ? (status_message
                           ?: NSLocalizedString(@"Title update installed successfully.",
                                                @"game content view controller: message"))
                    : (status_message
                           ?: NSLocalizedString(@"Title update installation failed.",
                                                @"game content view controller: message"));
          } break;
          case xe::XContentType::kMarketplaceContent: {
            std::string error_message;
            BOOL matched_external_location = NO;
            NSError* external_access_error = nil;
            std::filesystem::path source_relative_path;
            XeniaIOSExternalLibraryAccess* external_access =
                xe::ui::StartIOSExternalLibraryAccessForPath(
                    package_info.path, &matched_external_location, &source_relative_path,
                    &external_access_error);
            if (matched_external_location && !external_access) {
              result_title =
                  NSLocalizedString(@"Link Failed", @"game content view controller: title");
              result_message =
                  external_access_error.localizedDescription
                      ?: NSLocalizedString(@"XeniOS could not access the linked external folder.",
                                           @"game content view controller: message");
            } else if (external_access) {
              install_success = xe_link_content_package_into_root(
                  package_info, xe_dlc_content_root(title_id), source_relative_path,
                  &error_message);
              result_title =
                  install_success
                      ? NSLocalizedString(@"Linked", @"game content view controller: title")
                      : NSLocalizedString(@"Link Failed", @"game content view controller: title");
              result_message =
                  install_success
                      ? NSLocalizedString(@"DLC linked from the external folder.",
                                          @"game content view controller: message")
                      : ToNSString(error_message.empty() ? "DLC linking failed." : error_message);
            } else {
              dispatch_async(dispatch_get_main_queue(), ^{
                [progress_alert updateCopiedBytes:0 totalBytes:1];
              });
              install_success = xe_copy_content_package_into_root(
                  package_info, xe_dlc_content_root(title_id), transfer,
                  ^(uint64_t copied, uint64_t total) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                      [progress_alert updateCopiedBytes:copied totalBytes:total];
                    });
                  },
                  &error_message);
              cancelled = !install_success && transfer.cancelled;
              result_title =
                  install_success
                      ? NSLocalizedString(@"Installed", @"game content view controller: title")
                      : NSLocalizedString(@"Install Failed",
                                          @"game content view controller: title");
              // On success, error_message carries a warning about skipped folder entries.
              if (install_success) {
                result_message = error_message.empty()
                                     ? NSLocalizedString(@"DLC installed successfully.",
                                                         @"game content view controller: message")
                                     : [NSLocalizedString(@"DLC installed. ",
                                                          @"game content view controller: message")
                                           stringByAppendingString:ToNSString(error_message)];
              } else {
                result_message = ToNSString(error_message.empty() ? "DLC installation failed."
                                                                  : error_message);
              }
            }
          } break;
          default:
            result_message =
                [NSString stringWithFormat:NSLocalizedString(
                                               @"Content type 0x%08X is not a title update or DLC.",
                                               @"game content view controller: message"),
                                           static_cast<uint32_t>(package_info.content_type)];
            break;
        }
      }
      [result_title retain];
      [result_message retain];
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      if (access_granted) {
        [url stopAccessingSecurityScopedResource];
      }
      [progress_alert dismissWithCompletion:^{
        if (install_success) {
          // The library scan walks every linked folder, so the host does it off the main thread.
          [host refreshImportedGamesAsync];
          [[NSNotificationCenter defaultCenter]
              postNotificationName:XeniaGameContentDidChangeNotification
                            object:nil];
        }
        if (!cancelled) {
          XEPresentOKAlert(presenter, result_title, result_message);
        }
        [result_title release];
        [result_message release];
        [progress_alert release];
        [transfer release];
      }];
    });
  });
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController* __unused)controller {
  XELOGI("iOS: Manage Content picker cancelled");
}

@end

@implementation XeniaGameContentViewController {
  uint32_t title_id_;
  NSString* game_title_;
  id<XeniaGameContentHost> host_;  // not retained; presenter owns this sheet
  std::vector<IOSInstalledContentEntry> installed_content_;
  uint64_t content_reload_generation_;
  XeniaGameContentImporter* importer_;
}

- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                           host:(id<XeniaGameContentHost>)host {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    title_id_ = title_id;
    game_title_ = [title copy];
    host_ = host;
    self.title = NSLocalizedString(@"Updates & Add-ons", @"game content view controller: title");
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [importer_ release];
  [game_title_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.separatorInset = UIEdgeInsetsMake(0, 16, 0, 16);
  XESetSheetClose(self, self, @selector(doneTapped:));
  UIBarButtonItem* add = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                           target:self
                           action:@selector(addTapped:)] autorelease];
  add.accessibilityLabel =
      NSLocalizedString(@"Add from Files", @"game content view controller: add button");
  XESetSheetActions(self, @[ add ]);
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(contentChanged:)
                                               name:XeniaGameContentDidChangeNotification
                                             object:nil];
}

- (void)addTapped:(id)__unused sender {
  [self presentAddContentPicker];
}

// Nothing installed: one centred message and the screen's one primary button.
- (void)updateContentUnavailableConfigurationUsingState:(UIContentUnavailableConfigurationState*)
                                                            __unused state {
  if (!installed_content_.empty() || content_reload_generation_ == 0) {
    self.contentUnavailableConfiguration = nil;
    return;
  }
  UIContentUnavailableConfiguration* empty = [UIContentUnavailableConfiguration emptyConfiguration];
  empty.image = [UIImage systemImageNamed:@"square.stack.3d.up"];
  empty.text = NSLocalizedString(@"No Updates or Add-ons",
                                 @"game content view controller: empty title");
  empty.secondaryText =
      game_title_.length
          ? [NSString stringWithFormat:NSLocalizedString(
                                           @"Add title updates and DLC for %@ from Files.",
                                           @"game content view controller: empty detail"),
                                       game_title_]
          : NSLocalizedString(@"Add title updates and DLC for this game from Files.",
                              @"game content view controller: empty detail");
  UIButtonConfiguration* button = [UIButtonConfiguration filledButtonConfiguration];
  button.title = NSLocalizedString(@"Add from Files", @"game content view controller: add button");
  button.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  button.baseBackgroundColor = [UIColor whiteColor];
  button.baseForegroundColor = [UIColor blackColor];
  button.contentInsets = NSDirectionalEdgeInsetsMake(10.0, 22.0, 10.0, 22.0);
  button.titleTextAttributesTransformer =
      ^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
        NSMutableDictionary* out = [[in mutableCopy] autorelease];
        out[NSFontAttributeName] = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        return out;
      };
  empty.button = button;
  __unsafe_unretained XeniaGameContentViewController* unsafe_self = self;
  empty.buttonProperties.primaryAction = [UIAction actionWithHandler:^(__unused UIAction* action) {
    [unsafe_self presentAddContentPicker];
  }];
  self.contentUnavailableConfiguration = empty;
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

// Walks the content tree off the main thread, then reloads the table. A newer
// reload supersedes one still in flight.
- (void)reloadInstalledContent {
  const uint64_t generation = ++content_reload_generation_;
  const uint32_t title_id = title_id_;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    std::vector<IOSInstalledContentEntry> content = xe_list_installed_content(title_id);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (generation != content_reload_generation_) {
        return;
      }
      installed_content_ = std::move(content);
      [self.tableView reloadData];
      [self setNeedsUpdateContentUnavailableConfiguration];
    });
  });
}

- (void)refreshLauncherContentState {
  // The library scan walks every linked folder, so the host does it off the
  // main thread.
  [host_ refreshImportedGamesAsync];
}

- (void)presentAddContentPicker {
  if (!importer_) {
    importer_ = [[XeniaGameContentImporter alloc] initWithTitleID:title_id_
                                                             host:host_
                                                        presenter:self];
  }
  [importer_ present];
}

- (void)contentChanged:(NSNotification* __unused)note {
  [self reloadInstalledContent];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return installed_content_.empty() ? 0 : 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
    numberOfRowsInSection:(NSInteger __unused)section {
  return static_cast<NSInteger>(installed_content_.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger __unused)section {
  return NSLocalizedString(
      @"Swipe left on an item to delete it. Linked DLC stays in its folder.",
      @"game content view controller: user-visible text");
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kContentCellIdentifier = @"XeniaGameContentCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kContentCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kContentCellIdentifier] autorelease];
  }

  const IOSInstalledContentEntry& entry = installed_content_[static_cast<size_t>(indexPath.row)];
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.image = XeniaInstalledContentRowImage(entry, 44.0);
  content.imageProperties.maximumSize = CGSizeMake(44.0, 44.0);
  content.text = xe_installed_content_display_name(entry);
  content.textProperties.color = [XeniaTheme textPrimary];
  content.secondaryText = xe_installed_content_detail(entry);
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  cell.contentConfiguration = content;
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  return cell;
}

- (BOOL)tableView:(UITableView* __unused)tableView canEditRowAtIndexPath:(NSIndexPath*)indexPath {
  return indexPath.section == 0 && !installed_content_.empty();
}

- (void)tableView:(UITableView*)tableView
    commitEditingStyle:(UITableViewCellEditingStyle)editingStyle
     forRowAtIndexPath:(NSIndexPath*)indexPath {
  if (editingStyle != UITableViewCellEditingStyleDelete || indexPath.section != 0 ||
      installed_content_.empty()) {
    return;
  }

  const IOSInstalledContentEntry& entry = installed_content_[static_cast<size_t>(indexPath.row)];
  XeniaConfirmRemoveInstalledContent(
      self, entry, [tableView cellForRowAtIndexPath:indexPath] ?: tableView,
      ^{
        [self refreshLauncherContentState];
      },
      ^{
        [tableView setEditing:NO animated:YES];
      });
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section != 0 ||
      static_cast<size_t>(indexPath.row) >= installed_content_.size()) {
    return;
  }
  XeniaInstalledContentDetailViewController* detail =
      [[[XeniaInstalledContentDetailViewController alloc]
          initWithEntry:installed_content_[static_cast<size_t>(indexPath.row)]] autorelease];
  [self.navigationController pushViewController:detail animated:YES];
}

@end
