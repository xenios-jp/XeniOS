/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_layout_ui_coordinator_ios.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/hid/touch/touch_layout_ios.h"

#import "xenia/ui/ios/shared/ios_view_helpers.h"
#import "xenia/ui/ios/touch/touch_layout_library_ios.h"
#import "xenia/ui/ios/touch/touch_layout_library_view_ios.h"
#import "xenia/ui/ios/touch/touch_layout_store_ios.h"

using xe::ui::IOSTouchLocalLayoutInfo;
using xe::ui::DefaultOfficialTouchLayoutLocalID;
using xe::ui::IsFavoriteTouchLayoutLocalID;
using xe::ui::IsOfficialTouchLayoutLocalID;
using xe::ui::MakeOfficialIOSTouchLayoutModel;
using xe::ui::MakeOfficialIOSTouchLayoutModelForLocalID;
using xe::ui::MakeTouchLayoutSeedModelForTable;
using xe::ui::MakeTouchLayoutSlug;
using xe::ui::NormalizeOfficialTouchLayoutBaseTemplate;
using xe::ui::ReadGlobalTouchLayoutAssignment;
using xe::ui::ReadTitleTouchLayoutAssignment;
using xe::ui::RenderTouchLayoutThumbnail;
using xe::ui::SetFavoriteTouchLayoutLocalID;
using xe::ui::TouchLayoutContentMatches;
using xe::ui::TryNormalizeConfiguredTouchLayoutLocalID;
using xe::ui::WriteGlobalTouchLayoutAssignment;
using xe::ui::kOfficialTouchLayoutFPSCompactLocalID;
using xe::ui::kOfficialTouchLayoutFPSFullLocalID;
using xe::ui::kTouchLayoutAssignmentSection;

namespace {

constexpr NSUInteger kXeniaIOSTouchLayoutMaxBytes = 64 * 1024;
constexpr NSUInteger kXeniaIOSTouchLayoutURLMaxLength = 2048;

bool TouchLayoutControlsMatch(
    const xe::hid::touch::IOSTouchLayoutModel& layout,
    const xe::hid::touch::IOSTouchLayoutModel& reference_layout) {
  xe::hid::touch::IOSTouchLayoutModel comparable_layout = layout;
  comparable_layout.layout_id = reference_layout.layout_id;
  comparable_layout.display_name = reference_layout.display_name;
  comparable_layout.author = reference_layout.author;
  comparable_layout.base_template = reference_layout.base_template;
  return TouchLayoutContentMatches(comparable_layout, reference_layout);
}

std::string DeviceDefaultAssignmentForStaleInlineLayout(
    const xe::hid::touch::IOSTouchLayoutModel& layout) {
  const std::string default_layout_id = DefaultOfficialTouchLayoutLocalID();
  if (default_layout_id != kOfficialTouchLayoutFPSFullLocalID) {
    return std::string();
  }

  if (TouchLayoutControlsMatch(
          layout,
          MakeOfficialIOSTouchLayoutModelForLocalID(
              kOfficialTouchLayoutFPSCompactLocalID)) ||
      TouchLayoutControlsMatch(
          layout, MakeOfficialIOSTouchLayoutModelForLocalID(default_layout_id))) {
    return default_layout_id;
  }
  return std::string();
}

std::string OfficialResetLayoutIDForRuntimeLayout(
    const xe::hid::touch::IOSTouchLayoutModel* layout) {
  const std::string default_layout_id = DefaultOfficialTouchLayoutLocalID();
  const std::string base_template = NormalizeOfficialTouchLayoutBaseTemplate(
      layout ? layout->base_template : std::string());
  if (default_layout_id == kOfficialTouchLayoutFPSFullLocalID &&
      base_template == kOfficialTouchLayoutFPSCompactLocalID) {
    return default_layout_id;
  }
  return base_template;
}

}  // namespace

@implementation XeniaIOSTouchLayoutUICoordinator {
  id<XeniaIOSTouchLayoutUICoordinatorHost> _host;
  NSData* _pendingInstallBytes;
  NSString* _pendingInstallSuggestedName;
}

- (instancetype)initWithHost:(id<XeniaIOSTouchLayoutUICoordinatorHost>)host {
  if (!(self = [super init])) {
    return nil;
  }
  _host = host;
  return self;
}

- (void)dealloc {
  [_pendingInstallBytes release];
  [_pendingInstallSuggestedName release];
  [super dealloc];
}

- (BOOL)hasPendingInstall {
  return _pendingInstallBytes != nil;
}

- (xe::hid::touch::IOSTouchRuntimeModel*)runtimeModel {
  return [_host touchLayoutCoordinatorRuntimeModel];
}

- (std::filesystem::path)touchLayoutPathForLocalID:(const std::string&)local_id {
  return xe::ui::IOSTouchLayoutPathForLocalID(local_id);
}

- (BOOL)writeTouchLayoutModel:(const xe::hid::touch::IOSTouchLayoutModel&)layout
                         path:(const std::filesystem::path&)layout_path
                        error:(NSString**)error_out {
  return xe::ui::WriteIOSTouchLayoutModel(layout, layout_path, error_out) ? YES : NO;
}

- (BOOL)loadTouchLayoutModelAtPath:(const std::filesystem::path&)layout_path
                             model:(xe::hid::touch::IOSTouchLayoutModel*)layout_out
                             error:(NSString**)error_out {
  return xe::ui::LoadIOSTouchLayoutModelAtPath(layout_path, layout_out, error_out) ? YES : NO;
}

- (std::vector<IOSTouchLocalLayoutInfo>)availableLocalTouchLayouts {
  return xe::ui::AvailableLocalIOSTouchLayouts();
}

- (std::string)uniqueTouchLayoutLocalIDForBaseName:(NSString*)base_name {
  return xe::ui::UniqueIOSTouchLayoutLocalIDForBaseName(base_name, std::string());
}

- (std::string)uniqueTouchLayoutLocalIDForBaseName:(NSString*)base_name
                                  excludingLocalID:(const std::string&)existing_local_id {
  return xe::ui::UniqueIOSTouchLayoutLocalIDForBaseName(base_name, existing_local_id);
}

- (BOOL)findTouchLayoutInfoWithLocalID:(NSString*)localID
                                  info:(IOSTouchLocalLayoutInfo*)info_out {
  if (!localID.length || !info_out) {
    return NO;
  }
  std::string selected_local_id([localID UTF8String]);
  std::vector<IOSTouchLocalLayoutInfo> layouts = [self availableLocalTouchLayouts];
  for (const IOSTouchLocalLayoutInfo& info : layouts) {
    if (info.local_id == selected_local_id) {
      *info_out = info;
      return YES;
    }
  }
  return NO;
}

- (std::string)currentTouchLayoutLibrarySelectionLocalID {
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  return runtime_model ? runtime_model->layout().layout_id : std::string();
}

- (NSArray<XeniaTouchLayoutLibraryItem*>*)touchLayoutLibraryItemsForOverlay {
  std::vector<IOSTouchLocalLayoutInfo> layouts = [self availableLocalTouchLayouts];
  const uint32_t active_title_id = [_host touchLayoutCoordinatorActiveTitleID];
  const std::string title_default_local_id = ReadTitleTouchLayoutAssignment(active_title_id);
  const std::string global_default_local_id = ReadGlobalTouchLayoutAssignment();
  NSMutableArray<XeniaTouchLayoutLibraryItem*>* items =
      [NSMutableArray arrayWithCapacity:layouts.size()];
  for (const IOSTouchLocalLayoutInfo& info : layouts) {
    XeniaTouchLayoutLibraryItem* item = [[[XeniaTouchLayoutLibraryItem alloc] init] autorelease];
    item.localID = ToNSString(info.local_id);
    item.displayName = ToNSString(info.display_name);
    item.author = info.author.empty() ? @"" : ToNSString(info.author);
    item.official = info.official;
    item.thumbnail = RenderTouchLayoutThumbnail(info.layout, CGSizeMake(120.0, 68.0));
    item.isDefaultForCurrentTitle =
        !title_default_local_id.empty() && title_default_local_id == info.local_id;
    item.isDefaultForAllGames =
        !global_default_local_id.empty() && global_default_local_id == info.local_id;
    item.isFavorite = IsFavoriteTouchLayoutLocalID(info.local_id);
    [items addObject:item];
  }
  return items;
}

- (void)refreshTouchLayoutLibraryOverlayIfVisible {
  if (![_host touchLayoutCoordinatorIsShowingLayoutLibrary]) {
    return;
  }
  [_host touchLayoutCoordinatorShowLayoutLibraryWithItems:[self touchLayoutLibraryItemsForOverlay]
                                     currentLayoutLocalID:ToNSString(
                                                              [self currentTouchLayoutLibrarySelectionLocalID])];
}

- (void)applyDefaultLayoutModel {
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  if (!runtime_model) {
    return;
  }

  const std::string global_default_id = ReadGlobalTouchLayoutAssignment();
  if (!global_default_id.empty()) {
    if (IsOfficialTouchLayoutLocalID(global_default_id)) {
      [_host touchLayoutCoordinatorSetActiveLocalID:global_default_id];
      runtime_model->SetLayout(MakeOfficialIOSTouchLayoutModelForLocalID(global_default_id));
      [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
      return;
    }

    xe::hid::touch::IOSTouchLayoutModel global_default_layout;
    NSString* error_message = nil;
    if ([self loadTouchLayoutModelAtPath:[self touchLayoutPathForLocalID:global_default_id]
                                   model:&global_default_layout
                                   error:&error_message]) {
      [_host touchLayoutCoordinatorSetActiveLocalID:global_default_id];
      runtime_model->SetLayout(std::move(global_default_layout));
      [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
      return;
    }
    WriteGlobalTouchLayoutAssignment(std::string());
  }

  const std::string default_layout_id = DefaultOfficialTouchLayoutLocalID();
  [_host touchLayoutCoordinatorSetActiveLocalID:default_layout_id];
  runtime_model->SetLayout(
      MakeOfficialIOSTouchLayoutModelForLocalID(default_layout_id));
  [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
}

- (void)applyLayoutModelForTitleID:(uint32_t)title_id {
  [self applyDefaultLayoutModel];
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  if (!title_id || !runtime_model) {
    return;
  }

  toml::table config_table = config::LoadGameConfig(title_id);
  const toml::table* assignment_table = config_table[kTouchLayoutAssignmentSection].as_table();
  if (assignment_table) {
    if (auto local_layout_id = (*assignment_table)["local_layout_id"].value<std::string>()) {
      std::string normalized_local_id;
      if (!TryNormalizeConfiguredTouchLayoutLocalID(*local_layout_id, &normalized_local_id)) {
        XELOGW("iOS: ignoring invalid assigned touch layout id {}", *local_layout_id);
      } else if (IsOfficialTouchLayoutLocalID(normalized_local_id)) {
        [_host touchLayoutCoordinatorSetActiveLocalID:normalized_local_id];
        runtime_model->SetLayout(MakeOfficialIOSTouchLayoutModelForLocalID(normalized_local_id));
        [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
        [self saveCurrentLayoutForTitleID:title_id];
        return;
      } else {
        xe::hid::touch::IOSTouchLayoutModel local_layout;
        NSString* error_message = nil;
        if ([self loadTouchLayoutModelAtPath:[self touchLayoutPathForLocalID:normalized_local_id]
                                       model:&local_layout
                                       error:&error_message]) {
          [_host touchLayoutCoordinatorSetActiveLocalID:normalized_local_id];
          runtime_model->SetLayout(std::move(local_layout));
          [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
          [self saveCurrentLayoutForTitleID:title_id];
          return;
        }
        XELOGW("iOS: assigned touch layout {} missing or invalid: {}", normalized_local_id,
               error_message ? error_message.UTF8String : "unknown error");
      }
    }
  }

  const toml::table* touch_layout_table = config_table["TouchLayout"].as_table();
  if (!touch_layout_table) {
    return;
  }

  xe::hid::touch::IOSTouchLayoutModel layout = MakeTouchLayoutSeedModelForTable(*touch_layout_table);
  if (!xe::hid::touch::ApplyIOSTouchLayoutModel(*touch_layout_table, &layout)) {
    return;
  }

  const std::string migrated_assignment_id =
      DeviceDefaultAssignmentForStaleInlineLayout(layout);
  if (!migrated_assignment_id.empty()) {
    [_host touchLayoutCoordinatorSetActiveLocalID:migrated_assignment_id];
    runtime_model->SetLayout(MakeOfficialIOSTouchLayoutModelForLocalID(migrated_assignment_id));
    [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
    [self saveCurrentLayoutForTitleID:title_id];
    return;
  }

  [_host touchLayoutCoordinatorSetActiveLocalID:std::string()];
  runtime_model->SetLayout(std::move(layout));
  [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
}

- (void)saveCurrentLayoutForTitleID:(uint32_t)title_id {
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  if (!title_id || !runtime_model) {
    return;
  }

  toml::table config_table = config::LoadGameConfig(title_id);
  const xe::hid::touch::IOSTouchLayoutModel& layout = runtime_model->layout();

  std::string assignment_local_id;
  const std::string active_local_id = [_host touchLayoutCoordinatorActiveLocalID];
  if (!active_local_id.empty()) {
    xe::hid::touch::IOSTouchLayoutModel source_layout;
    bool source_loaded = false;
    if (IsOfficialTouchLayoutLocalID(active_local_id)) {
      source_layout = MakeOfficialIOSTouchLayoutModelForLocalID(active_local_id);
      source_loaded = true;
    } else {
      const std::filesystem::path source_path = [self touchLayoutPathForLocalID:active_local_id];
      NSString* source_error = nil;
      source_loaded =
          [self loadTouchLayoutModelAtPath:source_path model:&source_layout error:&source_error];
    }
    if (source_loaded && TouchLayoutContentMatches(layout, source_layout)) {
      assignment_local_id = active_local_id;
    }
  }

  if (!assignment_local_id.empty()) {
    toml::table assignment_table;
    assignment_table.insert_or_assign("local_layout_id", assignment_local_id);
    config_table.insert_or_assign(kTouchLayoutAssignmentSection, std::move(assignment_table));
    config_table.erase("TouchLayout");
  } else {
    config_table.insert_or_assign("TouchLayout", xe::hid::touch::EncodeIOSTouchLayoutModel(layout));
    config_table.erase(kTouchLayoutAssignmentSection);
  }

  try {
    config::SaveGameConfig(title_id, config_table);
  } catch (const std::exception& e) {
    XELOGE("iOS: failed to save touch layout for title {:08X}: {}", title_id, e.what());
  }
}

- (void)writeTouchLayoutAssignmentLocalID:(const std::string&)local_id
                               forTitleID:(uint32_t)title_id {
  if (!title_id || local_id.empty()) {
    return;
  }
  toml::table config_table = config::LoadGameConfig(title_id);
  toml::table assignment_table;
  assignment_table.insert_or_assign("local_layout_id", local_id);
  config_table.insert_or_assign(kTouchLayoutAssignmentSection, std::move(assignment_table));
  config_table.erase("TouchLayout");
  try {
    config::SaveGameConfig(title_id, config_table);
  } catch (const std::exception& e) {
    XELOGE("iOS: failed to update touch layout assignment for title {:08X}: {}", title_id,
           e.what());
  }
}

- (void)clearTouchLayoutAssignmentForTitleID:(uint32_t)title_id {
  if (!title_id) {
    return;
  }
  toml::table config_table = config::LoadGameConfig(title_id);
  config_table.erase(kTouchLayoutAssignmentSection);
  try {
    config::SaveGameConfig(title_id, config_table);
  } catch (const std::exception& e) {
    XELOGE("iOS: failed to clear touch layout assignment for title {:08X}: {}", title_id,
           e.what());
  }
}

- (void)presentLibrary {
  if (![self runtimeModel]) {
    return;
  }
  [_host touchLayoutCoordinatorShowLayoutLibraryWithItems:[self touchLayoutLibraryItemsForOverlay]
                                     currentLayoutLocalID:ToNSString(
                                                              [self currentTouchLayoutLibrarySelectionLocalID])];
}

- (void)applyLayoutWithLocalID:(NSString*)localID {
  if (!localID.length) {
    return;
  }
  std::string selected_local_id([localID UTF8String]);
  std::vector<IOSTouchLocalLayoutInfo> layouts = [self availableLocalTouchLayouts];
  for (const IOSTouchLocalLayoutInfo& info : layouts) {
    if (info.local_id == selected_local_id) {
      [self applyTouchLayoutInfo:info];
      return;
    }
  }
  [_host touchLayoutCoordinatorSetStatusText:@"Selected layout is no longer available."];
}

- (void)applyTouchLayoutInfo:(const IOSTouchLocalLayoutInfo&)info {
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  if (!runtime_model) {
    return;
  }

  xe::hid::touch::IOSTouchLayoutModel layout;
  NSString* error_message = nil;
  if (![self loadTouchLayoutModelAtPath:info.path model:&layout error:&error_message]) {
    [_host touchLayoutCoordinatorSetStatusText:error_message ?: @"Layout could not be loaded."];
    return;
  }

  [_host touchLayoutCoordinatorSetActiveLocalID:info.local_id];
  runtime_model->SetLayout(std::move(layout));
  [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
  [self refreshTouchLayoutLibraryOverlayIfVisible];
  [self saveCurrentLayoutForTitleID:[_host touchLayoutCoordinatorActiveTitleID]];
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"Applied %@.", ToNSString(info.display_name)]];
}

- (void)presentRenameSheet {
  std::vector<IOSTouchLocalLayoutInfo> local_layouts = [self availableLocalTouchLayouts];
  local_layouts.erase(
      std::remove_if(local_layouts.begin(), local_layouts.end(),
                     [](const IOSTouchLocalLayoutInfo& info) { return info.official; }),
      local_layouts.end());
  if (local_layouts.empty()) {
    [_host touchLayoutCoordinatorSetStatusText:@"No saved local layouts to rename."];
    return;
  }

  UIAlertController* sheet =
      [UIAlertController alertControllerWithTitle:@"Rename Saved Layout"
                                          message:@"Choose a local layout to rename."
                                   preferredStyle:UIAlertControllerStyleActionSheet];
  __unsafe_unretained XeniaIOSTouchLayoutUICoordinator* unsafe_self = self;
  for (const IOSTouchLocalLayoutInfo& info : local_layouts) {
    IOSTouchLocalLayoutInfo selected_info = info;
    NSString* current_name = ToNSString(selected_info.display_name);
    [sheet
        addAction:
            [UIAlertAction
                actionWithTitle:current_name
                          style:UIAlertActionStyleDefault
                        handler:^(__unused UIAlertAction* action) {
                          [unsafe_self renameLayout:selected_info currentName:current_name];
                        }]];
  }
  [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];

  [self presentActionSheet:sheet];
}

- (void)renameLayoutWithLocalID:(NSString*)localID {
  IOSTouchLocalLayoutInfo selected_info;
  if (![self findTouchLayoutInfoWithLocalID:localID info:&selected_info]) {
    [_host touchLayoutCoordinatorSetStatusText:@"Selected layout is no longer available."];
    return;
  }
  if (selected_info.official) {
    [_host touchLayoutCoordinatorSetStatusText:@"Official presets cannot be renamed."];
    return;
  }
  [self renameLayout:selected_info currentName:ToNSString(selected_info.display_name)];
}

- (void)renameLayout:(const IOSTouchLocalLayoutInfo&)selected_info
         currentName:(NSString*)current_name {
  __unsafe_unretained XeniaIOSTouchLayoutUICoordinator* unsafe_self = self;
  [_host touchLayoutCoordinatorPresentKeyboardPromptWithTitle:@"Rename Layout"
                                                  description:@"Enter a new name for the saved "
                                                              @"local layout."
                                                  defaultText:current_name
                                                   completion:^(BOOL cancelled, NSString* text) {
                                                     [unsafe_self finishRenameLayout:selected_info
                                                                                 text:text
                                                                            cancelled:cancelled];
                                                   }];
}

- (void)finishRenameLayout:(const IOSTouchLocalLayoutInfo&)selected_info
                      text:(NSString*)text
                 cancelled:(BOOL)cancelled {
  if (cancelled) {
    return;
  }
  NSString* trimmed =
      [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if (trimmed.length == 0) {
    return;
  }

  xe::hid::touch::IOSTouchLayoutModel layout;
  NSString* error_message = nil;
  if (![self loadTouchLayoutModelAtPath:selected_info.path model:&layout error:&error_message]) {
    [_host touchLayoutCoordinatorSetStatusText:error_message ?: @"Layout could not be loaded."];
    return;
  }

  std::string next_local_id = [self uniqueTouchLayoutLocalIDForBaseName:trimmed
                                                       excludingLocalID:selected_info.local_id];
  const std::filesystem::path next_path = [self touchLayoutPathForLocalID:next_local_id];
  layout.layout_id = next_local_id;
  layout.display_name = std::string([trimmed UTF8String]);
  if (layout.author.empty()) {
    layout.author = "Local";
  }

  if (![self writeTouchLayoutModel:layout path:next_path error:&error_message]) {
    [_host touchLayoutCoordinatorSetStatusText:error_message ?: @"Failed to rename saved layout."];
    return;
  }

  if (next_local_id != selected_info.local_id) {
    std::error_code remove_ec;
    std::filesystem::remove(selected_info.path, remove_ec);
    if (remove_ec) {
      [_host touchLayoutCoordinatorSetStatusText:
                 [NSString stringWithFormat:@"Renamed %@, but the old file could not be removed.",
                                            trimmed]];
      return;
    }
  }

  if ([_host touchLayoutCoordinatorActiveLocalID] == selected_info.local_id) {
    [_host touchLayoutCoordinatorSetActiveLocalID:std::string()];
  }
  if (ReadGlobalTouchLayoutAssignment() == selected_info.local_id) {
    WriteGlobalTouchLayoutAssignment(next_local_id);
  }
  if (IsFavoriteTouchLayoutLocalID(selected_info.local_id)) {
    SetFavoriteTouchLayoutLocalID(selected_info.local_id, false);
    SetFavoriteTouchLayoutLocalID(next_local_id, true);
  }
  const uint32_t active_title_id = [_host touchLayoutCoordinatorActiveTitleID];
  if (ReadTitleTouchLayoutAssignment(active_title_id) == selected_info.local_id) {
    [self writeTouchLayoutAssignmentLocalID:next_local_id forTitleID:active_title_id];
  }

  [self refreshTouchLayoutLibraryOverlayIfVisible];
  [_host touchLayoutCoordinatorSetStatusText:[NSString stringWithFormat:@"Renamed %@.", trimmed]];
}

- (void)presentDeleteSheet {
  std::vector<IOSTouchLocalLayoutInfo> local_layouts = [self availableLocalTouchLayouts];
  local_layouts.erase(
      std::remove_if(local_layouts.begin(), local_layouts.end(),
                     [](const IOSTouchLocalLayoutInfo& info) { return info.official; }),
      local_layouts.end());
  if (local_layouts.empty()) {
    [_host touchLayoutCoordinatorSetStatusText:@"No saved local layouts to delete."];
    return;
  }

  UIAlertController* sheet =
      [UIAlertController alertControllerWithTitle:@"Delete Saved Layout"
                                          message:@"Choose a local layout to delete."
                                   preferredStyle:UIAlertControllerStyleActionSheet];
  __unsafe_unretained XeniaIOSTouchLayoutUICoordinator* unsafe_self = self;
  for (const IOSTouchLocalLayoutInfo& info : local_layouts) {
    IOSTouchLocalLayoutInfo selected_info = info;
    NSString* current_name = ToNSString(selected_info.display_name);
    [sheet
        addAction:
            [UIAlertAction
                actionWithTitle:current_name
                          style:UIAlertActionStyleDestructive
                        handler:^(__unused UIAlertAction* action) {
                          [unsafe_self confirmDeleteLayout:selected_info currentName:current_name];
                        }]];
  }
  [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];

  [self presentActionSheet:sheet];
}

- (void)deleteLayoutWithLocalID:(NSString*)localID {
  IOSTouchLocalLayoutInfo selected_info;
  if (![self findTouchLayoutInfoWithLocalID:localID info:&selected_info]) {
    [_host touchLayoutCoordinatorSetStatusText:@"Selected layout is no longer available."];
    return;
  }
  if (selected_info.official) {
    [_host touchLayoutCoordinatorSetStatusText:@"Official presets cannot be deleted."];
    return;
  }
  [self confirmDeleteLayout:selected_info currentName:ToNSString(selected_info.display_name)];
}

- (void)confirmDeleteLayout:(const IOSTouchLocalLayoutInfo&)selected_info
                currentName:(NSString*)current_name {
  UIAlertController* confirm =
      [UIAlertController alertControllerWithTitle:@"Delete Layout?"
                                          message:@"This removes the saved local layout file."
                                   preferredStyle:UIAlertControllerStyleAlert];
  [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
  __unsafe_unretained XeniaIOSTouchLayoutUICoordinator* unsafe_self = self;
  [confirm
      addAction:[UIAlertAction actionWithTitle:@"Delete"
                                         style:UIAlertActionStyleDestructive
                                       handler:^(__unused UIAlertAction* delete_action) {
                                         [unsafe_self deleteLayout:selected_info
                                                       currentName:current_name];
                                       }]];
  [[_host touchLayoutCoordinatorTopPresenter] presentViewController:confirm
                                                           animated:YES
                                                         completion:nil];
}

- (void)deleteLayout:(const IOSTouchLocalLayoutInfo&)selected_info
         currentName:(NSString*)current_name {
  std::error_code remove_ec;
  const bool removed = std::filesystem::remove(selected_info.path, remove_ec);
  if (remove_ec || !removed) {
    [_host touchLayoutCoordinatorSetStatusText:@"Failed to delete saved layout."];
    return;
  }

  if ([_host touchLayoutCoordinatorActiveLocalID] == selected_info.local_id) {
    [_host touchLayoutCoordinatorSetActiveLocalID:std::string()];
  }
  if (ReadGlobalTouchLayoutAssignment() == selected_info.local_id) {
    WriteGlobalTouchLayoutAssignment(std::string());
  }
  SetFavoriteTouchLayoutLocalID(selected_info.local_id, false);
  const uint32_t active_title_id = [_host touchLayoutCoordinatorActiveTitleID];
  if (ReadTitleTouchLayoutAssignment(active_title_id) == selected_info.local_id) {
    [self clearTouchLayoutAssignmentForTitleID:active_title_id];
  }

  [self refreshTouchLayoutLibraryOverlayIfVisible];
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"Deleted %@.", current_name]];
}

- (void)saveCurrentLayoutCopy {
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  if (!runtime_model) {
    return;
  }

  NSString* suggested_name = runtime_model->layout().display_name.empty()
                                 ? @"Custom Layout"
                                 : ToNSString(runtime_model->layout().display_name);
  __unsafe_unretained XeniaIOSTouchLayoutUICoordinator* unsafe_self = self;
  [_host touchLayoutCoordinatorPresentKeyboardPromptWithTitle:@"Save Layout Copy"
                                                  description:@"Enter a name for the saved local "
                                                              @"layout."
                                                  defaultText:suggested_name
                                                   completion:^(BOOL cancelled, NSString* text) {
                                                     [unsafe_self finishSaveLayoutCopy:text
                                                                             cancelled:cancelled];
                                                   }];
}

- (void)finishSaveLayoutCopy:(NSString*)text cancelled:(BOOL)cancelled {
  if (cancelled) {
    return;
  }
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  if (!runtime_model) {
    return;
  }

  NSString* trimmed =
      [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if (trimmed.length == 0) {
    return;
  }

  std::string local_id = [self uniqueTouchLayoutLocalIDForBaseName:trimmed];
  xe::hid::touch::IOSTouchLayoutModel layout = runtime_model->layout();
  layout.layout_id = local_id;
  layout.display_name = std::string([trimmed UTF8String]);
  layout.author = "Local";
  layout.base_template = NormalizeOfficialTouchLayoutBaseTemplate(layout.base_template);

  NSString* error_message = nil;
  if (![self writeTouchLayoutModel:layout
                              path:[self touchLayoutPathForLocalID:local_id]
                             error:&error_message]) {
    [_host touchLayoutCoordinatorSetStatusText:error_message ?: @"Failed to save layout copy."];
    return;
  }

  [self refreshTouchLayoutLibraryOverlayIfVisible];
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"Saved copy %@.", trimmed]];
}

- (void)importFromFile {
  if ([_host touchLayoutCoordinatorGameRunning]) {
    [_host touchLayoutCoordinatorSetGameplayModalPresentationPending:YES];
    [_host touchLayoutCoordinatorUpdateTouchOverlayVisibilityAnimated:YES];
  }
  [_host touchLayoutCoordinatorOpenTouchLayoutFileImportPicker];
}

- (void)importLayoutAtURL:(NSURL*)url {
  BOOL touch_access_granted = [url startAccessingSecurityScopedResource];
  NSString* error_message = nil;
  xe::hid::touch::IOSTouchLayoutModel imported_layout;
  BOOL ok = [self loadTouchLayoutModelAtPath:std::filesystem::path([url.path UTF8String])
                                       model:&imported_layout
                                       error:&error_message];
  if (touch_access_granted) {
    [url stopAccessingSecurityScopedResource];
  }
  if (!ok) {
    [_host touchLayoutCoordinatorSetStatusText:error_message ?: @"Failed to import touch layout."];
  } else if ([self runtimeModel]) {
    std::string base_name = imported_layout.display_name;
    if (base_name.empty()) {
      NSString* fallback_name = [[url lastPathComponent] stringByDeletingPathExtension];
      base_name =
          fallback_name.length ? std::string([fallback_name UTF8String]) : "imported_layout";
    }
    std::string local_id = [self uniqueTouchLayoutLocalIDForBaseName:ToNSString(base_name)];
    imported_layout.layout_id = local_id;
    if (imported_layout.author.empty()) {
      imported_layout.author = "Imported";
    }
    imported_layout.base_template =
        NormalizeOfficialTouchLayoutBaseTemplate(imported_layout.base_template);
    NSString* write_error = nil;
    if ([self writeTouchLayoutModel:imported_layout
                               path:[self touchLayoutPathForLocalID:local_id]
                              error:&write_error]) {
      [_host touchLayoutCoordinatorSetActiveLocalID:std::string()];
      [self runtimeModel]->SetLayout(imported_layout);
      [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
      [self refreshTouchLayoutLibraryOverlayIfVisible];
      [self saveCurrentLayoutForTitleID:[_host touchLayoutCoordinatorActiveTitleID]];
      [_host touchLayoutCoordinatorSetStatusText:
                 [NSString stringWithFormat:@"Imported %@.",
                                            ToNSString(imported_layout.display_name)]];
    } else {
      [_host touchLayoutCoordinatorSetStatusText:write_error ?: @"Failed to save imported touch layout."];
    }
  }
  [_host touchLayoutCoordinatorSetGameplayModalPresentationPending:NO];
  [_host touchLayoutCoordinatorUpdateTouchOverlayVisibilityAnimated:YES];
}

- (void)exportTouchLayoutModel:(const xe::hid::touch::IOSTouchLayoutModel&)layout
                  fallbackName:(NSString*)fallback_name {
  std::string display_name = layout.display_name;
  if (display_name.empty() && fallback_name.length) {
    display_name = std::string([fallback_name UTF8String]);
  }
  std::string base_name =
      display_name.empty() ? "touch_layout" : MakeTouchLayoutSlug(display_name);
  std::filesystem::path export_path = std::filesystem::path([NSTemporaryDirectory() UTF8String]) /
                                      (base_name + ".touchlayout.toml");
  NSString* error_message = nil;
  if (![self writeTouchLayoutModel:layout path:export_path error:&error_message]) {
    [_host touchLayoutCoordinatorSetStatusText:error_message ?: @"Failed to export touch layout."];
    return;
  }

  if ([_host touchLayoutCoordinatorGameRunning]) {
    [_host touchLayoutCoordinatorSetGameplayModalPresentationPending:YES];
    [_host touchLayoutCoordinatorUpdateTouchOverlayVisibilityAnimated:YES];
  }

  UIActivityViewController* activity_controller = [[UIActivityViewController alloc]
      initWithActivityItems:@[ [NSURL fileURLWithPath:ToNSString(export_path.string())] ]
      applicationActivities:nil];
  __unsafe_unretained id<XeniaIOSTouchLayoutUICoordinatorHost> host = _host;
  activity_controller.completionWithItemsHandler =
      ^(__unused UIActivityType activity_type, __unused BOOL completed,
        __unused NSArray* returned_items, __unused NSError* activity_error) {
        [host touchLayoutCoordinatorSetGameplayModalPresentationPending:NO];
        [host touchLayoutCoordinatorUpdateTouchOverlayVisibilityAnimated:YES];
      };
  [self presentActionSheet:activity_controller];
  [activity_controller release];
}

- (void)exportCurrentLayout {
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  if (!runtime_model) {
    return;
  }
  [self exportTouchLayoutModel:runtime_model->layout() fallbackName:nil];
}

- (void)exportLayoutWithLocalID:(NSString*)localID {
  IOSTouchLocalLayoutInfo selected_info;
  if (![self findTouchLayoutInfoWithLocalID:localID info:&selected_info]) {
    [_host touchLayoutCoordinatorSetStatusText:@"Selected layout is no longer available."];
    return;
  }
  [self exportTouchLayoutModel:selected_info.layout
                  fallbackName:ToNSString(selected_info.display_name)];
}

- (void)setLayoutDefaultForCurrentTitleWithLocalID:(NSString*)localID {
  if (![_host touchLayoutCoordinatorActiveTitleID]) {
    [_host touchLayoutCoordinatorSetStatusText:@"No active game title for this layout default."];
    return;
  }
  IOSTouchLocalLayoutInfo selected_info;
  if (![self findTouchLayoutInfoWithLocalID:localID info:&selected_info]) {
    [_host touchLayoutCoordinatorSetStatusText:@"Selected layout is no longer available."];
    return;
  }
  [self applyTouchLayoutInfo:selected_info];
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"Default for this game is %@.",
                                        ToNSString(selected_info.display_name)]];
}

- (void)setLayoutDefaultForAllGamesWithLocalID:(NSString*)localID {
  IOSTouchLocalLayoutInfo selected_info;
  if (![self findTouchLayoutInfoWithLocalID:localID info:&selected_info]) {
    [_host touchLayoutCoordinatorSetStatusText:@"Selected layout is no longer available."];
    return;
  }
  WriteGlobalTouchLayoutAssignment(selected_info.local_id);
  [self refreshTouchLayoutLibraryOverlayIfVisible];
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"Default for all games is %@.",
                                        ToNSString(selected_info.display_name)]];
}

- (void)setLayoutFavoriteWithLocalID:(NSString*)localID favorite:(BOOL)favorite {
  IOSTouchLocalLayoutInfo selected_info;
  if (![self findTouchLayoutInfoWithLocalID:localID info:&selected_info]) {
    [_host touchLayoutCoordinatorSetStatusText:@"Selected layout is no longer available."];
    return;
  }
  SetFavoriteTouchLayoutLocalID(selected_info.local_id, favorite);
  [self refreshTouchLayoutLibraryOverlayIfVisible];
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"%@ %@.",
                                        favorite ? @"Favorited" : @"Unfavorited",
                                        ToNSString(selected_info.display_name)]];
}

- (void)resetToOfficialPreset {
  xe::ui::EnsureOfficialIOSTouchLayoutPresets();
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  const std::string base_template = OfficialResetLayoutIDForRuntimeLayout(
      runtime_model ? &runtime_model->layout() : nullptr);
  const xe::hid::touch::IOSTouchLayoutModel preset_layout =
      MakeOfficialIOSTouchLayoutModelForLocalID(base_template);
  [_host touchLayoutCoordinatorSetActiveLocalID:base_template];
  if (runtime_model) {
    runtime_model->SetLayout(preset_layout);
  }
  [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
  [self refreshTouchLayoutLibraryOverlayIfVisible];
  [self saveCurrentLayoutForTitleID:[_host touchLayoutCoordinatorActiveTitleID]];
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"Restored %@.", ToNSString(preset_layout.display_name)]];
}

- (BOOL)handleExternalFileURL:(NSURL*)url {
  if (!url || ![url isFileURL]) {
    return NO;
  }
  NSString* last_component = url.lastPathComponent ?: @"";
  NSString* lower_name = last_component.lowercaseString;
  static NSArray<NSString*>* const game_extensions =
      @[ @".iso", @".xex", @".zar", @".god", @".xbe", @".xbox", @".xcp", @".001" ];
  for (NSString* ext in game_extensions) {
    if ([lower_name hasSuffix:ext]) {
      return NO;
    }
  }

  BOOL access_granted = [url startAccessingSecurityScopedResource];
  NSError* read_error = nil;
  NSData* file_bytes = nil;
  NSNumber* file_size = nil;
  if ([url getResourceValue:&file_size forKey:NSURLFileSizeKey error:nil] && file_size &&
      file_size.unsignedLongLongValue > kXeniaIOSTouchLayoutMaxBytes) {
    if (access_granted) {
      [url stopAccessingSecurityScopedResource];
    }
    return NO;
  }
  file_bytes = [NSData dataWithContentsOfURL:url options:0 error:&read_error];
  if (access_granted) {
    [url stopAccessingSecurityScopedResource];
  }
  if (!file_bytes || file_bytes.length == 0 || file_bytes.length > kXeniaIOSTouchLayoutMaxBytes) {
    return NO;
  }

  std::string_view bytes_view(static_cast<const char*>(file_bytes.bytes), file_bytes.length);
  xe::hid::touch::IOSTouchLayoutModel sniff_layout;
  try {
    toml::table table = toml::parse(bytes_view);
    sniff_layout = MakeTouchLayoutSeedModelForTable(table);
    if (!xe::hid::touch::ApplyIOSTouchLayoutModel(table, &sniff_layout)) {
      return NO;
    }
  } catch (const std::exception&) {
    return NO;
  }

  NSString* suggested_name = [last_component stringByDeletingPathExtension];
  if ([suggested_name.lowercaseString hasSuffix:@".touchlayout"]) {
    suggested_name = [suggested_name stringByDeletingPathExtension];
  }
  XELOGD("iOS: Touch layout sniff matched; stashing install for {}",
         suggested_name.length ? suggested_name.UTF8String : "<unnamed>");

  [_pendingInstallBytes release];
  _pendingInstallBytes = [file_bytes retain];
  [_pendingInstallSuggestedName release];
  _pendingInstallSuggestedName = [suggested_name retain];
  [self presentPendingInstallIfReady];
  return YES;
}

- (void)presentPendingInstallIfReady {
  if (!_pendingInstallBytes || ![_host touchLayoutCoordinatorCanPresentPendingInstall]) {
    return;
  }

  NSData* bytes = [[_pendingInstallBytes retain] autorelease];
  NSString* suggested = [[_pendingInstallSuggestedName retain] autorelease];
  [_pendingInstallBytes release];
  _pendingInstallBytes = nil;
  [_pendingInstallSuggestedName release];
  _pendingInstallSuggestedName = nil;

  NSString* display_for_prompt = suggested.length ? suggested : @"this layout";
  NSString* confirm_message =
      [NSString stringWithFormat:@"Install \"%@\" into your XeniOS touch layout library?",
                                 display_for_prompt];
  XELOGD("iOS: Presenting touch layout install confirm for {}", display_for_prompt.UTF8String);
  NSString* error_message = nil;
  if (![self installTouchLayoutFromTOMLBytes:bytes
                               suggestedName:suggested
                                confirmTitle:@"Install Touch Layout"
                              confirmMessage:confirm_message
                                confirmLabel:@"Install"
                                       error:&error_message]) {
    if (error_message.length) {
      [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout" message:error_message];
    }
  }
}

- (BOOL)handleExternalSchemeURL:(NSURL*)url {
  if (!url) {
    return NO;
  }
  NSString* scheme = url.scheme.lowercaseString;
  NSString* host = url.host.lowercaseString;
  if (![scheme isEqualToString:@"xenios"] || ![host isEqualToString:@"touchlayout"]) {
    return NO;
  }
  NSString* absolute = url.absoluteString ?: @"";
  if (absolute.length > kXeniaIOSTouchLayoutURLMaxLength) {
    [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout"
                                              message:@"This install link is too long to be safe "
                                                      @"and was rejected."];
    return YES;
  }

  NSURLComponents* components = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
  NSString* source_param = nil;
  NSString* file_param = nil;
  for (NSURLQueryItem* item in components.queryItems) {
    if ([item.name isEqualToString:@"source"]) {
      source_param = item.value;
    } else if ([item.name isEqualToString:@"file"]) {
      file_param = item.value;
    }
  }

  if (file_param.length) {
    NSData* decoded = [[[NSData alloc]
        initWithBase64EncodedString:file_param
                            options:NSDataBase64DecodingIgnoreUnknownCharacters] autorelease];
    if (!decoded || decoded.length == 0) {
      [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout"
                                                message:@"This install link's payload could not be "
                                                        @"decoded."];
      return YES;
    }
    if (decoded.length > kXeniaIOSTouchLayoutMaxBytes) {
      [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout"
                                                message:@"The embedded layout is larger than 64 KB "
                                                        @"and was rejected."];
      return YES;
    }
    NSString* error_message = nil;
    if (![self installTouchLayoutFromTOMLBytes:decoded
                                 suggestedName:@"shared_layout"
                                  confirmTitle:@"Install Touch Layout"
                                confirmMessage:@"Install the touch layout from this link?"
                                  confirmLabel:@"Install"
                                         error:&error_message]) {
      if (error_message.length) {
        [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout" message:error_message];
      }
    }
    return YES;
  }

  if (source_param.length) {
    NSURL* source_url = [NSURL URLWithString:source_param];
    if (!source_url || ![source_url.scheme.lowercaseString isEqualToString:@"https"] ||
        source_url.host.length == 0) {
      [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout"
                                                message:@"Layout downloads must use a valid "
                                                        @"https:// URL."];
      return YES;
    }
    if (source_param.length > kXeniaIOSTouchLayoutURLMaxLength) {
      [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout"
                                                message:@"The download URL is too long to be safe "
                                                        @"and was rejected."];
      return YES;
    }

    NSString* host_label = source_url.host ?: @"the source";
    NSString* prompt_message =
        [NSString stringWithFormat:@"Download a touch layout from %@?", host_label];
    UIAlertController* confirm =
        [UIAlertController alertControllerWithTitle:@"Touch Layout"
                                            message:prompt_message
                                     preferredStyle:UIAlertControllerStyleAlert];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                                style:UIAlertActionStyleCancel
                                              handler:nil]];
    __unsafe_unretained XeniaIOSTouchLayoutUICoordinator* unsafe_self = self;
    [confirm addAction:[UIAlertAction actionWithTitle:@"Download"
                                                style:UIAlertActionStyleDefault
                                              handler:^(__unused UIAlertAction* _action) {
                                                [unsafe_self downloadTouchLayoutFromURL:source_url
                                                                              hostLabel:host_label];
                                              }]];
    [[_host touchLayoutCoordinatorTopPresenter] presentViewController:confirm
                                                             animated:YES
                                                           completion:nil];
    return YES;
  }

  [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout"
                                            message:@"This link is missing a 'source' URL or "
                                                    @"'file' payload."];
  return YES;
}

- (void)downloadTouchLayoutFromURL:(NSURL*)source_url hostLabel:(NSString*)host_label {
  if (!source_url) {
    return;
  }

  NSString* display_host = host_label.length ? host_label : (source_url.host ?: @"the source");
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"Downloading touch layout from %@...", display_host]];

  NSMutableURLRequest* request =
      [NSMutableURLRequest requestWithURL:source_url
                              cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                          timeoutInterval:20.0];
  [request setValue:@"text/plain, application/toml, */*;q=0.1" forHTTPHeaderField:@"Accept"];

  __unsafe_unretained XeniaIOSTouchLayoutUICoordinator* unsafe_self = self;
  NSURLSessionDownloadTask* task = [[NSURLSession sharedSession]
      downloadTaskWithRequest:request
            completionHandler:^(NSURL* location, NSURLResponse* response, NSError* download_error) {
              [unsafe_self finishDownloadedTouchLayoutFromURL:source_url
                                                    hostLabel:display_host
                                                     location:location
                                                     response:response
                                                        error:download_error];
            }];
  [task resume];
}

- (void)finishDownloadedTouchLayoutFromURL:(NSURL*)source_url
                                 hostLabel:(NSString*)display_host
                                  location:(NSURL*)location
                                  response:(NSURLResponse*)response
                                     error:(NSError*)download_error {
  NSString* failure = nil;
  NSData* response_data = nil;

  if (download_error || !location) {
    failure = download_error.localizedDescription ?: @"The touch layout could not be downloaded.";
  }

  if (!failure && [response isKindOfClass:[NSHTTPURLResponse class]]) {
    NSHTTPURLResponse* http_response = (NSHTTPURLResponse*)response;
    if (http_response.statusCode < 200 || http_response.statusCode >= 300) {
      failure =
          [NSString stringWithFormat:@"Download failed with HTTP %ld.",
                                     static_cast<long>(http_response.statusCode)];
    }
  }

  if (!failure && response.expectedContentLength > 0 &&
      static_cast<unsigned long long>(response.expectedContentLength) >
          kXeniaIOSTouchLayoutMaxBytes) {
    failure = @"The downloaded layout is larger than 64 KB and was rejected.";
  }

  if (!failure) {
    NSError* file_error = nil;
    NSDictionary* attributes =
        [[NSFileManager defaultManager] attributesOfItemAtPath:location.path error:&file_error];
    NSNumber* file_size = [attributes objectForKey:NSFileSize];
    if (!file_size) {
      failure = file_error.localizedDescription ?: @"The downloaded layout could not be inspected.";
    } else if (file_size.unsignedLongLongValue == 0) {
      failure = @"The downloaded layout was empty.";
    } else if (file_size.unsignedLongLongValue > kXeniaIOSTouchLayoutMaxBytes) {
      failure = @"The downloaded layout is larger than 64 KB and was rejected.";
    }
  }

  if (!failure) {
    NSError* read_error = nil;
    response_data = [NSData dataWithContentsOfURL:location options:0 error:&read_error];
    if (!response_data || response_data.length == 0) {
      failure = read_error.localizedDescription ?: @"The downloaded layout could not be read.";
    } else if (response_data.length > kXeniaIOSTouchLayoutMaxBytes) {
      failure = @"The downloaded layout is larger than 64 KB and was rejected.";
      response_data = nil;
    }
  }

  dispatch_async(dispatch_get_main_queue(), ^{
    if (failure.length || !response_data) {
      NSString* message = failure.length ? failure : @"The touch layout could not be downloaded.";
      [_host touchLayoutCoordinatorSetStatusText:message];
      [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout" message:message];
      return;
    }

    NSString* suggested = source_url.lastPathComponent.stringByDeletingPathExtension;
    if ([suggested.lowercaseString hasSuffix:@".touchlayout"]) {
      suggested = [suggested stringByDeletingPathExtension];
    }
    if (!suggested.length) {
      suggested = @"downloaded_layout";
    }

    NSString* install_message =
        [NSString stringWithFormat:@"Install the touch layout from %@?", display_host];
    NSString* error_message = nil;
    if (![self installTouchLayoutFromTOMLBytes:response_data
                                 suggestedName:suggested
                                  confirmTitle:@"Install Touch Layout"
                                confirmMessage:install_message
                                  confirmLabel:@"Install"
                                         error:&error_message]) {
      if (error_message.length) {
        [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout" message:error_message];
      }
    }
  });
}

- (BOOL)installTouchLayoutFromTOMLBytes:(NSData*)bytes
                          suggestedName:(NSString*)suggestedName
                           confirmTitle:(NSString*)confirmTitle
                         confirmMessage:(NSString*)confirmMessage
                           confirmLabel:(NSString*)confirmLabel
                                  error:(NSString**)error_out {
  if (!bytes || bytes.length == 0) {
    if (error_out) {
      *error_out = @"The touch layout payload was empty.";
    }
    return NO;
  }

  std::string_view bytes_view(static_cast<const char*>(bytes.bytes), bytes.length);
  xe::hid::touch::IOSTouchLayoutModel parsed_layout;
  try {
    toml::table table = toml::parse(bytes_view);
    parsed_layout = MakeTouchLayoutSeedModelForTable(table);
    if (!xe::hid::touch::ApplyIOSTouchLayoutModel(table, &parsed_layout)) {
      if (error_out) {
        *error_out = @"Touch layout file could not be applied.";
      }
      return NO;
    }
  } catch (const std::exception& parse_error) {
    if (error_out) {
      *error_out =
          [NSString stringWithFormat:@"Failed to parse touch layout: %s", parse_error.what()];
    }
    return NO;
  }

  NSString* effective_display = nil;
  if (!parsed_layout.display_name.empty()) {
    effective_display = ToNSString(parsed_layout.display_name);
  } else if (suggestedName.length) {
    effective_display = suggestedName;
  } else {
    effective_display = @"Imported Layout";
  }
  NSString* sheet_message = confirmMessage.length
                                ? confirmMessage
                                : [NSString stringWithFormat:@"Install \"%@\"?", effective_display];

  xe::hid::touch::IOSTouchLayoutModel* layout_copy =
      new xe::hid::touch::IOSTouchLayoutModel(parsed_layout);
  __unsafe_unretained XeniaIOSTouchLayoutUICoordinator* unsafe_self = self;
  NSString* base_name_for_id = effective_display;

  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:confirmTitle ?: @"Install Touch Layout"
                                          message:sheet_message
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:@"Cancel"
                                 style:UIAlertActionStyleCancel
                               handler:^(__unused UIAlertAction* _action) {
                                 delete layout_copy;
                                 [unsafe_self->_host
                                     touchLayoutCoordinatorEvaluateAutomaticStikDebugJITHandoffIfNeeded];
                               }]];
  [alert
      addAction:[UIAlertAction
                    actionWithTitle:confirmLabel.length ? confirmLabel : @"Install"
                              style:UIAlertActionStyleDefault
                            handler:^(__unused UIAlertAction* _action) {
                              [unsafe_self installParsedLayout:layout_copy
                                                 baseNameForID:base_name_for_id];
                            }]];

  UIAlertController* alert_to_present = [alert retain];
  __unsafe_unretained id<XeniaIOSTouchLayoutUICoordinatorHost> host = _host;
  dispatch_async(dispatch_get_main_queue(), ^{
    [[host touchLayoutCoordinatorTopPresenter] presentViewController:alert_to_present
                                                            animated:YES
                                                          completion:nil];
    [alert_to_present release];
  });
  return YES;
}

- (void)installParsedLayout:(xe::hid::touch::IOSTouchLayoutModel*)layout_copy
             baseNameForID:(NSString*)base_name_for_id {
  std::string local_id = [self uniqueTouchLayoutLocalIDForBaseName:base_name_for_id];
  layout_copy->layout_id = local_id;
  if (layout_copy->author.empty()) {
    layout_copy->author = "Imported";
  }
  layout_copy->base_template = NormalizeOfficialTouchLayoutBaseTemplate(layout_copy->base_template);
  NSString* write_error = nil;
  BOOL ok = [self writeTouchLayoutModel:*layout_copy
                                   path:[self touchLayoutPathForLocalID:local_id]
                                  error:&write_error];
  if (!ok) {
    NSString* message =
        write_error.length ? write_error : @"The touch layout could not be saved.";
    [_host touchLayoutCoordinatorSetStatusText:message];
    [_host touchLayoutCoordinatorPresentAlertWithTitle:@"Touch Layout" message:message];
    delete layout_copy;
    [_host touchLayoutCoordinatorEvaluateAutomaticStikDebugJITHandoffIfNeeded];
    return;
  }
  [_host touchLayoutCoordinatorSetActiveLocalID:std::string()];
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model = [self runtimeModel];
  if (runtime_model) {
    runtime_model->SetLayout(*layout_copy);
  }
  [_host touchLayoutCoordinatorRefreshTouchOverlayLayoutModel];
  [self refreshTouchLayoutLibraryOverlayIfVisible];
  [self saveCurrentLayoutForTitleID:[_host touchLayoutCoordinatorActiveTitleID]];
  [_host touchLayoutCoordinatorSetStatusText:
             [NSString stringWithFormat:@"Imported %@.", ToNSString(layout_copy->display_name)]];
  delete layout_copy;
  [_host touchLayoutCoordinatorEvaluateAutomaticStikDebugJITHandoffIfNeeded];
}

- (void)presentActionSheet:(UIViewController*)controller {
  UIViewController* presenter = [_host touchLayoutCoordinatorTopPresenter];
  UIPopoverPresentationController* popover = controller.popoverPresentationController;
  if (popover) {
    popover.sourceView = presenter.view;
    popover.sourceRect = CGRectMake(CGRectGetMidX(popover.sourceView.bounds),
                                    CGRectGetMidY(popover.sourceView.bounds), 1.0, 1.0);
    popover.permittedArrowDirections = 0;
  }
  [presenter presentViewController:controller animated:YES completion:nil];
}

@end
