/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_main_view_controller.h"

#import <GameController/GameController.h>
#import <PhotosUI/PhotosUI.h>

#include "xenia/ui/ios/shared/ios_edge_adapter.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/config.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/hid/input.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/xbox.h"

#import "xenia/hid/touch/touch_layout_ios.h"
#import "xenia/ui/ios/app/ios_controller_navigation_coordinator.h"
#import "xenia/ui/ios/app/ios_in_game_menu_overlay.h"
#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/app/ios_window_layout.h"
#import "xenia/ui/ios/app/ios_window_position_overlay.h"
#import "xenia/ui/ios/app/windowed_app_context_ios.h"
#import "xenia/ui/ios/game/ios_achievement_notification_presenter.h"
#import "xenia/ui/ios/game/ios_achievements_view_controller.h"
#import "xenia/ui/ios/game/ios_metal_view.h"
#import "xenia/ui/ios/game/ios_launch_status.h"
#import "xenia/ui/ios/game/ios_shader_compile_indicator.h"
#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_compat_report_view_controller.h"
#import "xenia/ui/ios/launcher/ios_content_management.h"
#import "xenia/ui/ios/launcher/ios_dashboard.h"
#import "xenia/ui/ios/launcher/ios_dashboard_setup_view_controller.h"
#import "xenia/ui/ios/launcher/ios_document_import_coordinator.h"
#import "xenia/ui/ios/launcher/ios_external_folders_view_controller.h"
#import "xenia/ui/ios/launcher/ios_external_url.h"
#import "xenia/ui/ios/launcher/ios_game_actions_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/launcher/ios_game_compatibility_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_content_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_disc_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_library.h"
#import "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/launcher/ios_game_patches_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_picker_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_tile_cell.h"
#import "xenia/ui/ios/launcher/ios_launcher_overlay_view.h"
#import "xenia/ui/ios/launcher/ios_system_link_view_controller.h"
#import "xenia/ui/ios/launcher/ios_zar_conversion_coordinator.h"
#import "xenia/ui/ios/settings/ios_choice_list_view_controller.h"
#import "xenia/ui/ios/settings/ios_config_builder.h"
#import "xenia/ui/ios/settings/ios_config_models.h"
#import "xenia/ui/ios/settings/ios_config_view_controller.h"
#import "xenia/ui/ios/settings/ios_log_view_controller.h"
#import "xenia/ui/ios/settings/ios_profile_view_controller.h"
#import "xenia/ui/ios/settings/ios_quick_settings_view_controller.h"
#import "xenia/ui/ios/settings/ios_settings_hub_view_controller.h"
#import "xenia/ui/ios/settings/ios_system_software_view_controller.h"
#import "xenia/ui/ios/shared/ios_alert_actions.h"
#import "xenia/ui/ios/shared/ios_crash_reporting.h"
#import "xenia/ui/ios/shared/ios_diagnostics.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_resource_pressure.h"
#import "xenia/ui/ios/shared/ios_status_toast.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_update_check.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"
#import "xenia/ui/ios/touch/touch_controls_overlay_ios.h"
#import "xenia/ui/ios/touch/touch_layout_editor_view_controller_ios.h"
#import "xenia/ui/ios/touch/touch_layout_library_ios.h"
#import "xenia/ui/ios/touch/touch_layout_store_ios.h"
#import "xenia/ui/ios/touch/touch_layout_ui_coordinator_ios.h"

DECLARE_bool(ios_touch_overlay);
DECLARE_bool(present_letterbox);
DECLARE_path(cache_root);

using xe::ui::ApplyIOSCompatibilityDataToDiscoveredGames;
using xe::ui::BuildDiscoveredGameFromPath;
using xe::ui::ExtractLaunchPathFromExternalURL;
using xe::ui::ExtractLaunchTitleIDFromExternalURL;
using xe::ui::FormatTitleID;
using xe::ui::ImportGameIntoIOSLibrary;
using xe::ui::IOSDiscoveredGame;
using xe::ui::IOSImportedGamesDirectory;
using xe::ui::IsExternalGameInfoRequestURL;
using xe::ui::IsLikelyGodContainerFile;
using xe::ui::IsZarPath;
using xe::ui::NormalizeURLToken;
using xe::ui::ScanIOSGameLibrary;
using xe::ui::SortDiscoveredGames;
using xe::ui::xe_game_info_callback_provider;
using xe::ui::xe_launch_url_for_title_id;
using xe::ui::xe_stikdebug_enable_jit_url_for_bundle_identifier;

namespace {

constexpr NSTimeInterval kXeniaAutoStikDebugCooldownSeconds = 10.0;
NSString* const kXeniaPendingStikDebugAutomationPromptPreferenceKey =
    @"ios_pending_stikdebug_automation_prompt";

NSTimeInterval GetUnixTimeSeconds() { return [[NSDate date] timeIntervalSince1970]; }

static BOOL xe_game_system_supports_compatibility(xe::ui::IOSGameSystem system) {
  return system == xe::ui::IOSGameSystem::kXbox360;
}

static BOOL xe_game_system_supports_manage_content(xe::ui::IOSGameSystem system) {
  return system == xe::ui::IOSGameSystem::kXbox360;
}

static BOOL xe_game_system_supports_remote_art(xe::ui::IOSGameSystem system) {
  return system == xe::ui::IOSGameSystem::kXbox360;
}

static bool xe_game_supports_zar_conversion(const IOSDiscoveredGame& game) {
  if (game.discs.empty()) {
    return !IsZarPath(game.path);
  }
  for (const IOSDiscoveredGame::Disc& disc : game.discs) {
    if (!IsZarPath(disc.path)) {
      return true;
    }
  }
  return false;
}

static std::filesystem::path xe_ios_settings_cache_root() {
  std::filesystem::path root = cvars::cache_root;
  if (root.empty()) {
    @autoreleasepool {
      NSArray* cache_paths =
          NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
      if (cache_paths.count > 0) {
        root = std::filesystem::path([cache_paths[0] UTF8String]) / "xenia";
      } else {
        root = xe_get_ios_documents_path() / "cache_host";
      }
    }
  } else if (!root.is_absolute()) {
    root = xe_get_ios_documents_path() / root;
  }
  return std::filesystem::absolute(root);
}

static std::string xe_title_id_cache_prefix(uint32_t title_id) {
  char buffer[9] = {};
  std::snprintf(buffer, sizeof(buffer), "%08X", title_id);
  return std::string(buffer);
}

static bool xe_cache_entry_matches_title(const std::filesystem::path& path,
                                         const std::string& title_prefix) {
  const std::string name = path.filename().string();
  return name == title_prefix || name.rfind(title_prefix + ".", 0) == 0;
}

static uintmax_t xe_remove_path_if_present(const std::filesystem::path& path,
                                           std::error_code* error_out) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (error_out) {
      *error_out = {};
    }
    return 0;
  }
  uintmax_t removed = std::filesystem::remove_all(path, ec);
  if (error_out) {
    *error_out = ec;
  }
  return ec ? 0 : removed;
}

static bool xe_clear_shader_cache_for_title(uint32_t title_id, uintmax_t* removed_out,
                                            std::error_code* error_out) {
  const std::filesystem::path cache_root = xe_ios_settings_cache_root();
  const std::string title_prefix = xe_title_id_cache_prefix(title_id);
  std::vector<std::filesystem::path> candidates;

  const std::filesystem::path shareable_root = xe::gpu::GetShaderStorageShareableRoot(cache_root);
  std::error_code ec;
  if (std::filesystem::exists(shareable_root, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(shareable_root, ec)) {
      if (ec) {
        break;
      }
      if (xe_cache_entry_matches_title(entry.path(), title_prefix)) {
        candidates.push_back(entry.path());
      }
    }
  }

  const std::filesystem::path local_root = xe::gpu::GetShaderStorageLocalRoot(cache_root);
  ec = {};
  if (std::filesystem::exists(local_root, ec)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(local_root, ec)) {
      if (ec) {
        break;
      }
      if (xe_cache_entry_matches_title(entry.path(), title_prefix)) {
        candidates.push_back(entry.path());
      }
    }
  }

  uintmax_t removed_total = 0;
  for (const std::filesystem::path& candidate : candidates) {
    ec = {};
    removed_total += xe_remove_path_if_present(candidate, &ec);
    if (ec) {
      if (removed_out) {
        *removed_out = removed_total;
      }
      if (error_out) {
        *error_out = ec;
      }
      return false;
    }
  }
  if (removed_out) {
    *removed_out = removed_total;
  }
  if (error_out) {
    *error_out = {};
  }
  return true;
}

static bool xe_clear_all_shader_caches(uintmax_t* removed_out, std::error_code* error_out) {
  std::error_code ec;
  uintmax_t removed =
      xe_remove_path_if_present(xe::gpu::GetShaderStorageRoot(xe_ios_settings_cache_root()), &ec);
  if (removed_out) {
    *removed_out = removed;
  }
  if (error_out) {
    *error_out = ec;
  }
  return !ec;
}

}  // namespace

// Private touch-overlay/library methods and internal UI state. Kept out of the
// public header so the app delegate sees only the bridge surface it needs.
@interface XeniaViewController () <XeniaIOSControllerNavigationHost,
                                   XeniaIOSDocumentImportCoordinatorHost,
                                   XeniaIOSTouchLayoutUICoordinatorHost,
                                   XeniaGameContentHost,
                                   XeniaIOSZarConversionCoordinatorHost,
                                   UIAdaptivePresentationControllerDelegate>
@property(nonatomic, strong) XeniaIOSLauncherOverlayView* launcherOverlayView;
@property(nonatomic, strong) UILabel* signedInProfileLabel;
@property(nonatomic, strong) XeniaIOSInGameMenuOverlay* inGameMenuOverlay;
@property(nonatomic, strong) XeniaIOSWindowPositionOverlay* windowPositionOverlay;
@property(nonatomic, strong)
    XeniaIOSControllerNavigationCoordinator* controllerNavigationCoordinator;
@property(nonatomic, strong) XeniaIOSDocumentImportCoordinator* documentImportCoordinator;
@property(nonatomic, strong) XeniaIOSZarConversionCoordinator* zarConversionCoordinator;
@property(nonatomic, strong) XeniaIOSTouchLayoutUICoordinator* touchLayoutCoordinator;
@property(nonatomic, strong)
    XeniaIOSAchievementNotificationPresenter* achievementNotificationPresenter;
@property(nonatomic, strong) XeniaIOSStatusToastPresenter* statusToastPresenter;
@property(nonatomic, strong) XeniaTouchControlsOverlayView* touchControlsOverlay;
@property(nonatomic, strong) XeniaShaderCompileIndicator* shaderCompileIndicator;
@property(nonatomic, strong) XeniaLaunchStatusView* launchStatusView;
@property(nonatomic, assign) BOOL gameRunning;
@property(nonatomic, assign) BOOL gameStopInProgress;
@property(nonatomic, strong) NSTimer* jitPollTimer;
@property(nonatomic, assign) NSTimeInterval jitPollInterval;
@property(nonatomic, assign) NSTimeInterval jitPollStartedAt;
@property(nonatomic, assign) BOOL jitPollTimedOut;
@property(nonatomic, assign) BOOL jitAcquired;
@property(nonatomic, assign) BOOL memoryEntitlementEnabled;
- (BOOL)hasConnectedGameplayController;
- (BOOL)shouldBlockGameplayInput;
- (BOOL)shouldShowTouchControlsOverlay;
- (void)refreshLauncherGameSnapshots;
- (void)refreshImportedGamesAsync;
- (void)refreshImportedGamesAsyncWithCompletion:(void (^)(void))completion;
- (void)refreshImportedGamesAsyncWithScannedGamesCompletion:
    (void (^)(const std::vector<IOSDiscoveredGame>& games))completion;
- (void)finishImportedGamesRefresh;
- (void)presentJITRequiredAlertForLaunchPath:(const std::filesystem::path&)gamePath
                                 displayName:(NSString*)displayName;
- (void)updateMemoryEntitlementStatusUI;
- (void)presentMemoryEntitlementWarningForLaunchPath:(const std::filesystem::path&)gamePath
                                         displayName:(NSString*)displayName;
- (void)launchGameAtPath:(const std::filesystem::path&)gamePath
                     displayName:(NSString*)displayName
    skipMemoryEntitlementWarning:(BOOL)skipMemoryEntitlementWarning;
- (BOOL)requestStikDebugJITHandoffForPendingLaunchPath:(const std::filesystem::path*)launchPath
                                     requirePreference:(BOOL)requirePreference
                          promptForAutomationOnSuccess:(BOOL)promptForAutomation;
- (void)finishJITAcquiredPendingLaunchWithQueuedPath:(const std::filesystem::path&)queuedPath
                                       persistedPath:(const std::filesystem::path&)persistedPath;
- (BOOL)consumePendingStikDebugAutomationPrompt;
- (void)presentStikDebugAutomationPromptWithCompletion:(void (^)(void))completion;
- (void)setTouchLayoutEditModeActive:(BOOL)active animated:(BOOL)animated;
- (void)finishTouchLayoutEditMode;
- (void)updateTouchControlsOverlayVisibilityAnimated:(BOOL)animated;
- (void)showStatusToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style;
- (void)showPersistentStatusToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style;
- (void)updateStatusToast:(NSString*)message;
- (void)dismissStatusToast;
- (void)showStatusToastForMessage:(NSString*)message;
- (void)presentLaunchProblemAlertWithMessage:(NSString*)message;
- (void)presentLinkCouldNotBeOpenedAlert;
- (void)presentGameActionsSheetForIndex:(size_t)game_index;
- (void)presentTouchLayoutGamePicker;
- (void)performGameAction:(XeniaIOSGameAction)action forIndex:(size_t)game_index;
- (void)presentGameSettingsSheetForIndex:(size_t)game_index;
- (void)confirmResetGameSettingsForIndex:(size_t)game_index;
- (void)presentGameTouchLayoutSheetForIndex:(size_t)game_index;
- (void)presentCompatibilitySheetForIndex:(size_t)game_index;
- (void)presentManageContentSheetForIndex:(size_t)game_index;
- (void)presentDiscSelectionSheetForIndex:(size_t)game_index;
- (void)presentPatchesSheetForIndex:(size_t)game_index;
- (void)scheduleNextJITPoll;
- (void)pauseButtonTapped:(id)sender;
- (void)resumeGameTapped:(UIButton*)sender;
- (void)inGameEditControlsTapped:(UIButton*)sender;
- (void)inGameAchievementsTapped:(UIButton*)sender;
- (void)inGameSettingsTapped:(UIButton*)sender;
- (void)inGameLiveLogTapped:(UIButton*)sender;
- (void)exitGameTapped:(UIButton*)sender;
- (void)exitGameToDashboard;
- (void)handleSettingsHubAction:(XeniaSettingsHubAction)action;
- (void)confirmClearCurrentGameShaderCache;
- (void)confirmClearAllShaderCaches;
- (void)clearCurrentGameShaderCache;
- (void)clearAllShaderCaches;
- (void)presentShaderCacheResultWithTitle:(NSString*)title
                                  success:(BOOL)success
                                  removed:(uintmax_t)removed
                                    error:(const std::error_code&)error;
- (void)refreshInGameDisplayMenu;
- (void)applyDefaultTouchLayoutModel;
- (void)applyTouchLayoutModelForTitleID:(uint32_t)title_id;
- (void)presentTouchLayoutLibrarySheet;
- (void)presentRenameTouchLayoutSheet;
- (void)presentDeleteTouchLayoutSheet;
- (void)saveCurrentTouchLayoutCopy;
- (UIViewController*)topPresentedControllerForModalPresentation;
- (void)importTouchLayoutFromFile;
- (void)exportCurrentTouchLayout;
- (void)resetToOfficialTouchLayoutPreset;
- (void)saveTouchLayoutModelForTitleID:(uint32_t)title_id;
- (uint32_t)titleIDForGamePath:(const std::filesystem::path&)game_path;
- (BOOL)handleExternalTouchLayoutFileURL:(NSURL*)url;
- (void)presentURLConfirmation:(UIAlertController*)alert;
- (void)clearURLConfirmation;
- (void)proceedWithExternalLaunchPath:(const std::filesystem::path&)launch_path
                          displayName:(NSString*)display_name;
- (void)sendGameInfoToCallbackScheme:(NSString*)callback_scheme requestURL:(NSURL*)url;
- (NSString*)runningGameDisplayName;
- (BOOL)handleExternalTouchLayoutSchemeURL:(NSURL*)url;
- (void)presentPendingTouchLayoutInstallIfReady;
- (void)handleAppDidBecomeActive:(NSNotification*)note;
- (void)handleControllerConnectionChanged:(NSNotification*)note;
- (BOOL)isGuestDisplayUncapped;
- (void)setGuestDisplayUncapped:(BOOL)uncapped;
- (BOOL)isPresentLetterboxEnabled;
- (void)setPresentLetterboxEnabled:(BOOL)enabled;
- (void)presentLibraryAddSheetFromView:(UIView*)sourceView;
- (void)presentExternalFoldersSheet;
- (BOOL)appendLinkedContentAccessesForTitleID:(uint32_t)title_id
                                     accesses:
                                         (NSMutableArray<XeniaIOSExternalLibraryAccess*>*)accesses
                                 errorMessage:(NSString**)errorMessage;
- (void)retainExternalGameAccessForLaunch:(XeniaIOSExternalLibraryAccess*)access
                          replaceExisting:(BOOL)replaceExisting;
- (void)retainExternalGameAccessesForLaunch:(NSArray<XeniaIOSExternalLibraryAccess*>*)accesses
                            replaceExisting:(BOOL)replaceExisting;
- (void)clearActiveExternalGameAccesses;

// Presents `rootController` as the standardized in-game/settings sheet: wraps it
// in a XeniaLandscapeNavigationController, applies the shared task-sheet chrome,
// suppresses the gameplay touch overlay while presented, and clears that
// suppression on both completion and interactive dismissal. Centralizes the
// boilerplate previously duplicated across the quick settings, live log and
// achievements presenters. Returns the autoreleased wrapping controller.
- (XeniaLandscapeNavigationController*)
    presentPauseSheetWithRootController:(UIViewController*)rootController
                          preferredSize:(CGSize)preferredSize
            preventInteractiveDismissal:(BOOL)preventInteractiveDismissal
                             completion:(void (^)(void))completion;
@end

@implementation XeniaViewController {
  std::vector<IOSDiscoveredGame> discovered_games_;
  NSDictionary* compat_data_;
  // Icon bytes already wrapped for the launcher, by game path, and the last snapshot set pushed to
  // it. A refresh that finds nothing changed leaves the grid (and any scroll or menu) alone.
  NSMutableDictionary<NSString*, NSData*>* launcher_icon_cache_;
  NSArray<NSString*>* launcher_snapshot_signature_;
  CGSize last_collection_layout_size_;
  uint64_t library_refresh_generation_;
  BOOL compat_fetch_started_;
  BOOL update_check_started_;
  BOOL online_features_notice_presented_;
  BOOL external_title_launch_rescanned_;
  UIAlertController* pending_url_confirmation_;  // retained
  std::filesystem::path pending_external_launch_path_;
  std::filesystem::path deferred_jit_prompt_queued_launch_path_;
  std::filesystem::path deferred_jit_prompt_persisted_launch_path_;
  std::unique_ptr<xe::hid::touch::IOSTouchRuntimeModel> touch_runtime_model_;
  BOOL touch_layout_edit_mode_active_;
  BOOL gameplay_modal_presentation_pending_;
  // The user paused the game with the in-game menu's Pause Emulator button.
  BOOL emulation_paused_by_user_;
  BOOL presenting_stikdebug_automation_prompt_;
  uint32_t active_game_title_id_;
  // The dashboard was launched from the library; the games it starts exit back to it.
  BOOL dashboard_session_;
  std::string active_touch_layout_local_id_;
  UITapGestureRecognizer* in_game_menu_tap_recognizer_;  // owned by self.view
  NSMutableArray<XeniaIOSExternalLibraryAccess*>* active_external_game_accesses_;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor blackColor];
  self.jitAcquired = NO;
  self.memoryEntitlementEnabled = YES;
  self.gameRunning = NO;
  self.gameStopInProgress = NO;
  last_collection_layout_size_ = CGSizeZero;
  compat_fetch_started_ = NO;
  update_check_started_ = NO;
  touch_layout_edit_mode_active_ = NO;
  gameplay_modal_presentation_pending_ = NO;
  presenting_stikdebug_automation_prompt_ = NO;
  active_game_title_id_ = 0;
  active_touch_layout_local_id_.clear();
  active_external_game_accesses_ = [[NSMutableArray alloc] init];
  XeniaIOSControllerNavigationCoordinator* controller_navigation =
      [[XeniaIOSControllerNavigationCoordinator alloc] initWithHost:self];
  self.controllerNavigationCoordinator = controller_navigation;
  [controller_navigation release];
  XeniaIOSDocumentImportCoordinator* document_import =
      [[XeniaIOSDocumentImportCoordinator alloc] initWithHost:self];
  self.documentImportCoordinator = document_import;
  [document_import release];
  XeniaIOSZarConversionCoordinator* zar_conversion =
      [[XeniaIOSZarConversionCoordinator alloc] initWithHost:self];
  self.zarConversionCoordinator = zar_conversion;
  [zar_conversion release];
  XeniaIOSTouchLayoutUICoordinator* touch_layout =
      [[XeniaIOSTouchLayoutUICoordinator alloc] initWithHost:self];
  self.touchLayoutCoordinator = touch_layout;
  [touch_layout release];
  XeniaIOSAchievementNotificationPresenter* achievement_notifications =
      [[XeniaIOSAchievementNotificationPresenter alloc] init];
  self.achievementNotificationPresenter = achievement_notifications;
  [achievement_notifications release];
  XeniaIOSStatusToastPresenter* status_toasts = [[XeniaIOSStatusToastPresenter alloc] init];
  self.statusToastPresenter = status_toasts;
  [status_toasts release];
  // The controller lives for the whole app run, so the blocks may hold it unretained.
  __unsafe_unretained XeniaViewController* resource_pressure_owner = self;
  xe_ios_resource_pressure_install(
      ^BOOL {
        return resource_pressure_owner.gameRunning;
      },
      ^(NSString* message) {
        [resource_pressure_owner showStatusToast:message style:XeniaIOSStatusToastStyleWarning];
      });
  __unsafe_unretained XeniaViewController* system_link_owner = self;
  xe_ios_system_link_install(^(NSString* message, XeniaIOSStatusToastStyle style) {
    [system_link_owner showStatusToast:message style:style];
  });
  xe_ios_system_link_watch_online(
      ^BOOL {
        return !system_link_owner.gameRunning && system_link_owner.launcherOverlayView &&
               !system_link_owner.launcherOverlayView.hidden;
      },
      ^{
        [system_link_owner.launcherOverlayView reloadGames];
      });
  xe_ios_system_link_set_online_menu(^{
    std::vector<XeniaIOSOnlineLibraryGame> library;
    for (const IOSDiscoveredGame& game : system_link_owner->discovered_games_) {
      library.push_back({game.title_id, ToNSString(game.title),
                         game.icon_data.empty() ? nil
                                                : [NSData dataWithBytes:game.icon_data.data()
                                                                 length:game.icon_data.size()]});
    }
    xe_ios_present_online_sheet(system_link_owner, library, ^(uint32_t title_id) {
      const std::vector<IOSDiscoveredGame>& games = system_link_owner->discovered_games_;
      for (size_t i = 0; i < games.size(); ++i) {
        if (games[i].title_id == title_id) {
          [system_link_owner performGameAction:XeniaIOSGameActionSystemLink forIndex:i];
          return;
        }
      }
    });
  });
  // Create the Metal-backed rendering view. The frame is set explicitly via
  // -applyMetalViewLayout in -viewDidLayoutSubviews so the user-chosen
  // window scaling mode (Fit / Stretch / Zoom) and portrait position offset
  // can be applied. autoresizingMask is intentionally NOT set — we manage
  // the frame on every layout pass instead. clipsToBounds keeps the Zoom
  // mode (which may extend the metal view past the parent in one dimension
  // to maintain aspect) from leaking outside our view controller's view.
  self.view.clipsToBounds = YES;
  self.metalView = [[[XeniaMetalView alloc] initWithFrame:self.view.bounds] autorelease];
  self.metalView.contentScaleFactor = [UIScreen mainScreen].scale;
  [self.view addSubview:self.metalView];

  // Above the game, below the touch controls and every menu.
  XeniaShaderCompileIndicator* compile_indicator =
      [[[XeniaShaderCompileIndicator alloc] initWithFrame:self.view.bounds] autorelease];
  compile_indicator.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.shaderCompileIndicator = compile_indicator;
  [self.view insertSubview:compile_indicator aboveSubview:self.metalView];

  // Create the iOS touch overlay above the Metal view. Hidden by default;
  // visibility is driven by -updateTouchControlsOverlayVisibilityAnimated:.
  touch_runtime_model_ = std::make_unique<xe::hid::touch::IOSTouchRuntimeModel>();
  XeniaTouchControlsOverlayView* touch_overlay =
      [[XeniaTouchControlsOverlayView alloc] initWithRuntimeModel:touch_runtime_model_.get()];
  touch_overlay.frame = self.view.bounds;
  touch_overlay.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.touchControlsOverlay = touch_overlay;
  __block XeniaViewController* block_self = self;
  self.touchControlsOverlay.pauseHandler = ^{
    [block_self pauseButtonTapped:nil];
  };
  self.touchControlsOverlay.doneEditingHandler = ^{
    [block_self finishTouchLayoutEditMode];
  };
  self.touchControlsOverlay.layoutLibraryHandler = ^{
    [block_self presentTouchLayoutLibrarySheet];
  };
  self.touchControlsOverlay.layoutLibraryLoadHandler = ^(NSString* local_id) {
    [block_self.touchLayoutCoordinator applyLayoutWithLocalID:local_id];
  };
  self.touchControlsOverlay.layoutLibrarySaveCopyHandler = ^{
    [block_self saveCurrentTouchLayoutCopy];
  };
  self.touchControlsOverlay.layoutLibraryRenameHandler = ^{
    [block_self presentRenameTouchLayoutSheet];
  };
  self.touchControlsOverlay.layoutLibraryDeleteHandler = ^{
    [block_self presentDeleteTouchLayoutSheet];
  };
  self.touchControlsOverlay.layoutLibraryImportHandler = ^{
    [block_self importTouchLayoutFromFile];
  };
  self.touchControlsOverlay.layoutLibraryExportHandler = ^{
    [block_self exportCurrentTouchLayout];
  };
  self.touchControlsOverlay.layoutLibraryResetHandler = ^{
    [block_self resetToOfficialTouchLayoutPreset];
  };
  self.touchControlsOverlay.layoutLibraryRenameLayoutHandler = ^(NSString* local_id) {
    [block_self.touchLayoutCoordinator renameLayoutWithLocalID:local_id];
  };
  self.touchControlsOverlay.layoutLibraryDeleteLayoutHandler = ^(NSString* local_id) {
    [block_self.touchLayoutCoordinator deleteLayoutWithLocalID:local_id];
  };
  self.touchControlsOverlay.layoutLibraryExportLayoutHandler = ^(NSString* local_id) {
    [block_self.touchLayoutCoordinator exportLayoutWithLocalID:local_id];
  };
  self.touchControlsOverlay.layoutLibrarySetTitleDefaultHandler = ^(NSString* local_id) {
    [block_self.touchLayoutCoordinator setLayoutDefaultForCurrentTitleWithLocalID:local_id];
  };
  self.touchControlsOverlay.layoutLibrarySetGlobalDefaultHandler = ^(NSString* local_id) {
    [block_self.touchLayoutCoordinator setLayoutDefaultForAllGamesWithLocalID:local_id];
  };
  self.touchControlsOverlay.layoutLibraryFavoriteHandler = ^(NSString* local_id, BOOL favorite) {
    [block_self.touchLayoutCoordinator setLayoutFavoriteWithLocalID:local_id favorite:favorite];
  };
  [self.view addSubview:self.touchControlsOverlay];
  [touch_overlay release];

  // Covers the game and its touch controls from a launch until the first frame.
  XeniaLaunchStatusView* launch_status =
      [[[XeniaLaunchStatusView alloc] initWithFrame:self.view.bounds] autorelease];
  launch_status.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  launch_status.cancelHandler = ^{
    [block_self exitGameTapped:nil];
  };
  self.launchStatusView = launch_status;
  [self.view addSubview:launch_status];

  // Create the launcher overlay UI immediately. When JIT is missing, keep
  // settings/navigation available but gate game launch with status.
  [self setupLauncherOverlay];
  [self setupInGameMenuOverlay];
  UITapGestureRecognizer* tap =
      [[UITapGestureRecognizer alloc] initWithTarget:self
                                              action:@selector(toggleInGameMenuTapped:)];
  tap.numberOfTapsRequired = 1;
  tap.cancelsTouchesInView = NO;
  [self.view addGestureRecognizer:tap];
  in_game_menu_tap_recognizer_ = tap;
  [tap release];
  [self updateJITStatusIndicator];
  [self updateJITAvailabilityUI];
  [self updateMemoryEntitlementStatusUI];
  [self refreshSignedInProfileUI];
  NSDictionary* cached_compat_data = xe_load_cached_compat_data();
  if (cached_compat_data) {
    [compat_data_ release];
    compat_data_ = [cached_compat_data retain];
  }
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onCompatDataDidUpdate:)
                                               name:kXeniaCompatDataDidUpdateNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onExternalLibraryDidChange:)
                                               name:kXeniaIOSExternalLibraryDidChangeNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onSystemSoftwareDidChange:)
                                               name:XeniaIOSSystemSoftwareDidChangeNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onTitleWillStart:)
                                               name:XeniaIOSTitleWillStartNotification
                                             object:nil];
  // Show the last scan's result at once (read from disk, no folder walk), then
  // rescan in the background.
  discovered_games_ = xe::ui::LoadCachedIOSGameLibrary();
  [self finishImportedGamesRefresh];
  [self refreshImportedGamesAsync];

  // Start polling for JIT.
  [self startJITPoll];
  [self.controllerNavigationCoordinator start];

  // Cold-launch "Open in XeniOS" with auto-StikDebug enabled backgrounds the
  // app right after the URL is dispatched, killing any in-flight modal.
  // Listening for the foreground transition lets us present the install
  // confirm once we're back on screen.
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(handleAppDidBecomeActive:)
                                               name:UIApplicationDidBecomeActiveNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(handleControllerConnectionChanged:)
                                               name:GCControllerDidConnectNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(handleControllerConnectionChanged:)
                                               name:GCControllerDidDisconnectNotification
                                             object:nil];
}

- (void)onExternalLibraryDidChange:(NSNotification* __unused)note {
  [self refreshImportedGamesAsync];
}

- (void)onSystemSoftwareDidChange:(NSNotification* __unused)note {
  [self.launcherOverlayView setDashboardInstalled:xe::ui::IsIOSDashboardInstalled()];
}

- (void)launchDashboard {
  [self launchGameAtPath:xe::ui::IOSInstalledDashboardPath()
             displayName:NSLocalizedString(@"Xbox 360 Dashboard",
                                           @"main view controller: title")];
}

// The library's Dashboard tile starts the dashboard, or, before there is
// system software, sets it up.
- (void)openDashboard {
  if (xe::ui::IsIOSDashboardInstalled()) {
    [self launchDashboard];
    return;
  }
  __unsafe_unretained XeniaViewController* unsafe_self = self;
  XeniaDashboardSetupViewController* setup =
      [[[XeniaDashboardSetupViewController alloc] initWithStartHandler:^{
        [unsafe_self launchDashboard];
      }] autorelease];
  [self presentLauncherSheetWithRoot:setup];
}

// The System Software page from Settings, on its own: the Dashboard tile's
// menu opens it.
- (void)openSystemSoftware {
  XeniaSystemSoftwareViewController* page = [[[XeniaSystemSoftwareViewController alloc]
      initWithGameRunning:self.gameRunning] autorelease];
  page.showsDoneButton = YES;
  [self presentLauncherSheetWithRoot:page];
}

- (void)presentLauncherSheetWithRoot:(UIViewController*)root {
  XeniaLandscapeNavigationController* sheet =
      [[[XeniaLandscapeNavigationController alloc] initWithRootViewController:root] autorelease];
  XEConfigureTaskSheet(sheet, self.view, CGSizeMake(620.0, 760.0), NO);
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:sheet animated:YES completion:nil];
}

// The launcher's grid leads with the Dashboard tile while there are games, so
// the controller's item indexes run one ahead of the games'.
- (NSInteger)launcherGameItemOffset {
  return discovered_games_.empty() ? 0 : 1;
}

// A title the emulator starts itself, like a game the dashboard launches, gets
// what a launch from the library sets up: its touch layout and access to the
// folder it is in and to every linked folder. A game the dashboard starts exits
// back to it.
- (void)onTitleWillStart:(NSNotification*)note {
  NSString* path_ns = note.userInfo[XeniaIOSTitlePathKey];
  if (![path_ns isKindOfClass:[NSString class]] || !path_ns.length) {
    return;
  }
  const std::filesystem::path path([path_ns fileSystemRepresentation]);
  NSMutableArray<XeniaIOSExternalLibraryAccess*>* accesses = [NSMutableArray array];
  BOOL matched_external_location = NO;
  if (XeniaIOSExternalLibraryAccess* access = xe::ui::StartIOSExternalLibraryAccessForPath(
          path, &matched_external_location, nil)) {
    [accesses addObject:access];
  }
  const BOOL starts_dashboard = xe::ui::IsIOSInstalledDashboardPath(path);
  // Every linked folder, as at a launch from the library, so a disc swap in a
  // game the dashboard starts can reach the other folders too.
  [accesses addObjectsFromArray:xe::ui::StartIOSExternalLibraryAccessForAllLocations()];
  self.inGameMenuOverlay.exitsToDashboard = dashboard_session_ && !starts_dashboard;
  active_game_title_id_ = [self titleIDForGamePath:path];
  [self appendLinkedContentAccessesForTitleID:active_game_title_id_
                                     accesses:accesses
                                 errorMessage:nil];
  [self retainExternalGameAccessesForLaunch:accesses replaceExisting:YES];
  [self applyTouchLayoutModelForTitleID:active_game_title_id_];
}

- (void)handleAppDidBecomeActive:(NSNotification* __unused)note {
  [self updateMemoryEntitlementStatusUI];
  if (!self.jitAcquired && !self.jitPollTimer) {
    [self startJITPoll];
  }
  if (!self.gameRunning && self.launcherOverlayView.hidden == NO) {
    [self refreshImportedGamesAsync];
  }
  [self presentPendingTouchLayoutInstallIfReady];
}

- (void)handleControllerConnectionChanged:(NSNotification*)note {
  [self.controllerNavigationCoordinator refreshLauncherFocus];
  [self.controllerNavigationCoordinator refreshInGameFocus];
  [self updateTouchControlsOverlayVisibilityAnimated:YES];
  // Losing the last controller mid-game opens the menu and pauses the game, instead of leaving
  // the player under fire (the menu blocks input but doesn't pause). Closing the menu resumes it.
  if ([note.name isEqualToString:GCControllerDidDisconnectNotification] && self.gameRunning &&
      self.launcherOverlayView.hidden && ![self hasConnectedGameplayController]) {
    const BOOL menu_was_hidden = self.inGameMenuOverlay.hidden;
    [self showInGameMenuOverlayAnimated:YES];
    if (menu_was_hidden) {
      [self setEmulationPausedByUser:YES];
    }
  }
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  xe_request_current_orientation(self);
  if ([self presentOnlineFeaturesNoticeIfNeeded]) {
    return;
  }
  [self runAppearanceTasks];
}

- (void)runAppearanceTasks {
  [self startCompatFetchIfNeeded];
  [self startUpdateCheckIfNeeded];
  [self presentPendingTouchLayoutInstallIfReady];
  [self.zarConversionCoordinator checkPendingConversionOnLaunch];
  xe_ios_offer_previous_session_report_if_needed(self);
}

// Once, before the first launch fetches anything: what goes over the network and that
// reports are public. Installs that answered it before it covered diagnostics are asked
// once about those instead. Diagnostics can be declined on their own, leaving the other
// online features on. The appearance tasks run when it closes.
- (BOOL)presentOnlineFeaturesNoticeIfNeeded {
  const BOOL first_launch = xe_ios_online_features_notice_pending();
  if (!first_launch && !xe_ios_diagnostics_notice_pending()) {
    return NO;
  }
  if (online_features_notice_presented_) {
    return YES;
  }
  // Only when nothing else is presented from the root: an alert shown over another one is
  // dismissed with it and would never be answered. Alerts don't trigger viewDidAppear, so
  // retry until the screen is free.
  if (self.presentedViewController || !self.view.window) {
    online_features_notice_presented_ = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     online_features_notice_presented_ = NO;
                     if (![self presentOnlineFeaturesNoticeIfNeeded]) {
                       [self runAppearanceTasks];
                     }
                   });
    return YES;
  }
  online_features_notice_presented_ = YES;
  const BOOL diagnostics = xe_ios_diagnostics_available();
  NSString* first_launch_message =
      diagnostics
          ? NSLocalizedString(@"XeniOS checks for updates, downloads compatibility information "
                              @"and cover art, and sends anonymous crash reports and performance "
                              @"statistics to help fix problems. It also links System Link games "
                              @"with other players over the internet, who can see your IP "
                              @"address and a profile ID. Compatibility reports you submit are "
                              @"public.\n\nYou can change this in Settings > About > Privacy.",
                              @"Alert message: first-run online features notice")
          : NSLocalizedString(@"XeniOS checks for updates, downloads compatibility information "
                              @"and cover art, and links System Link games with other players "
                              @"over the internet, who can see your IP address and a profile "
                              @"ID. Compatibility reports you submit are public.\n\nYou can "
                              @"change this in Settings > About > Privacy.",
                              @"Alert message: first-run online features notice");
  UIAlertController* alert =
      first_launch
          ? [UIAlertController
                alertControllerWithTitle:NSLocalizedString(
                                             @"Online Features",
                                             @"Alert title: first-run online features notice")
                                 message:first_launch_message
                          preferredStyle:UIAlertControllerStyleAlert]
          : [UIAlertController
                alertControllerWithTitle:NSLocalizedString(
                                             @"Crash Reports and Performance",
                                             @"Alert title: one-time notice that diagnostics are "
                                             @"now sent, for installs that saw the older notice")
                                 message:NSLocalizedString(
                                             @"XeniOS can now send anonymous crash reports and "
                                             @"performance statistics, such as frame rate and "
                                             @"device temperature, to help fix problems. They "
                                             @"never include your files, your name or your Apple "
                                             @"Account.\n\nYou can change this in Settings > "
                                             @"About > Privacy.",
                                             @"Alert message: one-time diagnostics notice")
                          preferredStyle:UIAlertControllerStyleAlert];
  void (^answer)(BOOL, BOOL) = ^(BOOL online, BOOL share_diagnostics) {
    if (first_launch) {
      xe_ios_set_online_features_enabled(online);
      xe_ios_set_online_features_notice_answered();
    } else {
      xe_ios_set_diagnostics_notice_answered();
    }
    xe_ios_set_share_diagnostics_enabled(share_diagnostics);
    xe_ios_diagnostics_settings_changed();
    [self runAppearanceTasks];
  };
  NSString* decline =
      first_launch ? NSLocalizedString(@"Turn Off", @"Alert button: disables online features")
                   : NSLocalizedString(@"Don't Send",
                                       @"Alert button: turns off crash reports and statistics");
  [alert addAction:[UIAlertAction actionWithTitle:decline
                                            style:UIAlertActionStyleDefault
                                          handler:^(__unused UIAlertAction* action) {
                                            // The one-time notice declines diagnostics only.
                                            answer(!first_launch, NO);
                                          }]];
  if (first_launch && diagnostics) {
    // Declining diagnostics must not cost the rest, such as System Link over the internet.
    [alert addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(
                                             @"Don't Send Diagnostics",
                                             @"Alert button: keeps online features on, without "
                                             @"crash reports and statistics")
                                   style:UIAlertActionStyleDefault
                                 handler:^(__unused UIAlertAction* action) {
                                   answer(YES, NO);
                                 }]];
  }
  UIAlertAction* keep_on = [UIAlertAction
      actionWithTitle:NSLocalizedString(@"Continue",
                                        @"Alert button: keeps online features or diagnostics on")
                style:UIAlertActionStyleDefault
              handler:^(__unused UIAlertAction* action) {
                answer(YES, YES);
              }];
  [alert addAction:keep_on];
  alert.preferredAction = keep_on;
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:alert animated:YES completion:nil];
  return YES;
}

- (void)startUpdateCheckIfNeeded {
  if (update_check_started_) {
    return;
  }
  if (!xe_ios_online_features_enabled()) {
    return;
  }
  update_check_started_ = YES;
  // Throttled to once per day inside xe_check_for_update. The block retains self
  // for the duration of the request.
  xe_check_for_update(NO, ^(XeniaUpdateInfo* info) {
    if (!info || info.status != XeniaUpdateStatusUpdateAvailable) {
      return;
    }
    if (!info.latestVersionString.length && info.latestBuild <= 0) {
      return;
    }
    // Notify at most once per newer build so we don't nag every launch.
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    long long notified_build = (long long)[defaults integerForKey:@"XeniaUpdateLastNotifiedBuild"];
    if (info.latestBuild > 0 && notified_build == info.latestBuild) {
      return;
    }
    [defaults setInteger:(NSInteger)info.latestBuild forKey:@"XeniaUpdateLastNotifiedBuild"];
    [self presentUpdateAvailableNotificationForInfo:info];
  });
}

- (void)presentUpdateAvailableNotificationForInfo:(XeniaUpdateInfo*)info {
  NSString* latest =
      info.latestVersionText.length
          ? info.latestVersionText
          : NSLocalizedString(@"a new version", @"Fallback version text in the update notice");
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  // Avoid stacking on a launch-time alert (JIT/entitlement prompts); fall back
  // to a toast and let Settings › About carry the actionable link.
  if ([presenter isKindOfClass:[UIAlertController class]]) {
    [self
        showStatusToast:[NSString
                            stringWithFormat:NSLocalizedString(
                                                 @"Update available: %@",
                                                 @"Toast: an update is available; %@ is a version"),
                                             latest]
                  style:XeniaIOSStatusToastStyleInfo];
    return;
  }

  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Update Available",
                                                 @"Alert title: update available")
                       message:[NSString
                                   stringWithFormat:
                                       NSLocalizedString(
                                           @"XeniOS %1$@ is available. You're on %2$@.",
                                           @"Alert message: update available; %1$@ is the new "
                                           @"version, %2$@ the current one"),
                                       latest,
                                       info.currentVersionText
                                           ?: NSLocalizedString(@"this build",
                                                                @"Fallback for the current version "
                                                                @"text in the update notice")]
                preferredStyle:UIAlertControllerStyleAlert];
  NSURL* download_target = [NSURL URLWithString:info.downloadURLString ?: @""];
  if (!xe_ios_is_trusted_release_url(download_target)) {
    download_target = [NSURL URLWithString:@"https://xenios.jp/downloads"];
  }
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Download",
                                                         @"Alert button: opens the download page")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 [[UIApplication sharedApplication] openURL:download_target
                                                                    options:@{}
                                                          completionHandler:nil];
                               }]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(
                                           @"Later", @"Alert button: dismisses the update notice")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (void)startCompatFetchIfNeeded {
  if (compat_fetch_started_) {
    return;
  }
  // Online features off: the cached list still loads, and turning them on later starts the
  // fetch on the next appearance.
  if (xe_ios_online_features_enabled()) {
    compat_fetch_started_ = YES;
  }
  xe_fetch_compat_data(^(NSDictionary* data) {
    if (!data) {
      return;
    }
    if (self->compat_data_ && [self->compat_data_ isEqualToDictionary:data]) {
      return;
    }
    std::filesystem::path focused_path;
    const NSInteger focused_game_index =
        self.controllerNavigationCoordinator.focusedGameIndex - [self launcherGameItemOffset];
    const BOOL has_focused_path =
        focused_game_index >= 0 &&
        focused_game_index < static_cast<NSInteger>(self->discovered_games_.size());
    if (has_focused_path) {
      focused_path = self->discovered_games_[static_cast<size_t>(focused_game_index)].path;
    }
    [self->compat_data_ release];
    self->compat_data_ = [data retain];
    [self applyCompatDataToDiscoveredGames];
    SortDiscoveredGames(&self->discovered_games_);
    if (has_focused_path) {
      auto focused_it = std::find_if(
          self->discovered_games_.begin(), self->discovered_games_.end(),
          [&focused_path](const IOSDiscoveredGame& game) { return game.path == focused_path; });
      if (focused_it != self->discovered_games_.end()) {
        [self.controllerNavigationCoordinator
            setFocusedGameIndex:static_cast<NSInteger>(
                                    std::distance(self->discovered_games_.begin(), focused_it)) +
                                [self launcherGameItemOffset]
                         scroll:NO];
      }
    }
    [self refreshLauncherGameSnapshots];
    [self.controllerNavigationCoordinator refreshLauncherFocus];
  });
}

- (void)onCompatDataDidUpdate:(NSNotification*)__unused notification {
  NSDictionary* cached_compat_data = xe_load_cached_compat_data();
  if (!cached_compat_data) {
    return;
  }
  std::filesystem::path focused_path;
  const NSInteger focused_game_index =
      self.controllerNavigationCoordinator.focusedGameIndex - [self launcherGameItemOffset];
  const BOOL has_focused_path =
      focused_game_index >= 0 &&
      focused_game_index < static_cast<NSInteger>(discovered_games_.size());
  if (has_focused_path) {
    focused_path = discovered_games_[static_cast<size_t>(focused_game_index)].path;
  }
  [compat_data_ release];
  compat_data_ = [cached_compat_data retain];
  [self applyCompatDataToDiscoveredGames];
  SortDiscoveredGames(&discovered_games_);
  if (has_focused_path) {
    auto focused_it = std::find_if(
        discovered_games_.begin(), discovered_games_.end(),
        [&focused_path](const IOSDiscoveredGame& game) { return game.path == focused_path; });
    if (focused_it != discovered_games_.end()) {
      [self.controllerNavigationCoordinator
          setFocusedGameIndex:static_cast<NSInteger>(
                                  std::distance(discovered_games_.begin(), focused_it)) +
                              [self launcherGameItemOffset]
                       scroll:NO];
    }
  }
  [self refreshLauncherGameSnapshots];
  [self.controllerNavigationCoordinator refreshLauncherFocus];
}

#pragma mark - Controller navigation host

- (BOOL)controllerNavigationLauncherVisible {
  return self.launcherOverlayView && !self.launcherOverlayView.hidden;
}

- (BOOL)controllerNavigationLauncherActionsEnabled {
  return self.launcherOverlayView.actionsEnabled;
}

- (NSInteger)controllerNavigationGameCount {
  return static_cast<NSInteger>(discovered_games_.size()) + [self launcherGameItemOffset];
}

- (NSInteger)controllerNavigationLauncherColumnCount {
  return [self.launcherOverlayView columnCount];
}

- (NSInteger)controllerNavigationLauncherPageStep {
  return [self.launcherOverlayView pageStep];
}

- (BOOL)controllerNavigationGameRunning {
  return self.gameRunning;
}

- (BOOL)controllerNavigationInGameMenuVisible {
  return self.inGameMenuOverlay && !self.inGameMenuOverlay.hidden;
}

- (BOOL)controllerNavigationInGameMenuActionEnabled:(XeniaIOSInGameMenuAction)action {
  return [self.inGameMenuOverlay isActionEnabled:action];
}

- (UIViewController*)controllerNavigationPresentedController {
  return self.presentedViewController;
}

- (BOOL)controllerNavigationHasConnectedController {
  return [self hasConnectedGameplayController];
}

- (BOOL)controllerNavigationReadEmulatorControllerState:(xe::hid::X_INPUT_STATE*)outState {
  if (!self.appContext || !outState) {
    return NO;
  }
  for (uint32_t user_index = 0; user_index < xe::XUserMaxUserCount; ++user_index) {
    if (self.appContext->GetControllerState(user_index, outState)) {
      return YES;
    }
  }
  return NO;
}

- (void)controllerNavigationApplyFocusedGameIndex:(NSInteger)index scroll:(BOOL)scroll {
  [self.launcherOverlayView setFocusedGameIndex:index scroll:scroll];
}

- (void)controllerNavigationApplyLauncherFocusEnabled:(BOOL)enabled
                                      settingsFocused:(BOOL)settingsFocused
                                       profileFocused:(BOOL)profileFocused
                                        importFocused:(BOOL)importFocused
                                   libraryFocusActive:(BOOL)libraryFocusActive {
  [self.launcherOverlayView setControllerNavigationEnabled:enabled
                                           settingsFocused:settingsFocused
                                            profileFocused:profileFocused
                                             importFocused:importFocused
                                        libraryFocusActive:libraryFocusActive];
}

- (void)controllerNavigationApplyInGameMenuFocusEnabled:(BOOL)enabled
                                          focusedAction:(XeniaIOSInGameMenuAction)focusedAction {
  [self.inGameMenuOverlay setControllerNavigationEnabled:enabled focusedAction:focusedAction];
}

- (void)controllerNavigationOpenSettings {
  [self openSettingsTapped:nil];
}

- (void)controllerNavigationOpenProfile {
  [self openProfileTapped:nil];
}

- (void)controllerNavigationImportGame {
  [self openGameTapped:nil];
}

- (void)controllerNavigationManageGameAtIndex:(NSInteger)index {
  if ([self launcherGameItemOffset] && index == 0) {
    [self openSystemSoftware];
    return;
  }
  index -= [self launcherGameItemOffset];
  if (index < 0 || index >= static_cast<NSInteger>(discovered_games_.size())) {
    return;
  }
  [self presentGameActionsSheetForIndex:static_cast<size_t>(index)];
}

- (void)controllerNavigationLaunchGameAtIndex:(NSInteger)index {
  if ([self launcherGameItemOffset] && index == 0) {
    [self openDashboard];
    return;
  }
  index -= [self launcherGameItemOffset];
  if (index < 0 || index >= static_cast<NSInteger>(discovered_games_.size())) {
    return;
  }
  const IOSDiscoveredGame& game = discovered_games_[static_cast<size_t>(index)];
  [self launchGameAtPath:game.path displayName:ToNSString(game.title)];
}

- (void)controllerNavigationShowInGameMenu {
  [self showInGameMenuOverlayAnimated:YES];
}

- (void)controllerNavigationHideInGameMenu {
  [self hideInGameMenuOverlay];
}

- (void)controllerNavigationPerformInGameMenuAction:(XeniaIOSInGameMenuAction)action {
  [self.inGameMenuOverlay performAction:action];
}

// ---------------------------------------------------------------------------
// JIT polling -- starts responsive, then backs off to avoid burning battery
// forever when StikDebug / JIT is unavailable.
// ---------------------------------------------------------------------------
- (void)startJITPoll {
  // Check immediately first.
  if (xe_check_jit_available()) {
    [self onJITAcquired];
    return;
  }

  XELOGI("iOS: JIT not yet available, polling...");
  self.jitPollInterval = 0.5;
  self.jitPollStartedAt = [NSDate timeIntervalSinceReferenceDate];
  self.jitPollTimedOut = NO;
  [self scheduleNextJITPoll];
}

- (void)pollJIT:(NSTimer*)timer {
  if (timer != self.jitPollTimer) {
    return;
  }
  self.jitPollTimer = nil;
  if (xe_check_jit_available()) {
    [self onJITAcquired];
    return;
  }

  const NSTimeInterval elapsed = [NSDate timeIntervalSinceReferenceDate] - self.jitPollStartedAt;
  if (elapsed >= 300.0) {
    self.jitPollTimedOut = YES;
    [self updateJITStatusIndicator];
    // The JIT card on the launcher carries the timed-out state and its retry action; a toast
    // would vanish before the user could act on it.
    return;
  }

  self.jitPollInterval = std::min<NSTimeInterval>(self.jitPollInterval * 2.0, 10.0);
  [self scheduleNextJITPoll];
}

- (void)scheduleNextJITPoll {
  [self.jitPollTimer invalidate];
  self.jitPollTimer = [NSTimer scheduledTimerWithTimeInterval:self.jitPollInterval
                                                       target:self
                                                     selector:@selector(pollJIT:)
                                                     userInfo:nil
                                                      repeats:NO];
}

- (void)onJITAcquired {
  [self.jitPollTimer invalidate];
  self.jitPollTimer = nil;
  self.jitPollTimedOut = NO;
  self.jitAcquired = YES;
  XELOGI("iOS: JIT acquired!");
  [self updateJITStatusIndicator];
  [self updateJITAvailabilityUI];

  std::filesystem::path queued_path = pending_external_launch_path_;
  std::filesystem::path persisted_path = TakePendingExternalLaunchPathPreference();
  pending_external_launch_path_.clear();

  if ([self consumePendingStikDebugAutomationPrompt]) {
    deferred_jit_prompt_queued_launch_path_ = queued_path;
    deferred_jit_prompt_persisted_launch_path_ = persisted_path;
    [self presentStikDebugAutomationPromptWithCompletion:^{
      std::filesystem::path deferred_queued = self->deferred_jit_prompt_queued_launch_path_;
      std::filesystem::path deferred_persisted = self->deferred_jit_prompt_persisted_launch_path_;
      self->deferred_jit_prompt_queued_launch_path_.clear();
      self->deferred_jit_prompt_persisted_launch_path_.clear();
      [self finishJITAcquiredPendingLaunchWithQueuedPath:deferred_queued
                                           persistedPath:deferred_persisted];
    }];
    return;
  }

  [self finishJITAcquiredPendingLaunchWithQueuedPath:queued_path persistedPath:persisted_path];
}

- (void)finishJITAcquiredPendingLaunchWithQueuedPath:(const std::filesystem::path&)queuedPath
                                       persistedPath:(const std::filesystem::path&)persistedPath {
  if (queuedPath.empty() && persistedPath.empty()) {
    return;
  }

  std::filesystem::path path_to_launch = !queuedPath.empty() ? queuedPath : persistedPath;
  NSString* display_name = [self displayNameForGamePath:path_to_launch];
  if (!display_name || display_name.length == 0) {
    display_name = ToNSString(path_to_launch.filename().string());
  }
  XELOGI("iOS: Launching queued external request: {}", path_to_launch.string());
  [self launchGameAtPath:path_to_launch displayName:display_name];
}

- (BOOL)consumePendingStikDebugAutomationPrompt {
  if (!GetUserDefaultBool(kXeniaPendingStikDebugAutomationPromptPreferenceKey, false)) {
    return NO;
  }
  SetUserDefaultBool(kXeniaPendingStikDebugAutomationPromptPreferenceKey, false);
  return !GetUserDefaultBool(kXeniaAutoOpenStikDebugOnLaunchPreferenceKey, false);
}

- (void)presentStikDebugAutomationPromptWithCompletion:(void (^)(void))completion {
  if (presenting_stikdebug_automation_prompt_) {
    if (completion) {
      completion();
    }
    return;
  }

  presenting_stikdebug_automation_prompt_ = YES;
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:
          NSLocalizedString(@"Always Auto-Enable JIT?",
                            @"Alert title: offers to enable JIT through StikDebug automatically")
                       message:NSLocalizedString(
                                   @"StikDebug enabled JIT successfully. XeniOS can use the same "
                                   @"StikDebug handoff automatically next time JIT is missing.",
                                   @"Alert message: offers to enable JIT through StikDebug "
                                   @"automatically")
                preferredStyle:UIAlertControllerStyleAlert];

  void (^finish)(void) = ^{
    self->presenting_stikdebug_automation_prompt_ = NO;
    if (completion) {
      completion();
    }
  };

  [alert addAction:
             [UIAlertAction
                 actionWithTitle:NSLocalizedString(@"Always Enable",
                                                   @"Alert button: turn on automatic StikDebug JIT")
                           style:UIAlertActionStyleDefault
                         handler:^(__unused UIAlertAction* action) {
                           SetUserDefaultBool(kXeniaAutoOpenStikDebugOnLaunchPreferenceKey, true);
                           [self
                               showStatusToast:NSLocalizedString(
                                                   @"Auto-enable JIT is on.",
                                                   @"Toast: automatic StikDebug JIT was turned on")
                                         style:XeniaIOSStatusToastStyleSuccess];
                           finish();
                         }]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Not Now",
                                                         @"Alert button: dismiss without enabling")
                                 style:UIAlertActionStyleCancel
                               handler:^(__unused UIAlertAction* action) {
                                 finish();
                               }]];
  [self presentViewController:alert animated:YES completion:nil];
}

// ---------------------------------------------------------------------------
// Launcher overlay — delegated to XeniaIOSLauncherOverlayView.
// ---------------------------------------------------------------------------
- (void)setupLauncherOverlay {
  XeniaIOSLauncherOverlayView* launcher_overlay =
      [[XeniaIOSLauncherOverlayView alloc] initWithFrame:self.view.bounds];
  self.launcherOverlayView = launcher_overlay;
  [launcher_overlay release];
  [self.view addSubview:self.launcherOverlayView];

  __unsafe_unretained XeniaViewController* unsafe_self = self;
  self.launcherOverlayView.settingsHandler = ^{
    [unsafe_self openSettingsTapped:nil];
  };
  self.launcherOverlayView.profileHandler = ^{
    [unsafe_self openProfileTapped:nil];
  };
  self.launcherOverlayView.importHandler = ^{
    [unsafe_self presentLibraryAddSheetFromView:unsafe_self.launcherOverlayView];
  };
  self.launcherOverlayView.dashboardHandler = ^{
    [unsafe_self openDashboard];
  };
  self.launcherOverlayView.dashboardSystemSoftwareHandler = ^{
    [unsafe_self openSystemSoftware];
  };
  [self.launcherOverlayView setDashboardInstalled:xe::ui::IsIOSDashboardInstalled()];
  self.launcherOverlayView.openStikDebugHandler = ^{
    [unsafe_self requestStikDebugJITHandoffForPendingLaunchPath:nullptr
                                              requirePreference:NO
                                   promptForAutomationOnSuccess:NO];
  };
  self.launcherOverlayView.bulkZarConversionHandler = ^{
    [unsafe_self.zarConversionCoordinator
        presentBulkConversionOptionsForGames:unsafe_self->discovered_games_];
  };
  self.launcherOverlayView.gameLaunchedHandler = ^(NSUInteger gameIndex) {
    if (gameIndex >= unsafe_self->discovered_games_.size()) {
      return;
    }
    const IOSDiscoveredGame& game = unsafe_self->discovered_games_[gameIndex];
    [unsafe_self launchGameAtPath:game.path displayName:ToNSString(game.title)];
  };
  self.launcherOverlayView.compatibilityHandler = ^(NSUInteger gameIndex) {
    [unsafe_self presentCompatibilitySheetForIndex:gameIndex];
  };
  self.launcherOverlayView.gameSettingsHandler = ^(NSUInteger gameIndex) {
    [unsafe_self presentGameSettingsSheetForIndex:gameIndex];
  };
  self.launcherOverlayView.touchLayoutHandler = ^(NSUInteger gameIndex) {
    [unsafe_self presentGameTouchLayoutSheetForIndex:gameIndex];
  };
  self.launcherOverlayView.manageContentHandler = ^(NSUInteger gameIndex) {
    [unsafe_self presentManageContentSheetForIndex:gameIndex];
  };
  self.launcherOverlayView.discSelectionHandler = ^(NSUInteger gameIndex) {
    [unsafe_self presentDiscSelectionSheetForIndex:gameIndex];
  };
  self.launcherOverlayView.patchesHandler = ^(NSUInteger gameIndex) {
    [unsafe_self presentPatchesSheetForIndex:gameIndex];
  };
  self.launcherOverlayView.zarConversionHandler = ^(NSUInteger gameIndex) {
    [unsafe_self.zarConversionCoordinator
        presentConversionOptionsForGames:unsafe_self->discovered_games_
                                   index:gameIndex];
  };
  self.launcherOverlayView.copyLaunchURLHandler = ^(NSUInteger gameIndex) {
    [unsafe_self copyLaunchURLForGameAtIndex:gameIndex];
  };

  // Allocated but off-screen — existing code can set .text without crashing.
  UILabel* signed_in_profile_label = [[UILabel alloc] init];
  self.signedInProfileLabel = signed_in_profile_label;
  [signed_in_profile_label release];
}

- (void)refreshLauncherGameSnapshots {
  NSMutableArray<XeniaIOSLauncherGameSnapshot*>* snapshots =
      [NSMutableArray arrayWithCapacity:discovered_games_.size()];
  NSMutableArray<NSString*>* signature =
      [NSMutableArray arrayWithCapacity:discovered_games_.size()];
  NSMutableDictionary<NSString*, NSData*>* next_icon_cache =
      [NSMutableDictionary dictionaryWithCapacity:discovered_games_.size()];
  for (const IOSDiscoveredGame& game : discovered_games_) {
    XeniaIOSLauncherGameSnapshot* snapshot = [[XeniaIOSLauncherGameSnapshot alloc] init];
    snapshot.title =
        game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
    snapshot.titleId = game.title_id;
    snapshot.hasCompatInfo = game.has_compat_info;
    snapshot.compatStatus = ToNSString(game.compat_status);
    NSMutableArray<NSString*>* detail_parts = [NSMutableArray array];
    bool has_imported_source = false;
    bool has_external_source = false;
    if (!game.discs.empty()) {
      for (const IOSDiscoveredGame::Disc& disc : game.discs) {
        if (disc.has_external_source || disc.is_external) {
          has_external_source = true;
        }
        if (disc.has_imported_source || (!disc.is_external && disc.source_label.empty())) {
          has_imported_source = true;
        }
      }
    } else if (game.is_external) {
      has_external_source = true;
    } else {
      has_imported_source = true;
    }
    if (has_imported_source && has_external_source) {
      [detail_parts
          addObject:NSLocalizedString(@"Imported + External",
                                      @"Game source label: file is both imported and external")];
    } else if (has_external_source) {
      [detail_parts
          addObject:NSLocalizedString(@"External", @"Game source label: file is external")];
    } else if (has_imported_source) {
      [detail_parts
          addObject:NSLocalizedString(@"Imported", @"Game source label: file is imported")];
    }
    if (!game.content_type_name.empty() &&
        (game.content_type == xe::XContentType::kArcadeTitle ||
         game.content_type == xe::XContentType::kGameDemo ||
         game.content_type == xe::XContentType::kCommunityGame)) {
      [detail_parts addObject:ToNSString(game.content_type_name)];
    }
    if (game.discs.size() > 1) {
      [detail_parts
          addObject:[NSString
                        stringWithFormat:NSLocalizedString(
                                             @"%zu Discs",
                                             @"Game detail label; %zu is the number of discs"),
                                         game.discs.size()]];
    }
    if (detail_parts.count > 0) {
      snapshot.contentTypeName = [detail_parts componentsJoinedByString:@" · "];
    }
    snapshot.supportsCompatibility =
        game.title_id != 0 && xe_game_system_supports_compatibility(game.system);
    snapshot.supportsManageContent =
        game.title_id != 0 && xe_game_system_supports_manage_content(game.system);
    snapshot.supportsDiscSelection = game.discs.size() > 1;
    snapshot.discCount = game.discs.size();
    snapshot.supportsPatches = self.appContext && game.title_id != 0;
    snapshot.supportsZarConversion = xe_game_supports_zar_conversion(game);
    snapshot.supportsRemoteArt = xe_game_system_supports_remote_art(game.system);
    if (!game.icon_data.empty()) {
      // Reuse the NSData from the last refresh when the bytes are identical, instead of copying
      // every icon again on each refresh.
      NSString* icon_key = ToNSString(game.path.string());
      NSData* icon = [launcher_icon_cache_ objectForKey:icon_key];
      if (!icon || icon.length != game.icon_data.size() ||
          memcmp(icon.bytes, game.icon_data.data(), icon.length) != 0) {
        icon = [NSData dataWithBytes:game.icon_data.data() length:game.icon_data.size()];
      }
      snapshot.iconData = icon;
      [next_icon_cache setObject:icon forKey:icon_key];
    }
    [signature addObject:[NSString stringWithFormat:@"%@|%@|%u|%d|%@|%@|%d%d%d%d%d%d|%zu|%lu",
                                                    ToNSString(game.path.string()), snapshot.title,
                                                    snapshot.titleId, (int)snapshot.hasCompatInfo,
                                                    snapshot.compatStatus,
                                                    snapshot.contentTypeName ?: @"",
                                                    (int)snapshot.supportsCompatibility,
                                                    (int)snapshot.supportsManageContent,
                                                    (int)snapshot.supportsDiscSelection,
                                                    (int)snapshot.supportsPatches,
                                                    (int)snapshot.supportsZarConversion,
                                                    (int)snapshot.supportsRemoteArt,
                                                    snapshot.discCount,
                                                    (unsigned long)snapshot.iconData.length]];
    [snapshots addObject:snapshot];
    [snapshot release];
  }
  [launcher_icon_cache_ release];
  launcher_icon_cache_ = [next_icon_cache retain];
  if (launcher_snapshot_signature_ && [launcher_snapshot_signature_ isEqualToArray:signature]) {
    // Nothing the grid shows has changed (a foreground return, a no-op rescan): keep the cells,
    // their decoded covers and any scroll or context menu in progress.
    return;
  }
  [launcher_snapshot_signature_ release];
  launcher_snapshot_signature_ = [signature copy];
  [self.launcherOverlayView setGames:snapshots];
  [self.launcherOverlayView
      setFocusedGameIndex:self.controllerNavigationCoordinator.focusedGameIndex
                   scroll:NO];
}

- (void)setupInGameMenuOverlay {
  XeniaIOSInGameMenuOverlay* overlay =
      [[XeniaIOSInGameMenuOverlay alloc] initWithFrame:self.view.bounds];
  self.inGameMenuOverlay = overlay;
  [overlay release];
  [self.view addSubview:self.inGameMenuOverlay];

  __unsafe_unretained XeniaViewController* unsafe_self = self;
  self.inGameMenuOverlay.resumeHandler = ^{
    [unsafe_self resumeGameTapped:nil];
  };
  self.inGameMenuOverlay.editControlsHandler = ^{
    [unsafe_self inGameEditControlsTapped:nil];
  };
  self.inGameMenuOverlay.achievementsHandler = ^{
    [unsafe_self inGameAchievementsTapped:nil];
  };
  self.inGameMenuOverlay.settingsHandler = ^{
    [unsafe_self inGameSettingsTapped:nil];
  };
  self.inGameMenuOverlay.liveLogHandler = ^{
    [unsafe_self inGameLiveLogTapped:nil];
  };
  self.inGameMenuOverlay.exitHandler = ^{
    if (unsafe_self.inGameMenuOverlay.exitsToDashboard) {
      [unsafe_self exitGameToDashboard];
    } else {
      [unsafe_self exitGameTapped:nil];
    }
  };
  self.inGameMenuOverlay.graphicsHandler = ^{
    [unsafe_self presentQuickSettings];
  };
  self.inGameMenuOverlay.pauseEmulatorHandler = ^{
    [unsafe_self setEmulationPausedByUser:!unsafe_self.inGameMenuOverlay.emulatorPaused];
  };
  [self refreshInGameDisplayMenu];
}

- (void)refreshInGameDisplayMenu {
  self.inGameMenuOverlay.displayMenu = [self buildInGameDisplayMenu];
}

- (void)toggleInGameMenuTapped:(UITapGestureRecognizer*)recognizer {
  if (recognizer.state != UIGestureRecognizerStateRecognized) {
    return;
  }
  if (self.launcherOverlayView.hidden == NO || !self.gameRunning || self.presentedViewController) {
    return;
  }

  // If the in-game menu is showing AND the tap landed on one of its buttons,
  // do NOT toggle the menu — let the button handle its own action. Without
  // this guard, opening the Display button's UIMenu (or any other menu-button
  // in the panel) would also dismiss the surrounding in-game menu overlay,
  // leaving the user with a floating UIMenu over empty space and forcing
  // them to tap-to-reopen afterwards. Tap recognizer was originally added
  // with cancelsTouchesInView=NO so the button still receives the tap.
  if (!self.inGameMenuOverlay.hidden) {
    CGPoint tap_in_overlay = [recognizer locationInView:self.inGameMenuOverlay];
    UIView* hit = [self.inGameMenuOverlay hitTest:tap_in_overlay withEvent:nil];
    if (hit && hit != self.inGameMenuOverlay) {
      return;
    }
  }

  if (self.inGameMenuOverlay.hidden) {
    [self showInGameMenuOverlayAnimated:YES];
  } else {
    [self hideInGameMenuOverlay];
  }
}

- (void)showInGameMenuOverlayAnimated:(BOOL)animated {
  if (!self.inGameMenuOverlay.hidden) {
    return;
  }

  if (touch_layout_edit_mode_active_) {
    [self saveTouchLayoutModelForTitleID:active_game_title_id_];
    [self setTouchLayoutEditModeActive:NO animated:NO];
  }

  [self refreshInGameDisplayMenu];
  [self.inGameMenuOverlay setOverlayVisible:YES animated:animated completion:nil];
  [self.controllerNavigationCoordinator focusDefaultInGameAction];
  [self updateTouchControlsOverlayVisibilityAnimated:NO];
}

- (void)hideInGameMenuOverlay {
  if (self.inGameMenuOverlay.hidden) {
    return;
  }
  [self.inGameMenuOverlay setOverlayVisible:NO
                                   animated:YES
                                 completion:^(__unused BOOL finished) {
                                   [self.controllerNavigationCoordinator refreshInGameFocus];
                                   [self updateTouchControlsOverlayVisibilityAnimated:YES];
                                 }];
}

- (void)setEmulationPausedByUser:(BOOL)paused {
  emulation_paused_by_user_ = paused;
  self.inGameMenuOverlay.emulatorPaused = paused;
  if (self.appContext) {
    self.appContext->SetEmulationPaused(xe::ui::IOSWindowedAppContext::EmulationPauseReason::kUser,
                                        paused ? true : false);
  }
}

- (void)resumeGameTapped:(UIButton*)sender {
  [self hideInGameMenuOverlay];
}

- (void)inGameEditControlsTapped:(UIButton*)sender {
  [self hideInGameMenuOverlay];
  [self setTouchLayoutEditModeActive:YES animated:YES];
}

- (void)inGameAchievementsTapped:(UIButton*)sender {
  [self hideInGameMenuOverlay];
  [self presentAchievementsForUserIndex:0 titleID:active_game_title_id_ completion:nil];
}

- (void)inGameSettingsTapped:(UIButton*)sender {
  [self hideInGameMenuOverlay];
  [self openSettingsTapped:nil];
}

- (XeniaLandscapeNavigationController*)
    presentPauseSheetWithRootController:(UIViewController*)rootController
                          preferredSize:(CGSize)preferredSize
            preventInteractiveDismissal:(BOOL)preventInteractiveDismissal
                             completion:(void (^)(void))completion {
  if (self.gameRunning) {
    gameplay_modal_presentation_pending_ = YES;
    [self updateTouchControlsOverlayVisibilityAnimated:YES];
  }
  XeniaLandscapeNavigationController* nav =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:rootController];
  XEConfigureTaskSheet(nav, self.view, preferredSize, preventInteractiveDismissal);
  nav.presentationController.delegate = self;
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:nav
                          animated:YES
                        completion:^{
                          gameplay_modal_presentation_pending_ = NO;
                          [self updateTouchControlsOverlayVisibilityAnimated:YES];
                          if (completion) {
                            completion();
                          }
                        }];
  return [nav autorelease];
}

- (void)presentQuickSettings {
  [self hideInGameMenuOverlay];
  // Name the running game so the sheet says which game its changes are saved for.
  NSString* quick_game_title = nil;
  for (const IOSDiscoveredGame& game : discovered_games_) {
    if (game.title_id == active_game_title_id_ && active_game_title_id_ != 0) {
      quick_game_title =
          game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
      break;
    }
  }
  XeniaIOSQuickSettingsViewController* quickVC =
      [[XeniaIOSQuickSettingsViewController alloc] initWithGameTitleID:active_game_title_id_
                                                             gameTitle:quick_game_title];
  quickVC.appContext = self.appContext;
  [self presentPauseSheetWithRootController:quickVC
                              preferredSize:CGSizeMake(480.0, 560.0)
                preventInteractiveDismissal:NO
                                 completion:nil];
  [quickVC release];
}

- (void)inGameLiveLogTapped:(UIButton*)sender {
  [self hideInGameMenuOverlay];
  XeniaLogViewController* log_vc = [[XeniaLogViewController alloc] init];
  [self presentPauseSheetWithRootController:log_vc
                              preferredSize:CGSizeMake(560.0, 620.0)
                preventInteractiveDismissal:NO
                                 completion:nil];
  [log_vc release];
}

// A game the dashboard started goes back to it, as the game's own exit does.
- (void)exitGameToDashboard {
  [self hideInGameMenuOverlay];
  if (self.gameStopInProgress) {
    return;
  }
  [self launchDashboard];
}

- (void)exitGameTapped:(UIButton*)sender {
  [self hideInGameMenuOverlay];
  if (self.gameStopInProgress) {
    // The "Stopping game..." state is already on screen until teardown finishes.
    return;
  }
  if (!self.appContext) {
    return;
  }

  self.gameStopInProgress = YES;
  self.gameRunning = NO;
  // A guest thread waiting on a message box or a sheet (achievements) would hold up the title's
  // teardown. Answer the prompts first: dismissing one unanswered leaves its thread waiting.
  XeniaCancelGuestAlerts(self);
  if (self.presentedViewController) {
    [self dismissViewControllerAnimated:NO completion:nil];
  }
  self.launcherOverlayView.hidden = NO;
  self.launcherOverlayView.alpha = 1.0;
  [self updateTouchControlsOverlayVisibilityAnimated:YES];
  xe_request_current_orientation(self);
  // Stays up until teardown finishes (showLauncherOverlay or the failure path dismisses it), so
  // the user is not left guessing while the game winds down.
  [self showPersistentStatusToast:NSLocalizedString(@"Stopping game...",
                                                    @"Status while the game is being stopped")
                            style:XeniaIOSStatusToastStyleInfo];

  xe::ui::IOSWindowedAppContext* app_context = self.appContext;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    BOOL requested_stop = app_context->TerminateCurrentGame() ? YES : NO;
    if (requested_stop) {
      return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      self.gameStopInProgress = NO;
      [self dismissStatusToast];
    });
  });
}

- (void)refreshSignedInProfileUI {
  if (!self.appContext) {
    self.launcherOverlayView.actionsEnabled = NO;
    self.launcherOverlayView.alpha = 0.5;
    self.signedInProfileLabel.text = NSLocalizedString(
        @"Profile system unavailable", @"Launcher label: profile services unavailable");
    return;
  }

  self.launcherOverlayView.actionsEnabled = YES;
  self.launcherOverlayView.alpha = 1.0;

  const auto profiles = self.appContext->ListProfiles();
  const xe::ui::IOSProfileSummary* signed_in_profile = nullptr;
  for (const auto& profile : profiles) {
    if (profile.signed_in) {
      signed_in_profile = &profile;
      break;
    }
  }

  if (signed_in_profile) {
    self.signedInProfileLabel.text = [NSString
        stringWithFormat:NSLocalizedString(@"Signed in: %@",
                                           @"Launcher label; %@ is the signed-in gamertag"),
                         ToNSString(signed_in_profile->gamertag)];
  } else if (profiles.empty()) {
    self.signedInProfileLabel.text =
        NSLocalizedString(@"No local profile yet", @"Launcher label: no profile exists");
  } else {
    self.signedInProfileLabel.text = NSLocalizedString(
        @"No profile signed in", @"Launcher label: a profile exists but none is signed in");
  }
}

- (void)presentProfileCreateAlert {
  // Under MRC, `__weak` is unavailable; rely on block strong captures.
  // Use `__block` for the alert to avoid a retain-cycle: alert -> action -> handler -> alert.
  __block UIAlertController* create_alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Create Profile",
                                                 @"Alert title: create a profile")
                       message:NSLocalizedString(@"Enter a gamertag (1-15 characters).",
                                                 @"Alert message: gamertag rules")
                preferredStyle:UIAlertControllerStyleAlert];
  [create_alert addTextFieldWithConfigurationHandler:^(UITextField* text_field) {
    text_field.placeholder =
        NSLocalizedString(@"Gamertag", @"Placeholder of the gamertag text field");
    text_field.autocapitalizationType = UITextAutocapitalizationTypeWords;
    text_field.autocorrectionType = UITextAutocorrectionTypeNo;
    text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
  }];
  [create_alert
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:nil]];
  [create_alert
      addAction:
          [UIAlertAction
              actionWithTitle:NSLocalizedString(@"Create", @"Alert button: create the profile")
                        style:UIAlertActionStyleDefault
                      handler:^(__unused UIAlertAction* action) {
                        if (!self.appContext) {
                          return;
                        }
                        UITextField* text_field = create_alert.textFields.firstObject;
                        NSString* raw_text = text_field.text ?: @"";
                        NSString* trimmed = [raw_text
                            stringByTrimmingCharactersInSet:[NSCharacterSet
                                                                whitespaceAndNewlineCharacterSet]];
                        if (trimmed.length == 0 || !self.appContext) {
                          return;
                        }
                        auto* app_context = self.appContext;
                        if (!app_context) {
                          return;
                        }
                        NSString* gamertag = [[trimmed copy] autorelease];
                        create_alert = nil;
                        uint64_t xuid =
                            app_context->CreateProfile(std::string([gamertag UTF8String]));
                        if (!xuid) {
                          UIAlertController* failure = [UIAlertController
                              alertControllerWithTitle:NSLocalizedString(
                                                           @"Profile Not Created",
                                                           @"Alert title: profile creation failed")
                                               message:
                                                   NSLocalizedString(
                                                       @"Profile could not be created. Please try "
                                                       @"again.",
                                                       @"Alert message: profile creation failed")
                                        preferredStyle:UIAlertControllerStyleAlert];
                          [failure
                              addAction:[UIAlertAction
                                            actionWithTitle:NSLocalizedString(
                                                                @"OK", @"Alert button: dismiss")
                                                      style:UIAlertActionStyleCancel
                                                    handler:nil]];
                          [self presentViewController:failure animated:YES completion:nil];
                          return;
                        }
                        BOOL signed_in = app_context->SignInProfile(xuid);
                        if (!signed_in) {
                          [self showStatusToast:NSLocalizedString(
                                                    @"Failed to sign in with the new profile.",
                                                    @"Toast: signing in to the new profile failed")
                                          style:XeniaIOSStatusToastStyleError];
                          return;
                        }
                        [self refreshSignedInProfileUI];
                        [self
                            showStatusToast:[NSString
                                                stringWithFormat:NSLocalizedString(
                                                                     @"Signed in as %@.",
                                                                     @"Toast; %@ is the gamertag"),
                                                                 gamertag]
                                      style:XeniaIOSStatusToastStyleSuccess];
                      }]];
  [self presentViewController:create_alert animated:YES completion:nil];
}

- (void)presentSettingsDestinationWithInitialSection:(XeniaSettingsInitialSection)initial_section
                         preventInteractiveDismissal:(BOOL)prevent_interactive_dismissal {
  if (!self.appContext) {
    return;
  }

  if (self.gameRunning) {
    gameplay_modal_presentation_pending_ = YES;
    [self updateTouchControlsOverlayVisibilityAnimated:YES];
  }

  __block XeniaViewController* unsafe_self = self;
  XeniaSettingsStatusHandler status_handler = ^(NSString* status_message) {
    [unsafe_self refreshSignedInProfileUI];
    if (status_message.length > 0) {
      [unsafe_self showStatusToastForMessage:status_message];
    }
  };
  void (^dismissal_handler)(void) = ^{
    gameplay_modal_presentation_pending_ = NO;
    [unsafe_self updateTouchControlsOverlayVisibilityAnimated:YES];
  };
  XeniaSettingsHubActionHandler action_handler = ^(XeniaSettingsHubAction action) {
    [unsafe_self handleSettingsHubAction:action];
  };

  const BOOL use_workspace =
      UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad &&
      self.traitCollection.horizontalSizeClass != UIUserInterfaceSizeClassCompact;

  UIViewController* destination = nil;
  if (use_workspace) {
    XeniaSettingsWorkspaceViewController* workspace =
        [[XeniaSettingsWorkspaceViewController alloc] initWithAppContext:self.appContext
                                                                onStatus:status_handler
                                                          initialSection:initial_section];
    workspace.dismissalHandler = dismissal_handler;
    workspace.actionHandler = action_handler;
    workspace.noGameRunning = !self.gameRunning;
    destination = workspace;
  } else {
    XeniaSettingsHubViewController* hub =
        [[XeniaSettingsHubViewController alloc] initWithAppContext:self.appContext
                                                          onStatus:status_handler
                                                    initialSection:initial_section];
    hub.showsCloseButton = YES;
    hub.dismissalHandler = dismissal_handler;
    hub.actionHandler = action_handler;
    hub.noGameRunning = !self.gameRunning;
    XeniaLandscapeNavigationController* nav =
        [[XeniaLandscapeNavigationController alloc] initWithRootViewController:hub];
    [hub release];
    destination = nav;
  }

  XEConfigureDestinationControllerPresentation(
      destination, self.view, use_workspace ? CGSizeMake(920.0, 760.0) : CGSizeMake(620.0, 760.0),
      prevent_interactive_dismissal);
  destination.presentationController.delegate = self;

  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:destination
                          animated:YES
                        completion:^{
                          gameplay_modal_presentation_pending_ = NO;
                          [self updateTouchControlsOverlayVisibilityAnimated:YES];
                        }];
  [destination release];
}

- (void)openProfileTapped:(UIButton*)sender {
  (void)sender;
  if (!self.appContext) {
    return;
  }

  if (self.gameRunning) {
    gameplay_modal_presentation_pending_ = YES;
    [self updateTouchControlsOverlayVisibilityAnimated:YES];
  }

  __block XeniaViewController* unsafe_self = self;
  IOSProfileStatusHandler status_handler = ^(NSString* status_message) {
    [unsafe_self refreshSignedInProfileUI];
    if (status_message.length > 0) {
      [unsafe_self showStatusToastForMessage:status_message];
    }
  };

  XeniaProfileViewController* profile =
      [[XeniaProfileViewController alloc] initWithAppContext:self.appContext
                                                    onStatus:status_handler];
  profile.showsDismissButton = YES;
  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:profile];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(520.0, 680.0),
                                     YES);
  navigation_controller.presentationController.delegate = self;

  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:navigation_controller
                          animated:YES
                        completion:^{
                          gameplay_modal_presentation_pending_ = NO;
                          [unsafe_self updateTouchControlsOverlayVisibilityAnimated:YES];
                        }];
  [navigation_controller release];
  [profile release];
}

- (void)dismissPresentedDestinationForSettingsAction:(void (^)(void))completion {
  void (^finish)(void) = ^{
    gameplay_modal_presentation_pending_ = NO;
    [self updateTouchControlsOverlayVisibilityAnimated:YES];
    if (completion) {
      completion();
    }
  };

  if (self.presentedViewController) {
    [self dismissViewControllerAnimated:YES completion:finish];
  } else {
    finish();
  }
}

- (void)handleSettingsHubAction:(XeniaSettingsHubAction)action {
  switch (action) {
    case XeniaSettingsHubActionOpenTouchLayoutLibrary:
      [self presentTouchLayoutLibrarySheet];
      break;
    case XeniaSettingsHubActionChooseGameTouchLayout: {
      __block XeniaViewController* unsafe_self = self;
      [self dismissPresentedDestinationForSettingsAction:^{
        [unsafe_self presentTouchLayoutGamePicker];
      }];
    } break;
    case XeniaSettingsHubActionEditTouchControls: {
      if (!self.gameRunning) {
        XEPresentOKAlert(
            [self topPresentedControllerForModalPresentation] ?: self,
            NSLocalizedString(@"Game Required", @"Alert title: no game running"),
            NSLocalizedString(@"Launch a game before editing the running touch layout.",
                              @"Alert message: touch layout editing needs a running game"));
        return;
      }
      __block XeniaViewController* unsafe_self = self;
      [self dismissPresentedDestinationForSettingsAction:^{
        [unsafe_self inGameEditControlsTapped:nil];
      }];
    } break;
    case XeniaSettingsHubActionImportTouchLayout:
      [self importTouchLayoutFromFile];
      break;
    case XeniaSettingsHubActionExportTouchLayout:
      [self exportCurrentTouchLayout];
      break;
    case XeniaSettingsHubActionResetTouchLayout:
      [self resetToOfficialTouchLayoutPreset];
      break;
    case XeniaSettingsHubActionImportGame:
      [self presentLibraryAddSheetFromView:nil];
      break;
    case XeniaSettingsHubActionRefreshLibrary:
      [self refreshImportedGamesAsync];
      break;
    case XeniaSettingsHubActionClearCurrentGameShaderCache:
      [self confirmClearCurrentGameShaderCache];
      break;
    case XeniaSettingsHubActionClearAllShaderCaches:
      [self confirmClearAllShaderCaches];
      break;
    case XeniaSettingsHubActionNone:
    default:
      break;
  }
}

- (void)confirmClearCurrentGameShaderCache {
  if (!active_game_title_id_) {
    XEPresentOKAlert(
        [self topPresentedControllerForModalPresentation] ?: self,
        NSLocalizedString(@"No Running Game", @"Alert title: no game running"),
        NSLocalizedString(@"Launch a game before clearing the current game's shader cache.",
                          @"Alert message: clearing the shader cache needs a running game"));
    return;
  }

  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(
                                   @"Clear Shader Cache?",
                                   @"Alert title: confirm clearing the current game's shader cache")
                       message:
                           NSLocalizedString(
                               @"Delete persistent shaders for the running title. Relaunch the "
                               @"game before testing.",
                               @"Alert message: confirm clearing the current game's shader cache")
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:nil]];
  __unsafe_unretained XeniaViewController* unsafe_self = self;
  [confirm addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(
                                             @"Clear",
                                             @"Alert button: confirm clearing the shader cache")
                                   style:UIAlertActionStyleDestructive
                                 handler:^(__unused UIAlertAction* action) {
                                   [unsafe_self clearCurrentGameShaderCache];
                                 }]];
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:confirm animated:YES completion:nil];
}

- (void)confirmClearAllShaderCaches {
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Clear All Shader Caches?",
                                                 @"Alert title: confirm clearing all shader caches")
                       message:NSLocalizedString(
                                   @"Delete persistent shader storage for every title. Games, "
                                   @"saves, content, and settings are not removed.",
                                   @"Alert message: confirm clearing all shader caches")
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:nil]];
  __unsafe_unretained XeniaViewController* unsafe_self = self;
  [confirm addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(
                                             @"Clear All",
                                             @"Alert button: confirm clearing all shader caches")
                                   style:UIAlertActionStyleDestructive
                                 handler:^(__unused UIAlertAction* action) {
                                   [unsafe_self clearAllShaderCaches];
                                 }]];
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:confirm animated:YES completion:nil];
}

- (void)presentShaderCacheResultWithTitle:(NSString*)title
                                  success:(BOOL)success
                                  removed:(uintmax_t)removed
                                    error:(const std::error_code&)error {
  NSString* message = nil;
  if (success) {
    message =
        removed == 0
            ? NSLocalizedString(@"No matching shader cache files were found.",
                                @"Result message: no shader cache files found")
            : [NSString
                  stringWithFormat:NSLocalizedString(
                                       @"Removed %llu cached item(s). Relaunch before testing.",
                                       @"Result message; %llu is the number of items removed"),
                                   static_cast<unsigned long long>(removed)];
  } else {
    message =
        [NSString stringWithFormat:NSLocalizedString(@"Failed to clear shader cache: %s",
                                                     @"Result message; %s is an error description"),
                                   error.message().c_str()];
  }
  XEPresentOKAlert([self topPresentedControllerForModalPresentation] ?: self, title, message);
  [self showStatusToast:message
                  style:(success ? XeniaIOSStatusToastStyleInfo : XeniaIOSStatusToastStyleError)];
}

- (void)clearCurrentGameShaderCache {
  uintmax_t removed = 0;
  std::error_code error;
  BOOL success =
      xe_clear_shader_cache_for_title(active_game_title_id_, &removed, &error) ? YES : NO;
  [self presentShaderCacheResultWithTitle:(success ? NSLocalizedString(
                                                         @"Shader Cache Cleared",
                                                         @"Alert title: shader cache clear result")
                                                   : NSLocalizedString(
                                                         @"Shader Cache Not Cleared",
                                                         @"Alert title: shader cache clear result"))
                                  success:success
                                  removed:removed
                                    error:error];
}

- (void)clearAllShaderCaches {
  uintmax_t removed = 0;
  std::error_code error;
  BOOL success = xe_clear_all_shader_caches(&removed, &error) ? YES : NO;
  [self
      presentShaderCacheResultWithTitle:(success
                                             ? NSLocalizedString(
                                                   @"Shader Caches Cleared",
                                                   @"Alert title: all shader caches clear result")
                                             : NSLocalizedString(
                                                   @"Shader Caches Not Cleared",
                                                   @"Alert title: all shader caches clear result"))
                                success:success
                                removed:removed
                                  error:error];
}

- (void)presentSystemSigninPromptForUserIndex:(uint32_t)user_index
                                  usersNeeded:(uint32_t)users_needed
                                   completion:(void (^)(BOOL success))completion {
  if (!self.appContext) {
    if (completion) {
      completion(NO);
    }
    return;
  }

  __block BOOL finished = NO;
  void (^finish)(BOOL) = ^(BOOL success) {
    if (finished) {
      return;
    }
    finished = YES;
    [self refreshSignedInProfileUI];
    if (completion) {
      completion(success);
    }
  };

  auto profiles = self.appContext->ListProfiles();
  void (^present_create_alert)(void) = ^{
    if (!self.appContext) {
      finish(NO);
      return;
    }

    // Under MRC, use `__block` to avoid a retain-cycle: alert -> action -> handler -> alert.
    __block UIAlertController* create_alert = [UIAlertController
        alertControllerWithTitle:NSLocalizedString(@"Create Profile",
                                                   @"Alert title: create a profile")
                         message:NSLocalizedString(@"Enter a gamertag (1-15 characters).",
                                                   @"Alert message: gamertag rules")
                  preferredStyle:UIAlertControllerStyleAlert];
    [create_alert addTextFieldWithConfigurationHandler:^(UITextField* text_field) {
      text_field.placeholder =
          NSLocalizedString(@"Gamertag", @"Placeholder of the gamertag text field");
      text_field.autocapitalizationType = UITextAutocapitalizationTypeWords;
      text_field.autocorrectionType = UITextAutocorrectionTypeNo;
      text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [create_alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(
                                                               @"Cancel", @"Alert button: cancel")
                                                     style:UIAlertActionStyleCancel
                                                   handler:^(__unused UIAlertAction* action) {
                                                     finish(NO);
                                                   }]];
    [create_alert
        addAction:
            [UIAlertAction
                actionWithTitle:NSLocalizedString(@"Create", @"Alert button: create the profile")
                          style:UIAlertActionStyleDefault
                        handler:^(__unused UIAlertAction* action) {
                          UITextField* text_field = create_alert.textFields.firstObject;
                          NSString* raw_text = text_field.text ?: @"";
                          NSString* trimmed =
                              [raw_text stringByTrimmingCharactersInSet:
                                            [NSCharacterSet whitespaceAndNewlineCharacterSet]];
                          if (trimmed.length == 0 || !self.appContext) {
                            finish(NO);
                            return;
                          }
                          auto* app_context = self.appContext;
                          if (!app_context) {
                            finish(NO);
                            return;
                          }
                          NSString* gamertag = [[trimmed copy] autorelease];
                          create_alert = nil;
                          uint64_t xuid =
                              app_context->CreateProfile(std::string([gamertag UTF8String]));
                          if (!xuid) {
                            [self showStatusToast:
                                      NSLocalizedString(
                                          @"Profile could not be created. Please try again.",
                                          @"Toast: profile creation failed")
                                            style:XeniaIOSStatusToastStyleError];
                            finish(NO);
                            return;
                          }
                          BOOL signed_in = app_context->SignInProfile(xuid);
                          if (signed_in) {
                            [self showStatusToast:
                                      [NSString stringWithFormat:NSLocalizedString(
                                                                     @"Signed in as %@.",
                                                                     @"Toast; %@ is the gamertag"),
                                                                 gamertag]
                                            style:XeniaIOSStatusToastStyleSuccess];
                          } else {
                            [self
                                showStatusToast:NSLocalizedString(
                                                    @"Failed to sign in with the new profile.",
                                                    @"Toast: signing in to the new profile failed")
                                          style:XeniaIOSStatusToastStyleError];
                          }
                          finish(signed_in);
                        }]];

    XeniaPresentGuestPrompt(self, create_alert, ^{
      finish(NO);
    });
  };

  if (profiles.empty()) {
    present_create_alert();
    return;
  }

  NSString* message =
      users_needed == 1
          ? NSLocalizedString(@"Select profile (needs 1 user).",
                              @"Profile picker message when exactly one user is needed")
          : [NSString stringWithFormat:NSLocalizedString(@"Select profile (needs %u users).",
                                                         @"Profile picker message; %u is the "
                                                         @"number of users needed (2 or more)"),
                                       users_needed];
  UIAlertController* sheet = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Select Profile",
                                                 @"Action sheet title: choose a profile")
                       message:message
                preferredStyle:UIAlertControllerStyleActionSheet];

  [sheet addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Create Profile",
                                                         @"Action sheet button: create a profile")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 present_create_alert();
                               }]];

  for (const auto& profile : profiles) {
    NSString* gamertag = ToNSString(profile.gamertag);
    NSString* title = gamertag;
    if (profile.signed_in) {
      title =
          [NSString stringWithFormat:NSLocalizedString(@"%@ (Signed In)",
                                                       @"Profile picker row; %@ is the gamertag"),
                                     title];
    }
    uint64_t xuid = profile.xuid;
    [sheet addAction:[UIAlertAction actionWithTitle:title
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction* action) {
                                              if (!self.appContext) {
                                                finish(NO);
                                                return;
                                              }
                                              BOOL signed_in = self.appContext->SignInProfile(xuid);
                                              finish(signed_in);
                                            }]];
  }

  [sheet addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button: cancel")
                                 style:UIAlertActionStyleCancel
                               handler:^(__unused UIAlertAction* action) {
                                 finish(NO);
                               }]];

  UIPopoverPresentationController* popover = sheet.popoverPresentationController;
  if (popover) {
    popover.sourceView = self.view;
    popover.sourceRect =
        CGRectMake(CGRectGetMidX(self.view.bounds), CGRectGetMidY(self.view.bounds), 1.0, 1.0);
    popover.permittedArrowDirections = 0;
  }

  XeniaPresentGuestPrompt(self, sheet, ^{
    finish(NO);
  });
}

- (void)presentAchievementsForUserIndex:(uint32_t)user_index
                                titleID:(uint32_t)title_id
                             completion:(void (^)(BOOL success))completion {
  if (!self.appContext) {
    if (completion) {
      completion(NO);
    }
    return;
  }

  XeniaAchievementsViewController* achievements_vc =
      [[XeniaAchievementsViewController alloc] initWithAppContext:self.appContext
                                                        userIndex:user_index
                                                          titleID:title_id];
  __block XeniaViewController* unsafe_self = self;
  // A guest showing the achievements UI waits until the player closes it. Answer it once: when
  // the sheet closes, or as failed if UIKit never shows it.
  __block BOOL answered = NO;
  void (^answer)(BOOL) = [[^(BOOL closed) {
    if (!answered) {
      answered = YES;
      if (completion) {
        completion(closed);
      }
    }
  } copy] autorelease];
  achievements_vc.dismissalHandler = ^{
    [unsafe_self updateTouchControlsOverlayVisibilityAnimated:YES];
    answer(YES);
  };
  UINavigationController* nav = [self presentPauseSheetWithRootController:achievements_vc
                                                            preferredSize:CGSizeMake(560.0, 620.0)
                                              preventInteractiveDismissal:NO
                                                               completion:nil];
  [achievements_vc release];
  [nav retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(1.5 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (!nav.presentingViewController) {
                     answer(NO);
                   }
                   [nav release];
                 });
}

- (void)presentAchievementNotification:(const xe::ui::AchievementNotificationPayload&)payload {
  [self.achievementNotificationPresenter presentPayload:payload inView:self.view];
}

- (void)presentSystemKeyboardPromptWithTitle:(NSString*)title
                                 description:(NSString*)description
                                 defaultText:(NSString*)default_text
                                  completion:(void (^)(BOOL cancelled, NSString* text))completion {
  [self presentSystemKeyboardPromptWithTitle:title
                                 description:description
                                 defaultText:default_text
                                   maxLength:0
                                  completion:completion];
}

- (void)presentSystemKeyboardPromptWithTitle:(NSString*)title
                                 description:(NSString*)description
                                 defaultText:(NSString*)default_text
                                   maxLength:(NSUInteger)max_length
                                  completion:(void (^)(BOOL cancelled, NSString* text))completion {
  __block UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:title.length ? title
                                            : NSLocalizedString(
                                                  @"Input Required",
                                                  @"Fallback alert title for a guest text prompt")
                       message:description
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* text_field) {
    text_field.text = default_text ?: @"";
    text_field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    text_field.autocorrectionType = UITextAutocorrectionTypeNo;
    text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
    if (max_length) {
      XeniaLimitTextFieldLength(text_field, max_length);
    }
  }];

  [alert
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:^(__unused UIAlertAction* action) {
                                         if (completion) {
                                           completion(YES, @"");
                                         }
                                       }]];
  UIAlertAction* ok_action =
      [UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button: confirm")
                               style:UIAlertActionStyleDefault
                             handler:^(__unused UIAlertAction* action) {
                               UITextField* text_field = alert.textFields.firstObject;
                               NSString* text = text_field.text ?: @"";
                               if (completion) {
                                 completion(NO, text);
                               }
                             }];
  [alert addAction:ok_action];
  // So Return submits.
  alert.preferredAction = ok_action;

  XeniaPresentGuestPrompt(self, alert, ^{
    if (completion) {
      completion(YES, @"");
    }
  });
}

- (void)updateJITStatusIndicator {
  [self.launcherOverlayView setJITAcquired:self.jitAcquired];
  if (self.jitAcquired) {
    [self.launcherOverlayView
        setJITStatusText:NSLocalizedString(@"JIT Enabled", @"Launcher status: JIT is enabled")];
  } else if (self.jitPollTimedOut) {
    [self.launcherOverlayView
        setJITStatusText:NSLocalizedString(
                             @"JIT unavailable. Open StikDebug, then restart polling.",
                             @"Launcher status: JIT is unavailable")];
  } else {
    [self.launcherOverlayView setJITStatusText:xe_jit_waiting_status_message()];
  }
}

- (void)updateJITAvailabilityUI {
  [self.launcherOverlayView setJITAcquired:self.jitAcquired];
}

- (void)updateMemoryEntitlementStatusUI {
  BOOL enabled = xe_has_increased_memory_limit_entitlement();
  BOOL previous_enabled = self.memoryEntitlementEnabled;
  self.memoryEntitlementEnabled = enabled;
  [self.launcherOverlayView
      setMemoryEntitlementStatusText:enabled ? @""
                                             : xe_memory_entitlement_missing_status_message()];
  [self.launcherOverlayView setMemoryEntitlementEnabled:enabled];
  if (previous_enabled != enabled) {
    XELOGI("iOS signing: runtime increased-memory-limit entitlement {}", enabled);
  }
}

- (std::filesystem::path)importedGamesDirectory {
  return IOSImportedGamesDirectory();
}

- (std::filesystem::path)importGameIntoLibrary:(NSURL*)source_url error:(NSError**)error {
  return ImportGameIntoIOSLibrary(source_url, error);
}

- (void)refreshImportedGamesAsync {
  [self refreshImportedGamesAsyncWithCompletion:nil];
}

- (void)refreshImportedGamesAsyncWithCompletion:(void (^)(void))completion {
  void (^completion_copy)(void) = [completion copy];
  [self refreshImportedGamesAsyncWithScannedGamesCompletion:^(
            const std::vector<IOSDiscoveredGame>& games) {
    (void)games;
    if (completion_copy) {
      completion_copy();
      [completion_copy release];
    }
  }];
}

- (void)refreshImportedGamesAsyncWithScannedGamesCompletion:
    (void (^)(const std::vector<IOSDiscoveredGame>& games))completion {
  const uint64_t refresh_generation = ++library_refresh_generation_;
  NSString* caches_dir =
      NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
  NSString* names_path = [caches_dir stringByAppendingPathComponent:@"title-names.plist"];
  // Parsed on the scan queue below, not here: the plist read must stay off the main thread.
  NSString* names_path_copy = [names_path copy];
  void (^completion_copy)(const std::vector<IOSDiscoveredGame>&) = [completion copy];

  __unsafe_unretained XeniaViewController* unsafe_self = self;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSDictionary* title_name_cache =
        [[NSDictionary dictionaryWithContentsOfFile:names_path_copy] retain];
    [names_path_copy release];
    auto* scanned_games = new std::vector<IOSDiscoveredGame>(ScanIOSGameLibrary(title_name_cache));
    [title_name_cache release];
    dispatch_async(dispatch_get_main_queue(), ^{
      std::unique_ptr<std::vector<IOSDiscoveredGame>> scanned_games_owner(scanned_games);
      if (refresh_generation == unsafe_self->library_refresh_generation_) {
        unsafe_self->discovered_games_ = std::move(*scanned_games_owner);
        [unsafe_self finishImportedGamesRefresh];
        if (completion_copy) {
          completion_copy(unsafe_self->discovered_games_);
        }
      } else if (completion_copy) {
        completion_copy(*scanned_games_owner);
      }
      // Run the completion even when a newer refresh superseded this scan:
      // callers (e.g. the ZAR conversion flow presenting its results sheet)
      // rely on it for flow continuation, not for library consistency.
      [completion_copy release];
    });
  });
}

- (void)finishImportedGamesRefresh {
  [self applyCompatDataToDiscoveredGames];
  SortDiscoveredGames(&discovered_games_);
  // The dashboard lists the library as its games.
  xe::ui::PublishIOSLibraryToDashboard(discovered_games_);

  NSInteger focused_game_index = self.controllerNavigationCoordinator.focusedGameIndex;
  if (discovered_games_.empty()) {
    focused_game_index = -1;
  } else if (focused_game_index < 0 ||
             focused_game_index >=
                 static_cast<NSInteger>(discovered_games_.size()) + [self launcherGameItemOffset]) {
    focused_game_index = 0;
  }
  [self.controllerNavigationCoordinator setFocusedGameIndex:focused_game_index scroll:NO];
  [self refreshLauncherGameSnapshots];
  [self.controllerNavigationCoordinator refreshLauncherFocus];
}

- (void)applyCompatDataToDiscoveredGames {
  ApplyIOSCompatibilityDataToDiscoveredGames(compat_data_, &discovered_games_);
}

- (void)presentJITRequiredAlertForLaunchPath:(const std::filesystem::path&)gamePath
                                 displayName:(NSString*)displayName {
  NSString* launch_path_string = gamePath.empty() ? nil : ToNSString(gamePath.string());
  NSString* message = xe_jit_not_detected_guidance_message();
  if (displayName.length > 0) {
    message =
        [NSString stringWithFormat:
                      NSLocalizedString(
                          @"%1$@\n\nStikDebug can enable JIT and return to XeniOS to launch %2$@.",
                          @"Alert message; %1$@ is the guidance text, %2$@ the game title"),
                      message, displayName];
  }

  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"JIT Not Detected",
                                                 @"Alert title: JIT was not detected")
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [alert
      addAction:[UIAlertAction
                    actionWithTitle:NSLocalizedString(@"Enable via StikDebug",
                                                      @"Alert button: enable JIT through StikDebug")
                              style:UIAlertActionStyleDefault
                            handler:^(__unused UIAlertAction* action) {
                              std::filesystem::path launch_path;
                              if (launch_path_string.length > 0) {
                                launch_path = std::filesystem::path(launch_path_string.UTF8String);
                              }
                              const std::filesystem::path* launch_path_ptr =
                                  launch_path.empty() ? nullptr : &launch_path;
                              if ([self
                                      requestStikDebugJITHandoffForPendingLaunchPath:launch_path_ptr
                                                                   requirePreference:NO
                                                        promptForAutomationOnSuccess:YES]) {
                                self->pending_external_launch_path_ = launch_path;
                              }
                            }]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Open Settings",
                                                         @"Alert button: opens the Settings app")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 [self openSettingsTapped:nil];
                               }]];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button: dismiss")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)presentMemoryEntitlementWarningForLaunchPath:(const std::filesystem::path&)gamePath
                                         displayName:(NSString*)displayName {
  NSString* launch_path_string = gamePath.empty() ? nil : ToNSString(gamePath.string());
  NSString* message = xe_memory_entitlement_not_detected_guidance_message();
  if (displayName.length > 0) {
    message = [NSString
        stringWithFormat:NSLocalizedString(
                             @"%1$@\n\nYou can still try launching %2$@, but it may crash.",
                             @"Alert message; %1$@ is the guidance text, %2$@ the game title"),
                         message, displayName];
  }

  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(
                                   @"Memory Entitlement Missing",
                                   @"Alert title: the increased memory entitlement is missing")
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(
                                                      @"Launch Anyway",
                                                      @"Alert button: launch despite the warning")
                                            style:UIAlertActionStyleDefault
                                          handler:^(__unused UIAlertAction* action) {
                                            std::filesystem::path launch_path;
                                            if (launch_path_string.length > 0) {
                                              launch_path = std::filesystem::path(
                                                  launch_path_string.UTF8String);
                                            }
                                            [self launchGameAtPath:launch_path
                                                                 displayName:displayName
                                                skipMemoryEntitlementWarning:YES];
                                          }]];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button: dismiss")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (NSString*)displayNameForGamePath:(const std::filesystem::path&)game_path {
  for (const IOSDiscoveredGame& game : discovered_games_) {
    if (game.path == game_path) {
      return ToNSString(game.title);
    }
    for (const auto& disc : game.discs) {
      if (disc.path == game_path) {
        return ToNSString(game.title);
      }
    }
  }
  return nil;
}

- (BOOL)findDiscoveredGameWithTitleID:(uint32_t)title_id
                               system:(const xe::ui::IOSGameSystem*)system_filter
                                 path:(std::filesystem::path*)path_out
                          displayName:(NSString**)display_name_out {
  if (!title_id || !path_out) {
    return NO;
  }

  auto find_match = [&]() -> const IOSDiscoveredGame* {
    for (const IOSDiscoveredGame& game : discovered_games_) {
      if (game.title_id == title_id && (!system_filter || game.system == *system_filter)) {
        return &game;
      }
    }
    return nullptr;
  };

  const IOSDiscoveredGame* match = find_match();
  if (!match) {
    return NO;
  }

  *path_out = match->path;
  if (display_name_out) {
    *display_name_out = ToNSString(match->title);
  }
  return YES;
}

- (BOOL)requestStikDebugJITHandoffForPendingLaunchPath:(const std::filesystem::path*)launch_path
                                     requirePreference:(BOOL)require_preference
                          promptForAutomationOnSuccess:(BOOL)prompt_for_automation {
  if (require_preference &&
      !GetUserDefaultBool(kXeniaAutoOpenStikDebugOnLaunchPreferenceKey, false)) {
    XELOGI("iOS: Automatic StikDebug handoff skipped (disabled)");
    return NO;
  }
  if (self.gameRunning || self.gameStopInProgress) {
    XELOGI("iOS: Automatic StikDebug handoff skipped (game already running)");
    return NO;
  }
  // Don't bounce through StikDebug while a touch layout install confirm is
  // pending. Layouts don't need JIT, and the app-switch round-trip kills the
  // modal before the user can act on it. The handoff gets re-evaluated once
  // the install confirm is dismissed.
  if ([self.touchLayoutCoordinator hasPendingInstall]) {
    XELOGI("iOS: Automatic StikDebug handoff deferred (touch layout install pending)");
    return NO;
  }
  if (self.jitAcquired || xe_check_jit_available()) {
    if (!self.jitAcquired) {
      [self onJITAcquired];
    }
    XELOGI("iOS: Automatic StikDebug handoff skipped (JIT already available)");
    return NO;
  }

  const double now = GetUnixTimeSeconds();
  const double last_attempt =
      GetUserDefaultDouble(kXeniaLastAutoStikDebugAttemptTimestampPreferenceKey, 0.0);
  if (require_preference && last_attempt > 0.0 &&
      (now - last_attempt) < kXeniaAutoStikDebugCooldownSeconds) {
    XELOGI("iOS: Skipping automatic StikDebug handoff (cooldown active)");
    return NO;
  }

  NSString* bundle_identifier = NSBundle.mainBundle.bundleIdentifier;
  NSURL* stikdebug_url = xe_stikdebug_enable_jit_url_for_bundle_identifier(bundle_identifier);
  if (!stikdebug_url) {
    XELOGW("iOS: Unable to build StikDebug JIT handoff URL");
    return NO;
  }

  UIApplication* application = [UIApplication sharedApplication];
  if (![application canOpenURL:stikdebug_url]) {
    XELOGW("iOS: StikDebug URL scheme unavailable");
    [self presentLaunchProblemAlertWithMessage:NSLocalizedString(
        @"StikDebug is not installed or unavailable.",
        @"Alert message: StikDebug is unavailable")];
    if (launch_path && !launch_path->empty()) {
      ClearPendingExternalLaunchPathPreference();
    }
    return NO;
  }

  if (launch_path && !launch_path->empty()) {
    StorePendingExternalLaunchPathPreference(*launch_path);
  }
  if (prompt_for_automation) {
    SetUserDefaultBool(kXeniaPendingStikDebugAutomationPromptPreferenceKey, true);
  }

  const BOOL has_pending_launch = launch_path && !launch_path->empty();
  SetUserDefaultDouble(kXeniaLastAutoStikDebugAttemptTimestampPreferenceKey, now);
  [self showStatusToast:has_pending_launch
                            ? NSLocalizedString(@"Opening StikDebug to enable JIT...",
                                                @"Toast: opening StikDebug before a launch")
                            : NSLocalizedString(@"Opening StikDebug for JIT...",
                                                @"Toast: opening StikDebug")
                  style:XeniaIOSStatusToastStyleInfo];
  XELOGI("iOS: Opening StikDebug handoff URL {}", stikdebug_url.absoluteString.UTF8String);

  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.15 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   [application openURL:stikdebug_url
                       options:@{}
                       completionHandler:^(BOOL success) {
                         if (!success) {
                           XELOGW("iOS: Failed to open StikDebug handoff URL");
                           [self presentLaunchProblemAlertWithMessage:NSLocalizedString(
                               @"Failed to open StikDebug.",
                               @"Alert message: could not open StikDebug")];
                           if (has_pending_launch) {
                             ClearPendingExternalLaunchPathPreference();
                           }
                           if (prompt_for_automation) {
                             SetUserDefaultBool(kXeniaPendingStikDebugAutomationPromptPreferenceKey,
                                                false);
                           }
                         }
                       }];
                 });
  return YES;
}

- (BOOL)requestAutomaticStikDebugJITHandoffForPendingLaunchPath:
    (const std::filesystem::path*)launch_path {
  return [self requestStikDebugJITHandoffForPendingLaunchPath:launch_path
                                            requirePreference:YES
                                 promptForAutomationOnSuccess:NO];
}

- (void)evaluateAutomaticStikDebugJITHandoffIfNeeded {
  [self requestAutomaticStikDebugJITHandoffForPendingLaunchPath:nullptr];
}

- (void)copyLaunchURLForGameAtIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }
  const IOSDiscoveredGame& game = discovered_games_[game_index];
  if (!game.title_id) {
    [self presentLaunchProblemAlertWithMessage:NSLocalizedString(
        @"Launch URL unavailable for this game.",
        @"Alert message: no launch URL exists for the game")];
    return;
  }

  NSString* launch_url = xe_launch_url_for_title_id(game.title_id, game.system);
  if (!launch_url || launch_url.length == 0) {
    [self presentLaunchProblemAlertWithMessage:NSLocalizedString(
        @"Failed to build launch URL.",
        @"Alert message: building the launch URL failed")];
    return;
  }

  [UIPasteboard generalPasteboard].string = launch_url;
  NSString* game_title = ToNSString(game.title);
  NSString* message =
      game_title.length > 0
          ? [NSString stringWithFormat:NSLocalizedString(@"Copied launch URL for %@.",
                                                         @"Toast; %@ is the game title"),
                                       game_title]
          : NSLocalizedString(@"Copied launch URL.", @"Toast: launch URL copied");
  [self showStatusToast:message style:XeniaIOSStatusToastStyleSuccess];
  XELOGI("iOS: Copied title-ID launch URL {}", [launch_url UTF8String]);
}

- (BOOL)respondToExternalGameInfoRequestURL:(NSURL*)url {
  NSString* callback_scheme = nil;
  if (!IsExternalGameInfoRequestURL(url, &callback_scheme)) {
    return NO;
  }

  if (!callback_scheme || callback_scheme.length == 0) {
    XELOGW("iOS: gameInfo request is missing callback scheme");
    [self presentLinkCouldNotBeOpenedAlert];
    return YES;
  }

  NSString* request_scheme = NormalizeURLToken(url.scheme);
  if (request_scheme && [callback_scheme caseInsensitiveCompare:request_scheme] == NSOrderedSame) {
    XELOGW("iOS: gameInfo callback scheme {} would loop back into XeniOS",
           [callback_scheme UTF8String]);
    [self presentLinkCouldNotBeOpenedAlert];
    return YES;
  }

  // Any app can send this URL: the library's titles and icons leave only after the
  // user agrees.
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(
                                   @"Share Game Library?",
                                   @"Alert title: another app asks for the game library")
                       message:[NSString
                                   stringWithFormat:
                                       NSLocalizedString(
                                           @"The app \"%@\" is asking for the titles and icons of "
                                           @"your game library.",
                                           @"Alert message; %@ is the name of the requesting app"),
                                       callback_scheme]
                preferredStyle:UIAlertControllerStyleAlert];
  [alert
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:^(__unused UIAlertAction* action) {
                                         [self clearURLConfirmation];
                                       }]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Share",
                                                         @"Alert button: share the game library")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 [self clearURLConfirmation];
                                 [self sendGameInfoToCallbackScheme:callback_scheme requestURL:url];
                               }]];
  [self presentURLConfirmation:alert];
  return YES;
}

- (void)sendGameInfoToCallbackScheme:(NSString*)callback_scheme requestURL:(NSURL*)url {
  // discovered_games_ is kept current by the async refreshes (launch, foreground,
  // after every import); a synchronous rescan here would block the main thread.
  NSMutableArray* exported_games = [NSMutableArray array];
  size_t skipped_without_title_id = 0;
  for (const IOSDiscoveredGame& game : discovered_games_) {
    if (!game.title_id) {
      ++skipped_without_title_id;
      continue;
    }

    NSString* title_id = ToNSString(FormatTitleID(game.title_id));
    NSString* title_name =
        game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
    NSString* icon_base64 = @"";
    if (!game.icon_data.empty()) {
      NSData* icon_data = [NSData dataWithBytes:game.icon_data.data() length:game.icon_data.size()];
      if (icon_data.length > 0) {
        icon_base64 = [icon_data base64EncodedStringWithOptions:0] ?: @"";
      }
    }

    NSDictionary* entry = @{
      @"titleName" : title_name ?: @"",
      @"version" : @"",
      @"iconData" : icon_base64 ?: @"",
      @"titleId" : title_id ?: @"",
      @"id" : title_id ?: @"",
      @"developer" : @"",
    };
    [exported_games addObject:entry];
  }

  NSError* json_error = nil;
  NSData* json_data = [NSJSONSerialization dataWithJSONObject:exported_games
                                                      options:0
                                                        error:&json_error];
  if (!json_data || json_error) {
    XELOGE("iOS: Failed serializing gameInfo payload: {}",
           json_error.localizedDescription.UTF8String);
    [self presentLinkCouldNotBeOpenedAlert];
    return;
  }

  NSString* games_payload = [json_data base64EncodedStringWithOptions:0];
  if (!games_payload || games_payload.length == 0) {
    XELOGW("iOS: gameInfo payload encoding produced empty data");
    [self presentLinkCouldNotBeOpenedAlert];
    return;
  }

  NSURLComponents* callback_components = [[[NSURLComponents alloc] init] autorelease];
  callback_components.scheme = callback_scheme;
  callback_components.host = xe_game_info_callback_provider(url);
  callback_components.queryItems = @[ [NSURLQueryItem queryItemWithName:@"games"
                                                                  value:games_payload] ];
  NSURL* callback_url = callback_components.URL;
  if (!callback_url) {
    XELOGE("iOS: Failed building gameInfo callback URL for scheme {}",
           [callback_scheme UTF8String]);
    [self presentLinkCouldNotBeOpenedAlert];
    return;
  }

  const NSUInteger exported_count = exported_games.count;
  NSString* callback_app = callback_scheme;
  XELOGI(
      "iOS: Returning {} games via {}:// (skipped {} without title IDs, payload {} bytes, URL "
      "chars {})",
      static_cast<uint32_t>(exported_count), [callback_scheme UTF8String],
      static_cast<uint32_t>(skipped_without_title_id), static_cast<uint32_t>(json_data.length),
      static_cast<uint32_t>(callback_url.absoluteString.length));

  [[UIApplication sharedApplication]
                openURL:callback_url
                options:@{}
      completionHandler:^(BOOL success) {
        if (success) {
          [self showStatusToast:[NSString stringWithFormat:NSLocalizedString(
                                                               @"Sent %1$lu games to %2$@.",
                                                               @"Toast; %1$lu is the number of "
                                                               @"games, %2$@ the receiving app"),
                                                           (unsigned long)exported_count,
                                                           callback_app]
                          style:XeniaIOSStatusToastStyleSuccess];
          return;
        }
        XELOGW("iOS: Failed opening gameInfo callback URL {}://", [callback_app UTF8String]);
        [self presentLaunchProblemAlertWithMessage:[NSString
            stringWithFormat:NSLocalizedString(@"Failed to return library to %@.",
                                               @"Toast; %@ is the name of the app"),
            callback_app]];
      }];
}

- (BOOL)handleExternalLaunchURL:(NSURL*)url {
  if ([self respondToExternalGameInfoRequestURL:url]) {
    return YES;
  }

  // Touch-layout file URLs and xenios://touchlayout?... drops share the
  // launch URL surface, so handle them before the game-launch branches.
  if ([self handleExternalTouchLayoutFileURL:url]) {
    return YES;
  }
  if ([self handleExternalTouchLayoutSchemeURL:url]) {
    return YES;
  }

  // A System Link invite launches its game by title ID. Its room is set only once the
  // player agrees to the launch.
  uint32_t invite_title_id = 0;
  NSString* invite_room = xe_ios_system_link_invite_room(url, &invite_title_id);
  NSURL* launch_url =
      invite_room ? [NSURL URLWithString:xe_launch_url_for_title_id(
                                             invite_title_id, xe::ui::IOSGameSystem::kXbox360)]
                  : url;

  std::filesystem::path launch_path;
  NSString* display_name = nil;
  uint32_t title_id = 0;
  xe::ui::IOSGameSystem title_system = xe::ui::IOSGameSystem::kXbox360;
  bool title_system_present = false;
  if (ExtractLaunchPathFromExternalURL(launch_url, &launch_path) && !launch_path.empty()) {
    display_name = [self displayNameForGamePath:launch_path];
    if (!display_name || display_name.length == 0) {
      display_name = ToNSString(launch_path.filename().string());
    }
  } else if (ExtractLaunchTitleIDFromExternalURL(launch_url, &title_id, &title_system,
                                                 &title_system_present) &&
             title_id) {
    if (![self findDiscoveredGameWithTitleID:title_id
                                      system:title_system_present ? &title_system : nullptr
                                        path:&launch_path
                                 displayName:&display_name]) {
      // The library may still show the cached list from the last launch: rescan once
      // in the background, then look again.
      if (!external_title_launch_rescanned_) {
        external_title_launch_rescanned_ = YES;
        [self refreshImportedGamesAsyncWithCompletion:^{
          [self handleExternalLaunchURL:url];
          external_title_launch_rescanned_ = NO;
        }];
        return YES;
      }
      XELOGW("iOS: External launch title ID {:08X} was not found in Library", title_id);
      [self presentLaunchProblemAlertWithMessage:[NSString
          stringWithFormat:NSLocalizedString(@"Title ID %08X was not found in Library.",
                                             @"Alert message; %08X is the title ID in hexadecimal"),
          title_id]];
      return NO;
    }
  }

  if (launch_path.empty()) {
    XELOGW("iOS: External launch URL missing valid game target (scheme={} host={})",
           url.scheme ? [url.scheme UTF8String] : "", url.host ? [url.host UTF8String] : "");
    [self presentLinkCouldNotBeOpenedAlert];
    return NO;
  }

  if (!display_name || display_name.length == 0) {
    display_name = ToNSString(launch_path.filename().string());
  }

  if (title_id) {
    XELOGI("iOS: External game launch requested by title ID {:08X}: {}", title_id,
           xe::strip_pii(launch_path.string()));
  } else {
    XELOGI("iOS: External game launch requested: {}", xe::strip_pii(launch_path.string()));
  }

  // Any app or web page can open xenios:// URLs, so a link never starts or replaces a
  // game without asking.
  const BOOL replaces_game = self.gameRunning || self.gameStopInProgress;
  NSString* running_name = replaces_game ? [self runningGameDisplayName] : nil;
  NSString* message = nil;
  if (!replaces_game) {
    message = [NSString stringWithFormat:NSLocalizedString(@"A link wants to launch \"%@\".",
                                                           @"Alert message; %@ is the game title"),
                                         display_name];
  } else if (running_name.length) {
    message = [NSString
        stringWithFormat:NSLocalizedString(
                             @"A link wants to launch \"%1$@\". This will stop \"%2$@\", and unsaved "
                             @"progress will be lost.",
                             @"Alert message; %1$@ is the game to launch, %2$@ the running game"),
                         display_name, running_name];
  } else {
    message = [NSString
        stringWithFormat:NSLocalizedString(@"A link wants to launch \"%@\". This will stop the "
                                           @"running game, and unsaved progress will be lost.",
                                           @"Alert message; %@ is the game to launch"),
                         display_name];
  }
  if (invite_room) {
    const BOOL restarts = replaces_game && active_game_title_id_ == invite_title_id;
    message = [message
        stringByAppendingFormat:@" %@", xe_ios_system_link_invite_note(invite_room, restarts)];
  }
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:replaces_game
                                   ? NSLocalizedString(
                                         @"Replace Running Game?",
                                         @"Alert title: a link wants to replace the running game")
                                   : NSLocalizedString(
                                         @"Launch Game?",
                                         @"Alert title: a link wants to launch a game")
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [alert
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:^(__unused UIAlertAction* action) {
                                         [self clearURLConfirmation];
                                       }]];
  const std::filesystem::path confirmed_path = launch_path;
  [alert
      addAction:[UIAlertAction
                    actionWithTitle:replaces_game
                                        ? NSLocalizedString(
                                              @"Replace", @"Alert button: replace the running game")
                                        : NSLocalizedString(
                                              @"Launch", @"Alert button: replace the running game")
                              style:replaces_game ? UIAlertActionStyleDestructive
                                                  : UIAlertActionStyleDefault
                            handler:^(__unused UIAlertAction* action) {
                              [self clearURLConfirmation];
                              if (invite_room) {
                                xe_ios_join_system_link_room(invite_title_id, invite_room);
                              }
                              [self proceedWithExternalLaunchPath:confirmed_path
                                                      displayName:display_name];
                            }]];
  [self presentURLConfirmation:alert];
  return YES;
}

- (void)proceedWithExternalLaunchPath:(const std::filesystem::path&)launch_path
                          displayName:(NSString*)display_name {
  if (!self.jitAcquired) {
    pending_external_launch_path_ = launch_path;
    if (![self requestAutomaticStikDebugJITHandoffForPendingLaunchPath:&launch_path]) {
      [self showStatusToast:[NSString
                                stringWithFormat:NSLocalizedString(@"Waiting for JIT to launch: %@",
                                                                   @"Toast; %@ is the game title"),
                                                 display_name]
                      style:XeniaIOSStatusToastStyleInfo];
    }
    return;
  }

  [self launchGameAtPath:launch_path displayName:display_name];
}

// Link-driven prompts (launch, gameInfo) show one at a time: a newer link replaces
// the older prompt instead of stacking on it.
- (void)presentURLConfirmation:(UIAlertController*)alert {
  UIAlertController* older = pending_url_confirmation_;
  pending_url_confirmation_ = [alert retain];
  // An older prompt's queued present must not show once a newer one has replaced it.
  void (^present)(void) = ^{
    if (pending_url_confirmation_ != alert) {
      return;
    }
    UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
    [presenter presentViewController:alert animated:YES completion:nil];
  };
  if (older.presentingViewController && !older.isBeingDismissed) {
    [older.presentingViewController dismissViewControllerAnimated:NO completion:present];
  } else {
    present();
  }
  [older release];
}

- (void)clearURLConfirmation {
  [pending_url_confirmation_ release];
  pending_url_confirmation_ = nil;
}

- (NSString*)runningGameDisplayName {
  if (!active_game_title_id_) {
    return nil;
  }
  for (const IOSDiscoveredGame& game : discovered_games_) {
    if (game.title_id == active_game_title_id_ && !game.title.empty()) {
      return ToNSString(game.title);
    }
  }
  return nil;
}

// A file opened from Files or AirDrop. Not a launch: route it through the import flow.
- (BOOL)handleOpenedFileURL:(NSURL*)url {
  if ([self handleExternalTouchLayoutFileURL:url]) {
    return YES;
  }
  const BOOL in_place =
      !xe::ui::IsPathInIOSDocumentsInbox(std::filesystem::path([url.path UTF8String]));
  [self.documentImportCoordinator handleOpenedDocumentURL:url openInPlace:in_place];
  return YES;
}

- (void)launchGameAtPath:(const std::filesystem::path&)game_path
             displayName:(NSString*)display_name {
  [self launchGameAtPath:game_path displayName:display_name skipMemoryEntitlementWarning:NO];
}

- (void)launchGameAtPath:(const std::filesystem::path&)game_path
                     displayName:(NSString*)display_name
    skipMemoryEntitlementWarning:(BOOL)skipMemoryEntitlementWarning {
  std::filesystem::path launch_path = game_path;
  NSError* external_access_error = nil;
  BOOL matched_external_location = NO;
  XeniaIOSExternalLibraryAccess* external_access = xe::ui::StartIOSExternalLibraryAccessForPath(
      launch_path, &matched_external_location, &external_access_error);
  if (matched_external_location && !external_access) {
    NSString* message =
        external_access_error.localizedDescription
            ?: NSLocalizedString(@"External library folder is unavailable.",
                                 @"Error text: the external library folder cannot be reached");
    [self presentLaunchProblemAlertWithMessage:message];
    [self refreshImportedGamesAsync];
    return;
  }

  // Games copied directly into Documents (including subfolders like
  // Documents/<Game>/default.xex and GOD packages) are launched in place. We no
  // longer move them into games/ on launch: relocating a folder/GOD game by its
  // launchable file alone orphaned it from its companion data and broke booting.
  // The library scan already covers Documents, so in-place launch just works.

  NSString* path_ns = ToNSString(launch_path.string());
  std::error_code path_exists_ec;
  if (!std::filesystem::exists(launch_path, path_exists_ec)) {
    [self presentLaunchProblemAlertWithMessage:NSLocalizedString(
        @"Game file is no longer available.",
        @"Alert message: the game file is gone")];
    [self refreshImportedGamesAsync];
    return;
  }

  if (IsLikelyGodContainerFile(launch_path)) {
    auto metadata = xe::vfs::ExtractStfsMetadata(launch_path);
    if (metadata.has_value() && xe::ui::IOSContentDataFileCount(launch_path) > 0 &&
        !HasContentSidecarDataDirectory(launch_path)) {
      XEPresentOKAlert(
          self,
          NSLocalizedString(@"Missing Game Data",
                            @"Alert title: GOD package lacks its .data folder"),
          NSLocalizedString(
              @"This GOD package needs its matching .data folder in the same directory. Transfer "
              @"both the GOD file and its .data folder, then try again.",
              @"Alert message: GOD package lacks its .data folder"));
      return;
    }
  }

  if (!self.jitAcquired) {
    [self presentJITRequiredAlertForLaunchPath:launch_path displayName:display_name];
    return;
  }

  if (!skipMemoryEntitlementWarning) {
    [self updateMemoryEntitlementStatusUI];
    if (!self.memoryEntitlementEnabled) {
      [self presentMemoryEntitlementWarningForLaunchPath:launch_path displayName:display_name];
      return;
    }
  }

  const uint32_t launch_title_id = [self titleIDForGamePath:launch_path];
  NSMutableArray<XeniaIOSExternalLibraryAccess*>* launch_external_accesses = [NSMutableArray array];
  if (external_access) {
    [launch_external_accesses addObject:external_access];
  }
  // Hold every linked folder for the session: the dashboard starts the games in
  // them, and a disc swap can pick a disc from a folder other than this game's.
  const BOOL launches_dashboard = xe::ui::IsIOSInstalledDashboardPath(launch_path);
  [launch_external_accesses
      addObjectsFromArray:xe::ui::StartIOSExternalLibraryAccessForAllLocations()];
  NSString* linked_content_access_error = nil;
  if (![self appendLinkedContentAccessesForTitleID:launch_title_id
                                          accesses:launch_external_accesses
                                      errorMessage:&linked_content_access_error]) {
    [self presentLaunchProblemAlertWithMessage:(linked_content_access_error
        ?: NSLocalizedString(@"Linked content is unavailable.",
                             @"Error text: linked content cannot be reached"))];
    [self refreshImportedGamesAsync];
    return;
  }
  dashboard_session_ = launches_dashboard;
  self.inGameMenuOverlay.exitsToDashboard = NO;

  if (self.gameStopInProgress || self.gameRunning) {
    if (self.appContext) {
      // The new game must not start under the old one's menu or user pause, and a guest thread
      // waiting on a prompt would hold up the old game's teardown.
      XeniaCancelGuestAlerts(self);
      [self hideInGameMenuOverlay];
      if (emulation_paused_by_user_) {
        [self setEmulationPausedByUser:NO];
      }
      self.gameRunning = YES;
      [self.launchStatusView beginLaunchWithTitle:display_name];
      if (self.gameStopInProgress) {
        [self dismissStatusToast];
      }
      self.gameStopInProgress = NO;
      active_game_title_id_ = launch_title_id;
      [self applyTouchLayoutModelForTitleID:active_game_title_id_];
      xe_request_landscape_orientation(self);
      [UIView animateWithDuration:0.3
          animations:^{
            self.launcherOverlayView.alpha = 0.0;
          }
          completion:^(__unused BOOL finished) {
            self.launcherOverlayView.hidden = YES;
            [self updateTouchControlsOverlayVisibilityAnimated:YES];
          }];
      [self retainExternalGameAccessesForLaunch:launch_external_accesses replaceExisting:NO];
      self.appContext->LaunchGame(std::string([path_ns UTF8String]));
    } else {
      [self presentLaunchProblemAlertWithMessage:NSLocalizedString(
          @"XeniOS isn't ready yet. Try again.",
          @"Alert message: launch could not be queued")];
    }
    return;
  }

  self.gameRunning = YES;
  [self.launchStatusView beginLaunchWithTitle:display_name];
  active_game_title_id_ = launch_title_id;
  [self applyTouchLayoutModelForTitleID:active_game_title_id_];

  xe_request_landscape_orientation(self);
  [UIView animateWithDuration:0.3
      animations:^{
        self.launcherOverlayView.alpha = 0.0;
      }
      completion:^(__unused BOOL finished) {
        self.launcherOverlayView.hidden = YES;
        [self updateTouchControlsOverlayVisibilityAnimated:YES];
      }];

  if (self.appContext) {
    [self retainExternalGameAccessesForLaunch:launch_external_accesses replaceExisting:YES];
    self.appContext->LaunchGame(std::string([path_ns UTF8String]));
  } else {
    [self presentLaunchProblemAlertWithMessage:NSLocalizedString(
        @"XeniOS isn't ready yet. Try again.",
        @"Alert message: launch failed")];
    self.launcherOverlayView.hidden = NO;
    self.launcherOverlayView.alpha = 1.0;
    [self updateTouchControlsOverlayVisibilityAnimated:YES];
  }
}

- (BOOL)installTitleUpdateAtPath:(NSString*)path
                          status:(NSString**)status_out
                  notTitleUpdate:(BOOL*)not_title_update_out {
  if (status_out) {
    *status_out = nil;
  }
  if (not_title_update_out) {
    *not_title_update_out = NO;
  }
  if (!self.appContext) {
    if (status_out) {
      *status_out =
          NSLocalizedString(@"App context unavailable.", @"Error text: app context is unavailable");
    }
    return NO;
  }

  std::string status;
  bool not_title_update = false;
  BOOL success = self.appContext->InstallTitleUpdate(std::string([path UTF8String]), &status,
                                                     &not_title_update);
  if (status_out && !status.empty()) {
    *status_out = ToNSString(status);
  }
  if (not_title_update_out) {
    *not_title_update_out = not_title_update;
  }
  return success;
}

- (void)presentGameActionsSheetForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame& game = discovered_games_[game_index];
  NSString* game_title =
      game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
  XeniaIOSGameActionsViewController* actions_controller = [[XeniaIOSGameActionsViewController alloc]
          initWithGameTitle:game_title
                    titleID:game.title_id
      supportsCompatibility:game.title_id != 0 && xe_game_system_supports_compatibility(game.system)
      supportsManageContent:game.title_id != 0 &&
                            xe_game_system_supports_manage_content(game.system)
      supportsDiscSelection:game.discs.size() > 1
            supportsPatches:self.appContext && game.title_id != 0
      supportsZarConversion:xe_game_supports_zar_conversion(game)];

  __unsafe_unretained XeniaViewController* unsafe_self = self;
  actions_controller.actionHandler = ^(XeniaIOSGameAction action) {
    [unsafe_self dismissViewControllerAnimated:YES
                                    completion:^{
                                      [unsafe_self performGameAction:action forIndex:game_index];
                                    }];
  };

  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:actions_controller];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(520.0, 620.0),
                                     NO);
  [self presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [actions_controller release];
}

- (void)performGameAction:(XeniaIOSGameAction)action forIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  switch (action) {
    case XeniaIOSGameActionPlay: {
      const IOSDiscoveredGame& game = discovered_games_[game_index];
      [self launchGameAtPath:game.path displayName:ToNSString(game.title)];
    } break;
    case XeniaIOSGameActionGameSettings:
      [self presentGameSettingsSheetForIndex:game_index];
      break;
    case XeniaIOSGameActionResetGameSettings:
      [self confirmResetGameSettingsForIndex:game_index];
      break;
    case XeniaIOSGameActionTouchLayout:
      [self presentGameTouchLayoutSheetForIndex:game_index];
      break;
    case XeniaIOSGameActionCompatibility:
      [self presentCompatibilitySheetForIndex:game_index];
      break;
    case XeniaIOSGameActionManageContent:
      [self presentManageContentSheetForIndex:game_index];
      break;
    case XeniaIOSGameActionLaunchDisc:
      [self presentDiscSelectionSheetForIndex:game_index];
      break;
    case XeniaIOSGameActionPatches:
      [self presentPatchesSheetForIndex:game_index];
      break;
    case XeniaIOSGameActionConvertToZar:
      [self.zarConversionCoordinator presentConversionOptionsForGames:discovered_games_
                                                                index:game_index];
      break;
    case XeniaIOSGameActionCopyLaunchURL:
      [self copyLaunchURLForGameAtIndex:game_index];
      break;
    case XeniaIOSGameActionSystemLink: {
      const IOSDiscoveredGame& game = discovered_games_[game_index];
      const std::filesystem::path game_path = game.path;
      NSString* display_name = ToNSString(game.title);
      xe_ios_present_system_link_sheet(self, display_name, game.title_id, ^{
        [self launchGameAtPath:game_path displayName:display_name];
      });
    } break;
  }
}

- (void)presentTouchLayoutGamePicker {
  NSMutableArray<XeniaIOSGamePickerItem*>* items =
      [NSMutableArray arrayWithCapacity:discovered_games_.size()];
  for (size_t i = 0; i < discovered_games_.size(); ++i) {
    const IOSDiscoveredGame& game = discovered_games_[i];
    if (!game.title_id) {
      continue;
    }

    NSString* title =
        game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
    NSMutableArray<NSString*>* details = [NSMutableArray array];
    [details
        addObject:[NSString
                      stringWithFormat:NSLocalizedString(
                                           @"Title ID %@",
                                           @"Picker row detail; %@ is a title ID in hexadecimal"),
                                       ToNSString(FormatTitleID(game.title_id))]];
    if (!game.content_type_name.empty()) {
      [details addObject:ToNSString(game.content_type_name)];
    } else if (game.discs.size() > 1) {
      [details addObject:[NSString
                             stringWithFormat:NSLocalizedString(
                                                  @"%zu discs",
                                                  @"Picker row detail; %zu is the number of discs"),
                                              game.discs.size()]];
    }

    [items addObject:[XeniaIOSGamePickerItem itemWithTitle:title
                                                  subtitle:[details componentsJoinedByString:@" · "]
                                                   titleID:game.title_id
                                                 gameIndex:static_cast<NSUInteger>(i)]];
  }

  if (items.count == 0) {
    XEPresentOKAlert(
        self,
        NSLocalizedString(@"No Games Available",
                          @"Alert title: no games for the touch layout picker"),
        NSLocalizedString(@"Import a game with a title ID before editing a per-game touch layout.",
                          @"Alert message: no games for the touch layout picker"));
    return;
  }

  XeniaIOSGamePickerViewController* picker = [[XeniaIOSGamePickerViewController alloc]
      initWithTitle:NSLocalizedString(@"Game Touch Layout",
                                      @"Title of the per-game touch layout picker")
             prompt:NSLocalizedString(@"Choose a title to edit before launch.",
                                      @"Prompt of the per-game touch layout picker")
              items:items];
  __unsafe_unretained XeniaViewController* unsafe_self = self;
  picker.selectionHandler = ^(NSUInteger gameIndex) {
    [unsafe_self dismissViewControllerAnimated:YES
                                    completion:^{
                                      [unsafe_self presentGameTouchLayoutSheetForIndex:gameIndex];
                                    }];
  };

  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:picker];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(560.0, 700.0),
                                     NO);
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [picker release];
}

- (void)presentGameSettingsSheetForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame& game = discovered_games_[game_index];
  if (!game.title_id) {
    XEPresentOKAlert(self,
                     NSLocalizedString(@"Unavailable", @"Alert title: item cannot be configured"),
                     NSLocalizedString(
                         @"This item does not expose a title ID, so game settings cannot be saved.",
                         @"Alert message: game settings need a title ID"));
    return;
  }

  NSString* game_title =
      game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
  XeniaConfigViewController* config_controller =
      [[XeniaConfigViewController alloc] initWithCatalogKind:IOSConfigCatalogKind::kPerGame
                                                       style:UITableViewStyleInsetGrouped
                                                 gameTitleID:game.title_id
                                                   gameTitle:game_title];
  config_controller.appContext = self.appContext;
  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:config_controller];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(640.0, 760.0),
                                     NO);
  [self presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [config_controller release];
}

- (void)confirmResetGameSettingsForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame& game = discovered_games_[game_index];
  if (!game.title_id) {
    XEPresentOKAlert(self,
                     NSLocalizedString(@"Unavailable", @"Alert title: item cannot be configured"),
                     NSLocalizedString(
                         @"This item does not expose a title ID, so game settings cannot be reset.",
                         @"Alert message: game settings need a title ID"));
    return;
  }

  NSString* game_title =
      game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Reset Game Settings?",
                                                 @"Alert title: confirm resetting game settings")
                       message:[NSString
                                   stringWithFormat:NSLocalizedString(
                                                        @"Delete saved overrides for %@ and return "
                                                        @"this title to defaults.",
                                                        @"Alert message; %@ is the game title"),
                                                    game_title]
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                         style:UIAlertActionStyleCancel
                                       handler:nil]];

  const uint32_t title_id = game.title_id;
  __unsafe_unretained XeniaViewController* unsafe_self = self;
  [confirm
      addAction:
          [UIAlertAction
              actionWithTitle:NSLocalizedString(@"Reset",
                                                @"Alert button: confirm resetting game settings")
                        style:UIAlertActionStyleDestructive
                      handler:^(__unused UIAlertAction* action) {
                        const bool deleted = xe::ui::DeleteIOSGameConfig(title_id);
                        NSString* title =
                            deleted ? NSLocalizedString(
                                          @"Game Settings Reset",
                                          @"Alert title: result of resetting game settings")
                                    : NSLocalizedString(
                                          @"Game Settings Not Reset",
                                          @"Alert title: result of resetting game settings");
                        NSString* message =
                            deleted
                                ? NSLocalizedString(
                                      @"Deleted title-specific overrides. Relaunch before testing.",
                                      @"Alert message: game settings were reset")
                                : NSLocalizedString(
                                      @"Failed to delete title-specific overrides. Check "
                                      @"xenia.log.",
                                      @"Alert message: game settings could not be reset");
                        UIViewController* top_presenter =
                            [unsafe_self topPresentedControllerForModalPresentation];
                        UIViewController* presenter = top_presenter ?: unsafe_self;
                        XEPresentOKAlert(presenter, title, message);
                        [unsafe_self showStatusToast:message
                                               style:(deleted ? XeniaIOSStatusToastStyleSuccess
                                                              : XeniaIOSStatusToastStyleError)];
                      }]];
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:confirm animated:YES completion:nil];
}

- (void)presentGameTouchLayoutSheetForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame& game = discovered_games_[game_index];
  if (!game.title_id) {
    XEPresentOKAlert(self,
                     NSLocalizedString(@"Unavailable", @"Alert title: item cannot be configured"),
                     NSLocalizedString(@"This item does not expose a title ID, so a title-specific "
                                       @"touch layout cannot be saved.",
                                       @"Alert message: touch layout needs a title ID"));
    return;
  }

  NSString* game_title =
      game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
  XeniaIOSTouchLayoutEditorViewController* editor_controller =
      [[XeniaIOSTouchLayoutEditorViewController alloc] initWithTitleID:game.title_id
                                                                 title:game_title];
  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:editor_controller];
  navigation_controller.landscapeOnly = YES;
  navigation_controller.modalPresentationStyle = UIModalPresentationFullScreen;
  navigation_controller.navigationBarHidden = YES;
  [self presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [editor_controller release];
}

- (void)presentCompatibilitySheetForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame& game = discovered_games_[game_index];
  if (!game.title_id) {
    XEPresentOKAlert(
        self, NSLocalizedString(@"Unavailable", @"Alert title: item cannot be configured"),
        NSLocalizedString(
            @"This item does not expose a title ID, so compatibility details cannot be loaded.",
            @"Alert message: compatibility details need a title ID"));
    return;
  }

  NSDictionary* compat_data = [compat_data_ objectForKey:XEFormatTitleIDHexUpper(game.title_id)];
  NSString* game_title =
      game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
  // The visible tile already holds the decoded cover, so take that first. Reading and decoding
  // the cached art file on the main thread is only the fallback for a tile that is off screen.
  UIImage* hero_artwork = nil;
  if (self.launcherOverlayView.gamesCollectionView) {
    NSIndexPath* index_path = [NSIndexPath indexPathForItem:(NSInteger)game_index inSection:0];
    XeniaGameTileCell* tile = (XeniaGameTileCell*)[self.launcherOverlayView.gamesCollectionView
        cellForItemAtIndexPath:index_path];
    if ([tile isKindOfClass:[XeniaGameTileCell class]]) {
      hero_artwork = tile.iconView.image;
    }
  }
  if (!hero_artwork) {
    hero_artwork = xe_cached_game_art(game.title_id);
  }
  if (!hero_artwork && !game.icon_data.empty()) {
    NSData* data = [NSData dataWithBytes:game.icon_data.data() length:game.icon_data.size()];
    hero_artwork = [UIImage imageWithData:data];
  }
  XeniaGameCompatibilityViewController* compatibility_controller =
      [[XeniaGameCompatibilityViewController alloc] initWithTitleID:game.title_id
                                                              title:game_title
                                                         compatData:compat_data];
  if (hero_artwork) {
    [compatibility_controller setHeroArtwork:hero_artwork];
  }
  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc]
          initWithRootViewController:compatibility_controller];
  navigation_controller.view.backgroundColor = [XeniaTheme bgPrimary];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(620.0, 720.0),
                                     NO);
  [self presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [compatibility_controller release];
}

- (void)presentManageContentSheetForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame& game = discovered_games_[game_index];
  if (!game.title_id) {
    XEPresentOKAlert(
        self, NSLocalizedString(@"Unavailable", @"Alert title: item cannot be configured"),
        NSLocalizedString(
            @"This item does not expose a title ID, so installed content cannot be managed.",
            @"Alert message: installed content needs a title ID"));
    return;
  }

  // Warm up profile/content services now so installing a title update from this
  // sheet does not fail its first attempt while the emulator is still coming up.
  if (self.appContext) {
    self.appContext->PrepareProfileServices();
  }

  XeniaGameContentViewController* content_controller = [[XeniaGameContentViewController alloc]
      initWithTitleID:game.title_id
                title:(game.title.empty() ? ToNSString(game.path.stem().string())
                                          : ToNSString(game.title))host:self];
  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:content_controller];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(600.0, 680.0),
                                     NO);
  [self presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [content_controller release];
}

- (void)presentDiscSelectionSheetForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame& game = discovered_games_[game_index];
  if (game.discs.size() <= 1) {
    [self launchGameAtPath:game.path displayName:ToNSString(game.title)];
    return;
  }

  NSString* game_title =
      game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
  __unsafe_unretained XeniaViewController* unsafe_self = self;
  XeniaGameDiscViewController* disc_controller = [[XeniaGameDiscViewController alloc]
         initWithTitle:game_title
                 discs:game.discs
      selectionHandler:^(NSString* path, NSString* label) {
        if (!path.length) {
          return;
        }
        NSString* display_name =
            label.length
                ? [NSString
                      stringWithFormat:
                          NSLocalizedString(
                              @"%1$@ (%2$@)",
                              @"Disc picker title; %1$@ is the game title, %2$@ the disc label"),
                          game_title, label]
                : game_title;
        [unsafe_self launchGameAtPath:std::filesystem::path([path UTF8String])
                          displayName:display_name];
      }];
  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:disc_controller];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(520.0, 560.0),
                                     NO);
  [self presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [disc_controller release];
}

- (void)presentPatchesSheetForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame& game = discovered_games_[game_index];
  if (!game.title_id) {
    XEPresentOKAlert(
        self, NSLocalizedString(@"Unavailable", @"Alert title: item cannot be configured"),
        NSLocalizedString(@"This item does not expose a title ID, so patches cannot be loaded.",
                          @"Alert message: patches need a title ID"));
    return;
  }
  if (!self.appContext) {
    XEPresentOKAlert(
        self, NSLocalizedString(@"Unavailable", @"Alert title and message: patches unavailable"),
        NSLocalizedString(@"Patch management is unavailable.",
                          @"Alert title and message: patches unavailable"));
    return;
  }

  NSString* game_title =
      game.title.empty() ? ToNSString(game.path.stem().string()) : ToNSString(game.title);
  XeniaGamePatchesViewController* patches_controller =
      [[XeniaGamePatchesViewController alloc] initWithTitleID:game.title_id
                                                        title:game_title
                                                   appContext:self.appContext];
  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:patches_controller];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(600.0, 680.0),
                                     NO);
  [self presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [patches_controller release];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  CGSize collection_size = self.launcherOverlayView.gamesCollectionView.bounds.size;
  if (!CGSizeEqualToSize(collection_size, last_collection_layout_size_)) {
    last_collection_layout_size_ = collection_size;
    [self.launcherOverlayView.gamesCollectionView.collectionViewLayout invalidateLayout];
  }
  // Keep scrollable content above the home indicator. The collection view's
  // visual frame extends to the screen bottom (so the launcher background
  // doesn't leave a white bar there), but cells should stop scrolling at the
  // safe area to keep the bottom row clear of the home gesture zone.
  CGFloat home_inset = self.view.safeAreaInsets.bottom;
  UIEdgeInsets current = self.launcherOverlayView.gamesCollectionView.contentInset;
  if (current.bottom != home_inset) {
    current.bottom = home_inset;
    self.launcherOverlayView.gamesCollectionView.contentInset = current;
    UIEdgeInsets indicator = self.launcherOverlayView.gamesCollectionView.scrollIndicatorInsets;
    indicator.bottom = home_inset;
    self.launcherOverlayView.gamesCollectionView.scrollIndicatorInsets = indicator;
  }
  [self applyMetalViewLayout];
  // Notify the app context that the layout changed, so the window and
  // presenter can update for rotation, split-view, or safe-area changes.
  if (self.appContext) {
    self.appContext->NotifyLayoutChanged();
  }
}

- (void)refreshLauncherChromeForCurrentTraits {
  [self.launcherOverlayView refreshChromeForCurrentTraits];
  [self.controllerNavigationCoordinator refreshLauncherFocus];
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection
          hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    [self refreshLauncherChromeForCurrentTraits];
  }
}

// External touch-layout install URL safety limits. 64 KB is well over the
// largest official preset and still small enough to reject hostile TOML drops
// before they can spend meaningful CPU time in the parser.
static constexpr NSUInteger kXeniaIOSTouchLayoutMaxBytes = 64 * 1024;
static constexpr NSUInteger kXeniaIOSTouchLayoutURLMaxLength = 2048;

#pragma mark - Window scaling

- (XeniaIOSWindowScalingMode)currentWindowScalingMode {
  return XeniaIOSCurrentWindowScalingMode();
}

- (void)setCurrentWindowScalingMode:(XeniaIOSWindowScalingMode)mode {
  if (mode == XeniaIOSWindowScalingModeStretch && [self isPresentLetterboxEnabled]) {
    [self setPresentLetterboxEnabled:NO];
  }
  XeniaIOSSetCurrentWindowScalingMode(mode);
  [self.view setNeedsLayout];
  [self.view layoutIfNeeded];
}

- (BOOL)isGuestDisplayUncapped {
  if (!self.appContext) {
    return NO;
  }
  return !self.appContext->GetGuestDisplayRefreshCap();
}

- (void)setGuestDisplayUncapped:(BOOL)uncapped {
  if (!self.appContext) {
    return;
  }
  self.appContext->SetGuestDisplayRefreshCap(!uncapped);
}

- (BOOL)isPresentLetterboxEnabled {
  if (!self.appContext) {
    return cvars::present_letterbox;
  }
  return self.appContext->GetPresentLetterbox();
}

- (void)setPresentLetterboxEnabled:(BOOL)enabled {
  if (enabled && [self currentWindowScalingMode] == XeniaIOSWindowScalingModeStretch) {
    enabled = NO;
  }
  if (!self.appContext) {
    cvars::present_letterbox = enabled;
    return;
  }
  self.appContext->SetPresentLetterbox(enabled);
}

- (CGFloat)currentGuestDisplayAspectRatio {
  std::pair<uint32_t, uint32_t> aspect = {16, 9};
  if (self.appContext) {
    aspect = self.appContext->GetGuestDisplayAspectRatio();
  }
  if (!aspect.first || !aspect.second) {
    return 16.0 / 9.0;
  }
  const CGFloat aspect_ratio =
      static_cast<CGFloat>(aspect.first) / static_cast<CGFloat>(aspect.second);
  return std::isfinite(static_cast<double>(aspect_ratio)) && aspect_ratio > 0.0 ? aspect_ratio
                                                                                : 16.0 / 9.0;
}

- (void)applyMetalViewLayout {
  if (!self.metalView) {
    return;
  }
  const CGRect parent = self.view.bounds;
  if (CGRectIsEmpty(parent)) {
    return;
  }
  const CGFloat guest_aspect = [self currentGuestDisplayAspectRatio];
  const CGFloat parent_aspect = parent.size.width / MAX(parent.size.height, 1.0);
  const XeniaIOSWindowScalingMode mode = [self currentWindowScalingMode];
  if (mode == XeniaIOSWindowScalingModeStretch && [self isPresentLetterboxEnabled]) {
    [self setPresentLetterboxEnabled:NO];
  }
  self.metalView.xeniaDrawableAspectRatio =
      mode == XeniaIOSWindowScalingModeStretch ? parent_aspect : guest_aspect;
  CGRect frame = XeniaIOSMetalViewFrameForParent(parent, guest_aspect, mode,
                                                 XeniaIOSCurrentPortraitWindowOffset());

  if (!CGRectEqualToRect(self.metalView.frame, frame)) {
    self.metalView.frame = frame;
  }
}

- (UIMenu*)buildInGameDisplayMenu {
  __unsafe_unretained XeniaViewController* unsafe_self = self;
  const XeniaIOSWindowScalingMode current_mode = [self currentWindowScalingMode];
  NSString* scaling_title = NSLocalizedString(@"Scaling", @"Title of the Scaling submenu");
  switch (current_mode) {
    case XeniaIOSWindowScalingModeFit:
      scaling_title =
          NSLocalizedString(@"Scaling: Fit", @"Title of the Scaling submenu with Fit active");
      break;
    case XeniaIOSWindowScalingModeStretch:
      scaling_title = NSLocalizedString(@"Scaling: Stretch",
                                        @"Title of the Scaling submenu with Stretch active");
      break;
    case XeniaIOSWindowScalingModeZoom:
      scaling_title =
          NSLocalizedString(@"Scaling: Fill", @"Title of the Scaling submenu with Fill active");
      break;
  }

  UIAction* fit_action =
      [UIAction actionWithTitle:NSLocalizedString(@"Fit (Preserve Aspect)", @"Display menu item")
                          image:[UIImage systemImageNamed:@"rectangle.center.inset.filled"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self setCurrentWindowScalingMode:XeniaIOSWindowScalingModeFit];
                          [unsafe_self refreshInGameDisplayMenu];
                        }];
  fit_action.state =
      current_mode == XeniaIOSWindowScalingModeFit ? UIMenuElementStateOn : UIMenuElementStateOff;

  UIAction* stretch_action = [UIAction
      actionWithTitle:NSLocalizedString(@"Stretch (Fill, Ignore Aspect)", @"Display menu item")
                image:[UIImage systemImageNamed:@"arrow.up.left.and.arrow.down.right"]
           identifier:nil
              handler:^(__unused UIAction* action) {
                [unsafe_self setCurrentWindowScalingMode:XeniaIOSWindowScalingModeStretch];
                [unsafe_self refreshInGameDisplayMenu];
              }];
  stretch_action.state = current_mode == XeniaIOSWindowScalingModeStretch ? UIMenuElementStateOn
                                                                          : UIMenuElementStateOff;

  UIAction* zoom_action =
      [UIAction actionWithTitle:NSLocalizedString(@"Zoom (Fill, Crop)", @"Display menu item")
                          image:[UIImage systemImageNamed:@"rectangle.fill"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self setCurrentWindowScalingMode:XeniaIOSWindowScalingModeZoom];
                          [unsafe_self refreshInGameDisplayMenu];
                        }];
  zoom_action.state =
      current_mode == XeniaIOSWindowScalingModeZoom ? UIMenuElementStateOn : UIMenuElementStateOff;

  UIMenu* scaling_submenu =
      [UIMenu menuWithTitle:scaling_title
                      image:[UIImage systemImageNamed:@"rectangle.on.rectangle"]
                 identifier:nil
                    options:0
                   children:@[ fit_action, stretch_action, zoom_action ]];

  const BOOL letterbox_available = current_mode != XeniaIOSWindowScalingModeStretch;
  const BOOL letterbox_enabled = letterbox_available && [self isPresentLetterboxEnabled];
  NSString* presentation_title =
      letterbox_enabled
          ? NSLocalizedString(@"Presentation: Letterbox", @"Title of the Presentation submenu")
          : NSLocalizedString(@"Presentation", @"Title of the Presentation submenu");
  UIAction* letterbox_action =
      [UIAction actionWithTitle:NSLocalizedString(@"Letterbox", @"Display menu item")
                          image:[UIImage systemImageNamed:@"rectangle.center.inset.filled"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          const BOOL next_enabled = ![unsafe_self isPresentLetterboxEnabled];
                          [unsafe_self setPresentLetterboxEnabled:next_enabled];
                          [unsafe_self refreshInGameDisplayMenu];
                        }];
  letterbox_action.state = letterbox_enabled ? UIMenuElementStateOn : UIMenuElementStateOff;
  if (!letterbox_available) {
    letterbox_action.attributes = UIMenuElementAttributesDisabled;
  }

  UIMenu* presentation_submenu =
      [UIMenu menuWithTitle:presentation_title
                      image:[UIImage systemImageNamed:@"rectangle.center.inset.filled"]
                 identifier:nil
                    options:0
                   children:@[ letterbox_action ]];

  const BOOL display_uncapped = [self isGuestDisplayUncapped];
  NSString* refresh_title =
      display_uncapped ? NSLocalizedString(@"Refresh: Uncapped", @"Title of the Refresh submenu")
                       : NSLocalizedString(@"Refresh", @"Title of the Refresh submenu");
  UIAction* uncapped_action = [UIAction
      actionWithTitle:NSLocalizedString(@"Emulated Display Uncapped", @"Display menu item")
                image:[UIImage systemImageNamed:@"speedometer"]
           identifier:nil
              handler:^(__unused UIAction* action) {
                const BOOL next_uncapped = ![unsafe_self isGuestDisplayUncapped];
                [unsafe_self setGuestDisplayUncapped:next_uncapped];
                [unsafe_self refreshInGameDisplayMenu];
              }];
  uncapped_action.state = display_uncapped ? UIMenuElementStateOn : UIMenuElementStateOff;

  UIMenu* refresh_submenu = [UIMenu menuWithTitle:refresh_title
                                            image:[UIImage systemImageNamed:@"speedometer"]
                                       identifier:nil
                                          options:0
                                         children:@[ uncapped_action ]];

  // Portrait position controls are only meaningful in portrait + Fit mode where
  // there's actual slack to drag the surface within.
  const BOOL is_portrait = self.view.bounds.size.height >= self.view.bounds.size.width;
  const BOOL position_available = is_portrait && current_mode == XeniaIOSWindowScalingModeFit;
  UIAction* position_action =
      [UIAction actionWithTitle:NSLocalizedString(@"Position in Portrait...", @"Display menu item")
                          image:[UIImage systemImageNamed:@"hand.draw"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self enterWindowPositionMode];
                        }];
  if (!position_available) {
    position_action.attributes = UIMenuElementAttributesDisabled;
  }

  UIAction* reset_action =
      [UIAction actionWithTitle:NSLocalizedString(@"Reset Position", @"Display menu item")
                          image:[UIImage systemImageNamed:@"arrow.counterclockwise"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self resetWindowPosition];
                        }];

  UIMenu* position_submenu =
      [UIMenu menuWithTitle:NSLocalizedString(@"Position", @"Title of the Position submenu")
                      image:[UIImage systemImageNamed:@"hand.draw"]
                 identifier:nil
                    options:0
                   children:@[ position_action, reset_action ]];

  return [UIMenu
      menuWithTitle:NSLocalizedString(@"Display", @"Title of the Display menu")
           children:@[ scaling_submenu, presentation_submenu, refresh_submenu, position_submenu ]];
}

#pragma mark - Window position drag mode (portrait, Fit only)

- (void)resetWindowPosition {
  XeniaIOSResetPortraitWindowOffset();
  [self.view setNeedsLayout];
  [self.view layoutIfNeeded];
}

- (void)enterWindowPositionMode {
  if (self.windowPositionOverlay) {
    return;
  }
  if (!self.gameRunning) {
    return;
  }
  [self hideInGameMenuOverlay];

  // Hide the touch overlay so its buttons don't compete for the pan gesture.
  // Will be restored on exit.
  if (self.touchControlsOverlay) {
    [self.touchControlsOverlay setGameplayOverlayVisible:NO animated:NO];
  }

  XeniaIOSWindowPositionOverlay* overlay =
      [[XeniaIOSWindowPositionOverlay alloc] initWithFrame:self.view.bounds];
  self.windowPositionOverlay = overlay;
  [overlay release];

  __unsafe_unretained XeniaViewController* unsafe_self = self;
  [self.windowPositionOverlay beginInView:self.view
                                metalView:self.metalView
                               completion:^{
                                 unsafe_self.windowPositionOverlay = nil;
                                 [unsafe_self updateTouchControlsOverlayVisibilityAnimated:YES];
                               }];
}

- (void)exitWindowPositionMode {
  if (!self.windowPositionOverlay) {
    return;
  }
  [self.windowPositionOverlay endAnimated:NO];
}

- (void)openGameTapped:(UIButton*)sender {
  [self presentLibraryAddSheetFromView:sender ?: self.launcherOverlayView];
}

- (void)presentLibraryAddSheetFromView:(UIView*)sourceView {
  XELOGI("iOS library action: presenting Add to Library sheet games={}", discovered_games_.size());
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Add to Library",
                                                 @"Action sheet title: add games to the library")
                       message:NSLocalizedString(
                                   @"Import Copy duplicates the game into XeniOS storage and needs "
                                   @"that much free space. For very large images, Link External "
                                   @"Folder plays them in place without copying.",
                                   @"Action sheet message: explains the two import options")
                preferredStyle:UIAlertControllerStyleActionSheet];

  __unsafe_unretained XeniaViewController* unsafe_self = self;
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(
                                           @"Import Copy",
                                           @"Action sheet button: copies a game into the app")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 XELOGI("iOS library action: Import Copy selected");
                                 [unsafe_self.documentImportCoordinator presentGameImportPicker];
                               }]];
  [alert
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(
                                                   @"Link External Folder",
                                                   @"Action sheet button: links a folder in place")
                                         style:UIAlertActionStyleDefault
                                       handler:^(__unused UIAlertAction* action) {
                                         XELOGI("iOS library action: Link External Folder "
                                                "selected");
                                         [unsafe_self.documentImportCoordinator
                                                 presentExternalLibraryFolderPicker];
                                       }]];
  [alert
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(
                                                   @"Manage External Folders…",
                                                   @"Action sheet button: manages linked folders")
                                         style:UIAlertActionStyleDefault
                                       handler:^(__unused UIAlertAction* action) {
                                         XELOGI("iOS library action: Manage External Folders "
                                                "selected");
                                         [unsafe_self presentExternalFoldersSheet];
                                       }]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel", @"Action sheet button: cancel")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];

  UIPopoverPresentationController* popover = alert.popoverPresentationController;
  if (popover) {
    UIView* anchor = sourceView ?: presenter.view;
    popover.sourceView = anchor;
    popover.sourceRect = anchor ? anchor.bounds : CGRectZero;
    popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
  }
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (void)presentExternalFoldersSheet {
  XeniaIOSExternalFoldersViewController* folders_controller =
      [[XeniaIOSExternalFoldersViewController alloc] init];
  XeniaLandscapeNavigationController* navigation_controller =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:folders_controller];
  XEConfigureDestinationPresentation(navigation_controller, self.view, CGSizeMake(600.0, 680.0),
                                     NO);
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
  [folders_controller release];
}

- (BOOL)appendLinkedContentAccessesForTitleID:(uint32_t)title_id
                                     accesses:
                                         (NSMutableArray<XeniaIOSExternalLibraryAccess*>*)accesses
                                 errorMessage:(NSString**)errorMessage {
  if (errorMessage) {
    *errorMessage = nil;
  }
  if (!title_id) {
    return YES;
  }

  for (IOSInstalledContentEntry& entry : xe_list_linked_content(title_id)) {
    BOOL matched_external_location = NO;
    NSError* external_access_error = nil;
    std::filesystem::path resolved_relative_path;
    XeniaIOSExternalLibraryAccess* access = xe::ui::StartIOSExternalLibraryAccessForPath(
        entry.linked_source_path, &matched_external_location, &resolved_relative_path,
        &external_access_error);
    if (!matched_external_location || !access) {
      if (errorMessage) {
        *errorMessage = external_access_error.localizedDescription
                            ?: NSLocalizedString(@"Linked content external folder is unavailable.",
                                                 @"Error text: linked content cannot be reached");
      }
      return NO;
    }

    std::filesystem::path relative_path =
        entry.linked_relative_path.empty() ? resolved_relative_path : entry.linked_relative_path;
    NSString* access_root_path = access.url.path;
    if (!relative_path.empty() && access_root_path.length == 0) {
      if (errorMessage) {
        *errorMessage = NSLocalizedString(@"Linked content external folder path is unavailable.",
                                          @"Error text: linked content cannot be reached");
      }
      return NO;
    }
    std::filesystem::path current_source_path =
        relative_path.empty()
            ? entry.linked_source_path
            : std::filesystem::path(std::string([access_root_path UTF8String])) / relative_path;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(current_source_path, ec) || ec) {
      if (errorMessage) {
        *errorMessage = NSLocalizedString(@"Linked content source file is no longer available.",
                                          @"Error text: linked content source is gone");
      }
      return NO;
    }

    if (current_source_path != entry.linked_source_path) {
      entry.linked_relative_path = relative_path;
      std::string refresh_error;
      if (!xe_refresh_linked_content_entry(entry, current_source_path, &refresh_error)) {
        if (errorMessage) {
          *errorMessage =
              ToNSString(refresh_error.empty() ? "Linked content refresh failed." : refresh_error);
        }
        return NO;
      }
    }

    if (accesses) {
      [accesses addObject:access];
    }
  }
  return YES;
}

- (void)retainExternalGameAccessForLaunch:(XeniaIOSExternalLibraryAccess*)access
                          replaceExisting:(BOOL)replaceExisting {
  NSArray<XeniaIOSExternalLibraryAccess*>* accesses = access ? @[ access ] : @[];
  [self retainExternalGameAccessesForLaunch:accesses replaceExisting:replaceExisting];
}

- (void)retainExternalGameAccessesForLaunch:(NSArray<XeniaIOSExternalLibraryAccess*>*)accesses
                            replaceExisting:(BOOL)replaceExisting {
  if (!active_external_game_accesses_) {
    active_external_game_accesses_ = [[NSMutableArray alloc] init];
  }
  if (replaceExisting) {
    [active_external_game_accesses_ removeAllObjects];
  }
  for (XeniaIOSExternalLibraryAccess* access in accesses) {
    if (access) {
      [active_external_game_accesses_ addObject:access];
    }
  }
}

- (void)clearActiveExternalGameAccesses {
  [active_external_game_accesses_ removeAllObjects];
}

- (void)openSettingsTapped:(UIButton*)sender {
  (void)sender;
  [self presentSettingsDestinationWithInitialSection:XeniaSettingsInitialSectionMain
                         preventInteractiveDismissal:YES];
}

- (void)presentationControllerDidDismiss:(UIPresentationController*)presentationController {
  (void)presentationController;
  gameplay_modal_presentation_pending_ = NO;
  [self updateTouchControlsOverlayVisibilityAnimated:YES];
}

#pragma mark - Document import coordinator host

- (UIViewController*)documentImportCoordinatorPresenter {
  return [self topPresentedControllerForModalPresentation] ?: self;
}

- (BOOL)documentImportCoordinatorGameStopInProgress {
  return self.gameStopInProgress;
}

- (BOOL)documentImportCoordinatorGameRunning {
  return self.gameRunning;
}

- (BOOL)documentImportCoordinatorJITAcquired {
  return self.jitAcquired;
}

- (void)documentImportCoordinatorSetStatusText:(NSString*)text {
  if (text.length > 0) {
    [self showStatusToastForMessage:text];
  }
}

- (std::filesystem::path)documentImportCoordinatorImportGameAtURL:(NSURL*)sourceURL
                                                            error:(NSError**)error {
  return [self importGameIntoLibrary:sourceURL error:error];
}

- (BOOL)documentImportCoordinatorLinkExternalLibraryAtURL:(NSURL*)folderURL error:(NSError**)error {
  return xe::ui::SaveIOSExternalLibraryLocation(folderURL, error);
}

- (void)documentImportCoordinatorRefreshImportedGames {
  [self refreshImportedGamesAsync];
}

- (void)documentImportCoordinatorRefreshImportedGamesWithCompletion:(void (^)(void))completion {
  [self refreshImportedGamesAsyncWithCompletion:completion];
}

- (void)documentImportCoordinatorRefreshImportedGamesWithScannedGamesCompletion:
    (void (^)(const std::vector<IOSDiscoveredGame>& games))completion {
  [self refreshImportedGamesAsyncWithScannedGamesCompletion:completion];
}

- (void)documentImportCoordinatorPromptForZarConversionAfterAddingPath:
            (const std::filesystem::path&)path
                                                          scannedGames:
                                                              (const std::vector<
                                                                  IOSDiscoveredGame>&)games
                                                       externalLibrary:(BOOL)externalLibrary
                                                            completion:
                                                                (void (^)(BOOL conversionChosen))
                                                                    completion {
  [self.zarConversionCoordinator presentPostImportConversionPromptForGames:games
                                                                 addedPath:path
                                                           externalLibrary:externalLibrary
                                                                completion:completion];
}

- (void)documentImportCoordinatorLaunchGameAtPath:(const std::filesystem::path&)gamePath
                                      displayName:(NSString*)displayName {
  [self launchGameAtPath:gamePath displayName:displayName];
}

- (BOOL)documentImportCoordinatorCanInstallTitleUpdates {
  return self.appContext != nullptr;
}

- (BOOL)documentImportCoordinatorInstallTitleUpdateAtPath:(const std::filesystem::path&)path
                                                   status:(std::string*)statusOut
                                           notTitleUpdate:(bool*)notTitleUpdateOut {
  if (!self.appContext) {
    if (statusOut) {
      *statusOut = "Title update installation is unavailable.";
    }
    if (notTitleUpdateOut) {
      *notTitleUpdateOut = false;
    }
    return NO;
  }
  return self.appContext->InstallTitleUpdate(path.string(), statusOut, notTitleUpdateOut);
}

- (void)documentImportCoordinatorPresentAlertWithTitle:(NSString*)title message:(NSString*)message {
  XEPresentOKAlert(self, title, message);
}

- (void)documentImportCoordinatorImportTouchLayoutAtURL:(NSURL*)url {
  [self.touchLayoutCoordinator importLayoutAtURL:url];
}

- (void)documentImportCoordinatorTouchLayoutImportCancelled {
  if (gameplay_modal_presentation_pending_) {
    gameplay_modal_presentation_pending_ = NO;
    [self updateTouchControlsOverlayVisibilityAnimated:YES];
  }
}

#pragma mark - ZAR conversion coordinator host

- (UIViewController*)zarConversionCoordinatorPresenter {
  return [self topPresentedControllerForModalPresentation] ?: self;
}

- (void)zarConversionCoordinatorShowStatusToast:(NSString*)message
                                          style:(XeniaIOSStatusToastStyle)style {
  [self showStatusToast:message style:style];
}

- (void)zarConversionCoordinatorRefreshImportedGames {
  [self refreshImportedGamesAsync];
}

- (void)zarConversionCoordinatorRefreshImportedGamesWithCompletion:(void (^)(void))completion {
  [self refreshImportedGamesAsyncWithCompletion:completion];
}

#pragma mark - Status bar / home indicator

- (BOOL)prefersStatusBarHidden {
  return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  // Always permit portrait and landscape, including during gameplay.
  // The previous gameplay-only landscape lock conflicted with the window
  // scaling modes; the portrait drag-to-position UX is meaningless if the user
  // can never rotate the device to portrait after launching a game.
  return UIInterfaceOrientationMaskAll;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
  return xe_current_interface_orientation(self.view);
}

- (BOOL)shouldAutorotate {
  return YES;
}

// Only a running game defers the system edge gestures and hides the home indicator,
// so the launcher's home swipe works on the first try.
- (BOOL)prefersHomeIndicatorAutoHidden {
  return self.gameRunning;
}

- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
  return self.gameRunning ? UIRectEdgeAll : UIRectEdgeNone;
}

- (void)setGameRunning:(BOOL)gameRunning {
  if (_gameRunning == gameRunning) {
    return;
  }
  _gameRunning = gameRunning;
  if (!gameRunning && emulation_paused_by_user_) {
    [self setEmulationPausedByUser:NO];
  }
  self.shaderCompileIndicator.active = gameRunning;
  self.launchStatusView.active = gameRunning;
  if (gameRunning) {
    xe_ios_resource_pressure_game_started();
  }
  [self setNeedsUpdateOfHomeIndicatorAutoHidden];
  [self setNeedsUpdateOfScreenEdgesDeferringSystemGestures];
}

#pragma mark - Public API

- (void)showLauncherOverlay {
  if (touch_layout_edit_mode_active_) {
    [self saveTouchLayoutModelForTitleID:active_game_title_id_];
    [self setTouchLayoutEditModeActive:NO animated:NO];
  }
  [self saveTouchLayoutModelForTitleID:active_game_title_id_];
  active_game_title_id_ = 0;
  active_touch_layout_local_id_.clear();
  gameplay_modal_presentation_pending_ = NO;
  [self clearActiveExternalGameAccesses];
  self.gameRunning = NO;
  if (self.gameStopInProgress) {
    [self dismissStatusToast];  // The "Stopping game..." state is over.
  }
  self.gameStopInProgress = NO;
  // A sheet or alert left up by the game that ended: the "Game Isn't Responding" alert, or the
  // achievements sheet a guest thread is still waiting on (its dismissal reports completion).
  // The launch-failure alert is presented after this runs. Guest prompts are answered before
  // anything is dismissed, so no thread is left waiting on one.
  XeniaCancelGuestAlerts(self);
  if (self.presentedViewController) {
    [self dismissViewControllerAnimated:NO completion:nil];
  }
  [self hideInGameMenuOverlay];
  self.launcherOverlayView.hidden = NO;
  [self refreshSignedInProfileUI];
  [self updateJITStatusIndicator];
  [self updateJITAvailabilityUI];
  [self updateMemoryEntitlementStatusUI];
  [self refreshImportedGamesAsync];
  [self.controllerNavigationCoordinator refreshLauncherFocus];
  xe_request_current_orientation(self);
  [UIView animateWithDuration:0.3
                   animations:^{
                     self.launcherOverlayView.alpha = 1.0;
                   }];
  [self updateTouchControlsOverlayVisibilityAnimated:YES];
}

- (void)presentGameLaunchFailureAlertWithMessage:(NSString*)message {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Couldn't Start Game",
                                                 @"Alert title: the game failed to start")
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Share Log",
                                                         @"Alert button: shares the log file")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 xe_ios_present_log_share(self, nil, NO);
                               }]];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button: dismiss")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (void)presentGameStopStuckAlert {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Game Isn't Responding",
                                                 @"Alert title: the game is not stopping")
                       message:NSLocalizedString(@"The game is taking too long to stop. You can "
                                                 @"wait a little longer, or force-quit XeniOS from "
                                                 @"the app switcher and open it again.",
                                                 @"Alert message: the game is not stopping")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Share Log",
                                                         @"Alert button: shares the log file")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 xe_ios_present_log_share(self, nil, NO);
                               }]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(
                                           @"Keep Waiting",
                                           @"Alert button: keep waiting for the game to stop")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  UIViewController* presenter = [self topPresentedControllerForModalPresentation] ?: self;
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (void)dealloc {
  [self.jitPollTimer invalidate];
  [self.controllerNavigationCoordinator invalidate];
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [compat_data_ release];
  [launcher_icon_cache_ release];
  [launcher_snapshot_signature_ release];
  [_controllerNavigationCoordinator release];
  [_documentImportCoordinator release];
  [_zarConversionCoordinator release];
  [_touchLayoutCoordinator release];
  [_windowPositionOverlay release];
  [_touchControlsOverlay release];
  [_shaderCompileIndicator release];
  [_launchStatusView release];
  [_achievementNotificationPresenter release];
  [_statusToastPresenter release];
  [active_external_game_accesses_ release];
  [pending_url_confirmation_ release];
  [_metalView release];
  [super dealloc];
}

#pragma mark - Touch overlay state

- (xe::hid::touch::IOSTouchRuntimeModel*)touchRuntimeModel {
  return touch_runtime_model_.get();
}

- (uint32_t)titleIDForGamePath:(const std::filesystem::path&)game_path {
  for (const IOSDiscoveredGame& game : discovered_games_) {
    if (game.path == game_path && game.title_id) {
      return game.title_id;
    }
    for (const auto& disc : game.discs) {
      if (disc.path == game_path && game.title_id) {
        return game.title_id;
      }
    }
  }
  IOSDiscoveredGame discovered_game;
  if (BuildDiscoveredGameFromPath(game_path, &discovered_game)) {
    return discovered_game.title_id;
  }
  return 0;
}

- (BOOL)hasConnectedGameplayController {
  for (GCController* controller in GCController.controllers) {
    if (controller.extendedGamepad || controller.microGamepad) {
      return YES;
    }
  }
  return NO;
}

- (BOOL)shouldBlockGameplayInput {
  return self.gameRunning &&
         (touch_layout_edit_mode_active_ || gameplay_modal_presentation_pending_ ||
          !self.inGameMenuOverlay.hidden || self.presentedViewController != nil);
}

- (BOOL)shouldShowTouchControlsOverlay {
  if (touch_layout_edit_mode_active_) {
    return self.gameRunning && self.launcherOverlayView.hidden && self.inGameMenuOverlay.hidden &&
           self.presentedViewController == nil;
  }

  if (!cvars::ios_touch_overlay) {
    return NO;
  }

  return self.gameRunning && self.launcherOverlayView.hidden && self.inGameMenuOverlay.hidden &&
         self.presentedViewController == nil && !gameplay_modal_presentation_pending_ &&
         ![self hasConnectedGameplayController];
}

- (void)setTouchLayoutEditModeActive:(BOOL)active animated:(BOOL)animated {
  touch_layout_edit_mode_active_ = active;
  if (self.touchControlsOverlay) {
    [self.touchControlsOverlay setEditingControlsEnabled:active animated:animated];
  }
  [self updateTouchControlsOverlayVisibilityAnimated:animated];
}

- (void)finishTouchLayoutEditMode {
  [self saveTouchLayoutModelForTitleID:active_game_title_id_];
  [self setTouchLayoutEditModeActive:NO animated:YES];
}

- (void)updateTouchControlsOverlayVisibilityAnimated:(BOOL)animated {
  const BOOL overlay_visible = [self shouldShowTouchControlsOverlay];
  if (self.touchControlsOverlay) {
    [self.touchControlsOverlay setGameplayOverlayVisible:overlay_visible animated:animated];
  }
  if (self.appContext) {
    const bool covered = [self shouldBlockGameplayInput] ? true : false;
    self.appContext->SetGameplayInputBlocked(covered);
    // The menu and sheets leave the game running, so a System Link game keeps going for the
    // other players. A pause the user chose in the menu ends when the game is uncovered.
    if (!covered && emulation_paused_by_user_) {
      [self setEmulationPausedByUser:NO];
    }
  }
  // The screen-wide tap recognizer used to toggle the in-game menu would
  // otherwise fire every time the user presses a touch overlay button (the
  // recognizer's cancelsTouchesInView is NO so the button still fires too).
  // While the overlay is the active input surface the user opens the menu via
  // its dedicated pause button; the tap-anywhere fallback only matters when
  // the overlay is hidden (e.g. hardware controller connected) or when the
  // in-game menu is up and a tap-outside should dismiss it.
  in_game_menu_tap_recognizer_.enabled = !overlay_visible;
}

- (void)showStatusToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style {
  [self.statusToastPresenter presentMessage:message style:style inView:self.view];
}

- (void)showPersistentStatusToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style {
  [self.statusToastPresenter presentMessage:message style:style inView:self.view duration:0.0];
}

- (void)updateStatusToast:(NSString*)message {
  [self.statusToastPresenter updateMessage:message];
}

- (void)dismissStatusToast {
  [self.statusToastPresenter dismiss];
}

// Errors and states with a next step are never toasts: a toast vanishes in a few seconds and has
// nothing to tap, so a failed launch or link gets an alert with a button instead.
- (void)presentLaunchProblemAlertWithMessage:(NSString*)message {
  XEPresentOKAlert(
      self, NSLocalizedString(@"Something Went Wrong", @"Alert title: a game or link failed"),
      message);
}

- (void)presentLinkCouldNotBeOpenedAlert {
  XEPresentOKAlert(self, NSLocalizedString(@"Can't Open Link", @"Alert title: a link failed"),
                   NSLocalizedString(@"That link could not be opened.",
                                     @"Alert message: an incoming link was not usable"));
}

- (void)showStatusToastForMessage:(NSString*)message {
  if (message.length == 0) {
    return;
  }
  NSString* lower = [message lowercaseString];
  XeniaIOSStatusToastStyle style = XeniaIOSStatusToastStyleInfo;
  if ([lower rangeOfString:@"failed"].location != NSNotFound ||
      [lower rangeOfString:@"unable"].location != NSNotFound ||
      [lower rangeOfString:@"missing"].location != NSNotFound ||
      [lower rangeOfString:@"not found"].location != NSNotFound ||
      [lower rangeOfString:@"unavailable"].location != NSNotFound ||
      [lower rangeOfString:@"could not"].location != NSNotFound ||
      [lower rangeOfString:@"rejected"].location != NSNotFound) {
    style = XeniaIOSStatusToastStyleError;
  } else if ([lower rangeOfString:@"signed in"].location != NSNotFound ||
             [lower rangeOfString:@"copied"].location != NSNotFound ||
             [lower rangeOfString:@"imported"].location != NSNotFound ||
             [lower rangeOfString:@"installed"].location != NSNotFound ||
             [lower rangeOfString:@"applied"].location != NSNotFound ||
             [lower rangeOfString:@"renamed"].location != NSNotFound ||
             [lower rangeOfString:@"deleted"].location != NSNotFound ||
             [lower rangeOfString:@"saved copy"].location != NSNotFound ||
             [lower rangeOfString:@"restored"].location != NSNotFound ||
             [lower rangeOfString:@"sent"].location != NSNotFound) {
    style = XeniaIOSStatusToastStyleSuccess;
  }
  [self showStatusToast:message style:style];
}

- (void)pauseButtonTapped:(id)__unused sender {
  if (self.launcherOverlayView.hidden == NO || !self.gameRunning || self.presentedViewController) {
    return;
  }

  [self showInGameMenuOverlayAnimated:YES];
}

#pragma mark - Touch layout UI coordinator host

- (UIViewController*)topPresentedControllerForModalPresentation {
  UIViewController* presenter = self;
  while (presenter.presentedViewController) {
    presenter = presenter.presentedViewController;
  }
  return presenter;
}

- (xe::hid::touch::IOSTouchRuntimeModel*)touchLayoutCoordinatorRuntimeModel {
  return touch_runtime_model_.get();
}

- (uint32_t)touchLayoutCoordinatorActiveTitleID {
  return active_game_title_id_;
}

- (std::string)touchLayoutCoordinatorActiveLocalID {
  return active_touch_layout_local_id_;
}

- (void)touchLayoutCoordinatorSetActiveLocalID:(const std::string&)localID {
  active_touch_layout_local_id_ = localID;
}

- (BOOL)touchLayoutCoordinatorGameRunning {
  return self.gameRunning;
}

- (BOOL)touchLayoutCoordinatorCanPresentPendingInstall {
  return self.view.window &&
         [UIApplication sharedApplication].applicationState == UIApplicationStateActive &&
         !self.presentedViewController;
}

- (UIViewController*)touchLayoutCoordinatorTopPresenter {
  return [self topPresentedControllerForModalPresentation];
}

- (void)touchLayoutCoordinatorSetGameplayModalPresentationPending:(BOOL)pending {
  gameplay_modal_presentation_pending_ = pending;
}

- (void)touchLayoutCoordinatorUpdateTouchOverlayVisibilityAnimated:(BOOL)animated {
  [self updateTouchControlsOverlayVisibilityAnimated:animated];
}

- (void)touchLayoutCoordinatorRefreshTouchOverlayLayoutModel {
  [self.touchControlsOverlay refreshLayoutModel];
}

- (BOOL)touchLayoutCoordinatorIsShowingLayoutLibrary {
  return self.touchControlsOverlay && self.touchControlsOverlay.isShowingLayoutLibrary;
}

- (void)touchLayoutCoordinatorShowLayoutLibraryWithItems:
            (NSArray<XeniaTouchLayoutLibraryItem*>*)items
                                    currentLayoutLocalID:(NSString*)currentLayoutLocalID {
  [self.touchControlsOverlay showLayoutLibraryWithItems:items
                                   currentLayoutLocalID:currentLayoutLocalID];
}

- (void)touchLayoutCoordinatorSetStatusText:(NSString*)text style:(XeniaIOSStatusToastStyle)style {
  if (text.length > 0) {
    [self showStatusToast:text style:style];
  }
}

- (void)touchLayoutCoordinatorPresentAlertWithTitle:(NSString*)title message:(NSString*)message {
  XEPresentOKAlert(self, title, message);
}

- (void)touchLayoutCoordinatorPresentKeyboardPromptWithTitle:(NSString*)title
                                                 description:(NSString*)description
                                                 defaultText:(NSString*)defaultText
                                                  completion:(void (^)(BOOL cancelled,
                                                                       NSString* text))completion {
  [self presentSystemKeyboardPromptWithTitle:title
                                 description:description
                                 defaultText:defaultText
                                  completion:completion];
}

- (void)touchLayoutCoordinatorOpenTouchLayoutFileImportPicker {
  UIViewController* presenter = [self topPresentedControllerForModalPresentation];
  [self.documentImportCoordinator presentTouchLayoutImportPickerFromViewController:presenter];
}

- (void)touchLayoutCoordinatorEvaluateAutomaticStikDebugJITHandoffIfNeeded {
  [self evaluateAutomaticStikDebugJITHandoffIfNeeded];
}

#pragma mark - Touch layout coordination

- (void)applyDefaultTouchLayoutModel {
  [self.touchLayoutCoordinator applyDefaultLayoutModel];
}

- (void)applyTouchLayoutModelForTitleID:(uint32_t)title_id {
  [self.touchLayoutCoordinator applyLayoutModelForTitleID:title_id];
}

- (void)saveTouchLayoutModelForTitleID:(uint32_t)title_id {
  [self.touchLayoutCoordinator saveCurrentLayoutForTitleID:title_id];
}

- (void)presentTouchLayoutLibrarySheet {
  [self.touchLayoutCoordinator presentLibrary];
}

- (void)presentRenameTouchLayoutSheet {
  [self.touchLayoutCoordinator presentRenameSheet];
}

- (void)presentDeleteTouchLayoutSheet {
  [self.touchLayoutCoordinator presentDeleteSheet];
}

- (void)saveCurrentTouchLayoutCopy {
  [self.touchLayoutCoordinator saveCurrentLayoutCopy];
}

- (void)importTouchLayoutFromFile {
  [self.touchLayoutCoordinator importFromFile];
}

- (void)exportCurrentTouchLayout {
  [self.touchLayoutCoordinator exportCurrentLayout];
}

- (void)resetToOfficialTouchLayoutPreset {
  [self.touchLayoutCoordinator resetToOfficialPreset];
}

- (BOOL)handleExternalTouchLayoutFileURL:(NSURL*)url {
  return [self.touchLayoutCoordinator handleExternalFileURL:url];
}

- (BOOL)handleExternalTouchLayoutSchemeURL:(NSURL*)url {
  return [self.touchLayoutCoordinator handleExternalSchemeURL:url];
}

- (void)presentPendingTouchLayoutInstallIfReady {
  [self.touchLayoutCoordinator presentPendingInstallIfReady];
}

@end
