/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_controller_navigation_coordinator.h"

#import <GameController/GameController.h>

#include <algorithm>
#include <chrono>

#import "xenia/ui/ios/settings/ios_log_view_controller.h"

namespace {

using IOSFocusNodeId = xe::ui::apple::FocusNodeId;
static constexpr IOSFocusNodeId kLauncherFocusSettings = 1;
static constexpr IOSFocusNodeId kLauncherFocusProfile = 2;
static constexpr IOSFocusNodeId kLauncherFocusImport = 3;
static constexpr IOSFocusNodeId kLauncherFocusLibrary = 4;
static constexpr IOSFocusNodeId kInGameFocusResume = 101;
static constexpr IOSFocusNodeId kInGameFocusEdit = 102;
static constexpr IOSFocusNodeId kInGameFocusAchievements = 103;
static constexpr IOSFocusNodeId kInGameFocusDisplay = 104;
static constexpr IOSFocusNodeId kInGameFocusSettings = 105;
static constexpr IOSFocusNodeId kInGameFocusLog = 106;
static constexpr IOSFocusNodeId kInGameFocusExit = 107;
static constexpr IOSFocusNodeId kInGameFocusGraphics = 108;

uint64_t GetNowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

int16_t ToThumbAxis(float value) {
  const float clamped = std::clamp(value, -1.0f, 1.0f);
  return static_cast<int16_t>(clamped * 32767.0f);
}

uint8_t ToTriggerAxis(float value) {
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return static_cast<uint8_t>(clamped * 255.0f);
}

XeniaIOSInGameMenuAction InGameActionForFocus(IOSFocusNodeId focus) {
  switch (focus) {
    case kInGameFocusResume:
      return XeniaIOSInGameMenuActionResume;
    case kInGameFocusEdit:
      return XeniaIOSInGameMenuActionEditControls;
    case kInGameFocusAchievements:
      return XeniaIOSInGameMenuActionAchievements;
    case kInGameFocusDisplay:
      return XeniaIOSInGameMenuActionDisplay;
    case kInGameFocusSettings:
      return XeniaIOSInGameMenuActionSettings;
    case kInGameFocusLog:
      return XeniaIOSInGameMenuActionLiveLog;
    case kInGameFocusExit:
      return XeniaIOSInGameMenuActionExit;
    case kInGameFocusGraphics:
      return XeniaIOSInGameMenuActionGraphics;
    default:
      return XeniaIOSInGameMenuActionNone;
  }
}

BOOL TriggerBarButtonItem(UIBarButtonItem* item) {
  if (!item || !item.enabled || !item.target || !item.action) {
    return NO;
  }
  return [[UIApplication sharedApplication] sendAction:item.action
                                                    to:item.target
                                                  from:item
                                              forEvent:nil];
}

}  // namespace

@implementation XeniaIOSControllerNavigationCoordinator {
  id<XeniaIOSControllerNavigationHost> _host;
  NSTimer* _timer;
  xe::ui::apple::ControllerNavigationMapper _mapper;
  xe::ui::apple::FocusGraph _launcherFocusGraph;
  xe::ui::apple::FocusGraph _inGameFocusGraph;
  NSInteger _focusedGameIndex;
  BOOL _launcherLibraryFocusActive;
  BOOL _navigationWasEnabled;
  uint32_t _nativeControllerPacketNumber;
}

- (instancetype)initWithHost:(id<XeniaIOSControllerNavigationHost>)host {
  if (!(self = [super init])) {
    return nil;
  }
  _host = host;
  _focusedGameIndex = -1;
  _launcherLibraryFocusActive = NO;
  _navigationWasEnabled = NO;
  _nativeControllerPacketNumber = 0;
  _mapper.Reset();
  return self;
}

- (void)dealloc {
  [self invalidate];
  [super dealloc];
}

- (NSInteger)focusedGameIndex {
  return _focusedGameIndex;
}

- (void)start {
  if (_timer) {
    return;
  }
  _timer = [[NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                            target:self
                                          selector:@selector(poll:)
                                          userInfo:nil
                                           repeats:YES] retain];
  _timer.tolerance = 0.01;
}

- (void)invalidate {
  [_timer invalidate];
  [_timer release];
  _timer = nil;
}

- (void)setFocusedGameIndex:(NSInteger)index scroll:(BOOL)scroll {
  const NSInteger game_count = [_host controllerNavigationGameCount];
  if (game_count <= 0) {
    index = -1;
  } else {
    if (index < 0) {
      index = 0;
    }
    const NSInteger max_index = game_count - 1;
    if (index > max_index) {
      index = max_index;
    }
  }
  _focusedGameIndex = index;
  [_host controllerNavigationApplyFocusedGameIndex:_focusedGameIndex scroll:scroll];
}

- (void)refreshLauncherFocus {
  if (![_host controllerNavigationHasConnectedController]) {
    _navigationWasEnabled = NO;
    _mapper.Reset();
    _launcherFocusGraph.Clear();
    _inGameFocusGraph.Clear();
    [self applyLauncherFocusVisuals];
    [self applyInGameMenuFocusVisuals];
    return;
  }
  _navigationWasEnabled = YES;
  [self rebuildLauncherFocusGraph];
  [self applyLauncherFocusVisuals];
}

- (void)refreshInGameFocus {
  if (![_host controllerNavigationHasConnectedController]) {
    _navigationWasEnabled = NO;
    _mapper.Reset();
    _launcherFocusGraph.Clear();
    _inGameFocusGraph.Clear();
    [self applyLauncherFocusVisuals];
    [self applyInGameMenuFocusVisuals];
    return;
  }
  _navigationWasEnabled = YES;
  [self rebuildInGameFocusGraph];
  [self applyInGameMenuFocusVisuals];
}

- (void)focusDefaultInGameAction {
  [self rebuildInGameFocusGraph];
  _inGameFocusGraph.SetCurrent(kInGameFocusResume);
  [self applyInGameMenuFocusVisuals];
}

- (void)rebuildLauncherFocusGraph {
  IOSFocusNodeId previous_focus = _launcherFocusGraph.current();
  _launcherFocusGraph.Clear();

  const BOOL launcher_visible = [_host controllerNavigationLauncherVisible];
  const BOOL actions_enabled =
      launcher_visible && [_host controllerNavigationLauncherActionsEnabled];

  xe::ui::apple::FocusNode settings;
  settings.id = kLauncherFocusSettings;
  settings.right = kLauncherFocusProfile;
  settings.down = kLauncherFocusImport;
  settings.enabled = actions_enabled;

  xe::ui::apple::FocusNode profile;
  profile.id = kLauncherFocusProfile;
  profile.left = kLauncherFocusSettings;
  profile.down = kLauncherFocusImport;
  profile.enabled = actions_enabled;

  xe::ui::apple::FocusNode import_button;
  import_button.id = kLauncherFocusImport;
  import_button.left = kLauncherFocusProfile;
  import_button.right = kLauncherFocusLibrary;
  import_button.up = kLauncherFocusSettings;
  import_button.down = kLauncherFocusLibrary;
  import_button.enabled = actions_enabled;

  xe::ui::apple::FocusNode library;
  library.id = kLauncherFocusLibrary;
  library.left = kLauncherFocusImport;
  library.up = kLauncherFocusImport;
  library.enabled = [_host controllerNavigationGameCount] > 0;

  _launcherFocusGraph.AddOrUpdateNode(settings);
  _launcherFocusGraph.AddOrUpdateNode(profile);
  _launcherFocusGraph.AddOrUpdateNode(import_button);
  _launcherFocusGraph.AddOrUpdateNode(library);

  if (previous_focus != xe::ui::apple::kInvalidFocusNodeId) {
    _launcherFocusGraph.SetCurrent(previous_focus);
  }
}

- (void)rebuildInGameFocusGraph {
  IOSFocusNodeId previous_focus = _inGameFocusGraph.current();
  _inGameFocusGraph.Clear();

  xe::ui::apple::FocusNode resume;
  resume.id = kInGameFocusResume;
  resume.down = kInGameFocusEdit;
  resume.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                              XeniaIOSInGameMenuActionResume];

  // Row 1 left: Edit Controls
  xe::ui::apple::FocusNode edit_controls;
  edit_controls.id = kInGameFocusEdit;
  edit_controls.up = kInGameFocusResume;
  edit_controls.down = kInGameFocusDisplay;
  edit_controls.right = kInGameFocusGraphics;
  edit_controls.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                     XeniaIOSInGameMenuActionEditControls];

  // Row 1 right: Graphics
  xe::ui::apple::FocusNode graphics;
  graphics.id = kInGameFocusGraphics;
  graphics.up = kInGameFocusResume;
  graphics.down = kInGameFocusSettings;
  graphics.left = kInGameFocusEdit;
  graphics.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                XeniaIOSInGameMenuActionGraphics];

  // Row 2 left: Display
  xe::ui::apple::FocusNode display_node;
  display_node.id = kInGameFocusDisplay;
  display_node.up = kInGameFocusEdit;
  display_node.down = kInGameFocusAchievements;
  display_node.right = kInGameFocusSettings;
  display_node.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                    XeniaIOSInGameMenuActionDisplay];

  // Row 2 right: Settings
  xe::ui::apple::FocusNode settings;
  settings.id = kInGameFocusSettings;
  settings.up = kInGameFocusGraphics;
  settings.down = kInGameFocusExit;
  settings.left = kInGameFocusDisplay;
  settings.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                XeniaIOSInGameMenuActionSettings];

  // Row 3 left: Achievements
  xe::ui::apple::FocusNode achievements;
  achievements.id = kInGameFocusAchievements;
  achievements.up = kInGameFocusDisplay;
  achievements.down = kInGameFocusLog;
  achievements.right = kInGameFocusExit;
  achievements.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                     XeniaIOSInGameMenuActionAchievements];

  // Row 3 right: Exit
  xe::ui::apple::FocusNode exit_node;
  exit_node.id = kInGameFocusExit;
  exit_node.up = kInGameFocusSettings;
  exit_node.down = kInGameFocusLog;
  exit_node.left = kInGameFocusAchievements;
  exit_node.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                 XeniaIOSInGameMenuActionExit];

  // Footer: Live Log
  xe::ui::apple::FocusNode log;
  log.id = kInGameFocusLog;
  log.up = kInGameFocusAchievements;
  log.down = kInGameFocusResume;
  log.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                           XeniaIOSInGameMenuActionLiveLog];

  _inGameFocusGraph.AddOrUpdateNode(resume);
  _inGameFocusGraph.AddOrUpdateNode(edit_controls);
  _inGameFocusGraph.AddOrUpdateNode(graphics);
  _inGameFocusGraph.AddOrUpdateNode(display_node);
  _inGameFocusGraph.AddOrUpdateNode(settings);
  _inGameFocusGraph.AddOrUpdateNode(achievements);
  _inGameFocusGraph.AddOrUpdateNode(exit_node);
  _inGameFocusGraph.AddOrUpdateNode(log);

  if (previous_focus != xe::ui::apple::kInvalidFocusNodeId) {
    _inGameFocusGraph.SetCurrent(previous_focus);
  }
}

- (void)applyLauncherFocusVisuals {
  if (!_navigationWasEnabled) {
    _launcherLibraryFocusActive = NO;
    [_host controllerNavigationApplyLauncherFocusEnabled:NO
                                         settingsFocused:NO
                                          profileFocused:NO
                                           importFocused:NO
                                      libraryFocusActive:NO];
    return;
  }

  IOSFocusNodeId current_focus = _launcherFocusGraph.current();
  BOOL settings_focused = current_focus == kLauncherFocusSettings;
  BOOL profile_focused = current_focus == kLauncherFocusProfile;
  BOOL import_focused = current_focus == kLauncherFocusImport;
  BOOL library_focused = current_focus == kLauncherFocusLibrary;

  if (library_focused && _focusedGameIndex < 0 &&
      [_host controllerNavigationGameCount] > 0) {
    [self setFocusedGameIndex:0 scroll:NO];
  }

  _launcherLibraryFocusActive = library_focused;
  [_host controllerNavigationApplyLauncherFocusEnabled:YES
                                      settingsFocused:settings_focused
                                       profileFocused:profile_focused
                                        importFocused:import_focused
                                   libraryFocusActive:_launcherLibraryFocusActive];
}

- (void)applyInGameMenuFocusVisuals {
  if (![_host controllerNavigationInGameMenuVisible] || !_navigationWasEnabled) {
    [_host controllerNavigationApplyInGameMenuFocusEnabled:NO
                                            focusedAction:XeniaIOSInGameMenuActionNone];
    return;
  }

  [_host controllerNavigationApplyInGameMenuFocusEnabled:YES
                                          focusedAction:InGameActionForFocus(
                                                            _inGameFocusGraph.current())];
}

- (BOOL)handleControllerActionsForTableController:(UITableViewController*)table_controller
                                          actions:
                                              (const xe::ui::apple::ControllerActionSet&)actions {
  UITableView* table_view = table_controller.tableView;
  if (!table_view) {
    return NO;
  }

  NSMutableArray<NSIndexPath*>* all_paths = [NSMutableArray array];
  NSInteger sections = [table_view numberOfSections];
  for (NSInteger section = 0; section < sections; ++section) {
    NSInteger rows = [table_view numberOfRowsInSection:section];
    for (NSInteger row = 0; row < rows; ++row) {
      [all_paths addObject:[NSIndexPath indexPathForRow:row inSection:section]];
    }
  }
  if (all_paths.count == 0) {
    return NO;
  }

  NSIndexPath* selected = table_view.indexPathForSelectedRow;
  NSInteger selected_index = 0;
  if (selected) {
    NSUInteger found = [all_paths indexOfObject:selected];
    if (found != NSNotFound) {
      selected_index = static_cast<NSInteger>(found);
    }
  } else {
    selected = all_paths.firstObject;
    [table_view selectRowAtIndexPath:selected
                            animated:NO
                      scrollPosition:UITableViewScrollPositionMiddle];
  }

  BOOL handled = NO;
  if (actions.navigate_up && selected_index > 0) {
    selected_index--;
    handled = YES;
  }
  if (actions.navigate_down && selected_index + 1 < static_cast<NSInteger>(all_paths.count)) {
    selected_index++;
    handled = YES;
  }
  if (actions.page_prev && selected_index > 0) {
    NSInteger step = std::max<NSInteger>(1, table_view.indexPathsForVisibleRows.count - 1);
    selected_index = std::max<NSInteger>(0, selected_index - step);
    handled = YES;
  }
  if (actions.page_next && selected_index + 1 < static_cast<NSInteger>(all_paths.count)) {
    NSInteger step = std::max<NSInteger>(1, table_view.indexPathsForVisibleRows.count - 1);
    selected_index =
        std::min<NSInteger>(static_cast<NSInteger>(all_paths.count - 1), selected_index + step);
    handled = YES;
  }
  if (actions.section_prev && selected.section > 0) {
    for (NSInteger target_section = selected.section - 1; target_section >= 0; --target_section) {
      NSInteger rows = [table_view numberOfRowsInSection:target_section];
      if (rows > 0) {
        selected = [NSIndexPath indexPathForRow:0 inSection:target_section];
        handled = YES;
        break;
      }
    }
  } else if (actions.section_next && selected.section + 1 < sections) {
    for (NSInteger target_section = selected.section + 1; target_section < sections;
         ++target_section) {
      NSInteger rows = [table_view numberOfRowsInSection:target_section];
      if (rows > 0) {
        selected = [NSIndexPath indexPathForRow:0 inSection:target_section];
        handled = YES;
        break;
      }
    }
  } else {
    selected = all_paths[selected_index];
  }

  if (handled && selected) {
    [table_view selectRowAtIndexPath:selected
                            animated:YES
                      scrollPosition:UITableViewScrollPositionMiddle];
  }

  if (actions.accept && selected) {
    UITableViewCell* cell = [table_view cellForRowAtIndexPath:selected];
    if ([cell.accessoryView isKindOfClass:[UISwitch class]]) {
      UISwitch* toggle = (UISwitch*)cell.accessoryView;
      [toggle setOn:!toggle.isOn animated:YES];
      [toggle sendActionsForControlEvents:UIControlEventValueChanged];
    } else {
      id<UITableViewDelegate> delegate = table_view.delegate;
      if ([delegate respondsToSelector:@selector(tableView:didSelectRowAtIndexPath:)]) {
        [delegate tableView:table_view didSelectRowAtIndexPath:selected];
      }
    }
    handled = YES;
  }

  if (actions.quick_action &&
      TriggerBarButtonItem(table_controller.navigationItem.rightBarButtonItem)) {
    handled = YES;
  }

  if (actions.back) {
    UINavigationController* nav = table_controller.navigationController;
    if (nav && nav.viewControllers.count > 1) {
      [nav popViewControllerAnimated:YES];
    } else {
      [table_controller dismissViewControllerAnimated:YES completion:nil];
    }
    handled = YES;
  }

  return handled;
}

- (BOOL)handlePresentedControllerActions:(const xe::ui::apple::ControllerActionSet&)actions {
  UIViewController* presented = [_host controllerNavigationPresentedController];
  if (!presented) {
    return NO;
  }

  if ([presented isKindOfClass:[UIAlertController class]]) {
    if (actions.back) {
      [presented dismissViewControllerAnimated:YES completion:nil];
      return YES;
    }
    return NO;
  }

  if ([presented isKindOfClass:[UINavigationController class]]) {
    UINavigationController* nav = (UINavigationController*)presented;
    UIViewController* top = nav.topViewController;
    if ([top isKindOfClass:[XeniaLogViewController class]] &&
        [(XeniaLogViewController*)top handleControllerActions:actions]) {
      return YES;
    }
    if ([top isKindOfClass:[UITableViewController class]] &&
        [self handleControllerActionsForTableController:(UITableViewController*)top
                                                actions:actions]) {
      return YES;
    }
    if (actions.quick_action && TriggerBarButtonItem(top.navigationItem.rightBarButtonItem)) {
      return YES;
    }
    if (actions.back) {
      if (nav.viewControllers.count > 1) {
        [nav popViewControllerAnimated:YES];
      } else {
        [nav dismissViewControllerAnimated:YES completion:nil];
      }
      return YES;
    }
    return NO;
  }

  if (actions.back) {
    [presented dismissViewControllerAnimated:YES completion:nil];
    return YES;
  }
  return NO;
}

- (BOOL)handleLauncherControllerActions:(const xe::ui::apple::ControllerActionSet&)actions {
  if (![_host controllerNavigationLauncherVisible]) {
    return NO;
  }

  [self rebuildLauncherFocusGraph];
  IOSFocusNodeId current_focus = _launcherFocusGraph.current();
  NSInteger game_count = [_host controllerNavigationGameCount];
  BOOL handled = NO;
  BOOL focus_changed = NO;

  auto move_focus = [&](xe::ui::apple::NavigationDirection direction) {
    IOSFocusNodeId previous = _launcherFocusGraph.current();
    IOSFocusNodeId next = _launcherFocusGraph.Move(direction);
    if (next != previous) {
      focus_changed = YES;
    }
  };

  if (actions.section_prev) {
    IOSFocusNodeId target =
        current_focus == kLauncherFocusLibrary ? kLauncherFocusImport : kLauncherFocusSettings;
    if (_launcherFocusGraph.SetCurrent(target)) {
      focus_changed = YES;
    }
    handled = YES;
  }
  if (actions.section_next && game_count > 0) {
    if (_launcherFocusGraph.SetCurrent(kLauncherFocusLibrary)) {
      focus_changed = YES;
    }
    handled = YES;
  }

  current_focus = _launcherFocusGraph.current();
  if (current_focus == kLauncherFocusLibrary && game_count > 0) {
    NSInteger columns = std::max<NSInteger>(1, [_host controllerNavigationLauncherColumnCount]);
    NSInteger next_index = _focusedGameIndex < 0 ? 0 : _focusedGameIndex;

    if (actions.navigate_left) {
      if (next_index % columns == 0) {
        move_focus(xe::ui::apple::NavigationDirection::kLeft);
      } else if (next_index > 0) {
        next_index--;
      }
      handled = YES;
    }
    if (actions.navigate_right) {
      if (next_index + 1 < game_count) {
        next_index++;
      }
      handled = YES;
    }
    if (actions.navigate_up) {
      if (next_index - columns >= 0) {
        next_index -= columns;
      } else {
        move_focus(xe::ui::apple::NavigationDirection::kUp);
      }
      handled = YES;
    }
    if (actions.navigate_down) {
      if (next_index + columns < game_count) {
        next_index += columns;
      }
      handled = YES;
    }
    if (actions.page_prev) {
      NSInteger page_step = std::max<NSInteger>(1, [_host controllerNavigationLauncherPageStep]);
      next_index = std::max<NSInteger>(0, next_index - page_step);
      handled = YES;
    }
    if (actions.page_next) {
      NSInteger page_step = std::max<NSInteger>(1, [_host controllerNavigationLauncherPageStep]);
      next_index = std::min<NSInteger>(game_count - 1, next_index + page_step);
      handled = YES;
    }

    if (_launcherFocusGraph.current() == kLauncherFocusLibrary &&
        next_index != _focusedGameIndex) {
      [self setFocusedGameIndex:next_index scroll:YES];
      handled = YES;
    }
  } else {
    if (actions.navigate_up) {
      move_focus(xe::ui::apple::NavigationDirection::kUp);
      handled = YES;
    }
    if (actions.navigate_down) {
      move_focus(xe::ui::apple::NavigationDirection::kDown);
      handled = YES;
    }
    if (actions.navigate_left) {
      move_focus(xe::ui::apple::NavigationDirection::kLeft);
      handled = YES;
    }
    if (actions.navigate_right) {
      move_focus(xe::ui::apple::NavigationDirection::kRight);
      handled = YES;
    }
  }

  if (actions.context) {
    if (_launcherFocusGraph.current() == kLauncherFocusLibrary && _focusedGameIndex >= 0 &&
        _focusedGameIndex < game_count) {
      [_host controllerNavigationManageGameAtIndex:_focusedGameIndex];
    } else {
      [_host controllerNavigationOpenProfile];
    }
    handled = YES;
  }
  if (actions.quick_action) {
    [_host controllerNavigationImportGame];
    handled = YES;
  }
  if (actions.guide) {
    [_host controllerNavigationOpenSettings];
    handled = YES;
  }

  if (actions.accept) {
    switch (_launcherFocusGraph.current()) {
      case kLauncherFocusSettings:
        [_host controllerNavigationOpenSettings];
        handled = YES;
        break;
      case kLauncherFocusProfile:
        [_host controllerNavigationOpenProfile];
        handled = YES;
        break;
      case kLauncherFocusImport:
        [_host controllerNavigationImportGame];
        handled = YES;
        break;
      case kLauncherFocusLibrary:
        if (_focusedGameIndex >= 0 && _focusedGameIndex < game_count) {
          [_host controllerNavigationLaunchGameAtIndex:_focusedGameIndex];
          handled = YES;
        }
        break;
      default:
        break;
    }
  }

  if (focus_changed || handled) {
    [self applyLauncherFocusVisuals];
  }
  return handled;
}

- (BOOL)handleInGameControllerActions:(const xe::ui::apple::ControllerActionSet&)actions {
  if ([_host controllerNavigationLauncherVisible] || ![_host controllerNavigationGameRunning]) {
    return NO;
  }

  if (actions.guide) {
    if (![_host controllerNavigationInGameMenuVisible]) {
      [_host controllerNavigationShowInGameMenu];
    } else {
      [_host controllerNavigationHideInGameMenu];
    }
    return YES;
  }

  if (![_host controllerNavigationInGameMenuVisible]) {
    return NO;
  }

  if (actions.back) {
    [_host controllerNavigationHideInGameMenu];
    return YES;
  }

  [self rebuildInGameFocusGraph];
  if (_inGameFocusGraph.current() == xe::ui::apple::kInvalidFocusNodeId) {
    _inGameFocusGraph.SetCurrent(kInGameFocusResume);
  }

  BOOL handled = NO;
  BOOL focus_changed = NO;
  auto move_focus = [&](xe::ui::apple::NavigationDirection direction) {
    IOSFocusNodeId previous = _inGameFocusGraph.current();
    IOSFocusNodeId next = _inGameFocusGraph.Move(direction);
    if (next != previous) {
      focus_changed = YES;
    }
  };

  if (actions.navigate_up) {
    move_focus(xe::ui::apple::NavigationDirection::kUp);
    handled = YES;
  }
  if (actions.navigate_down) {
    move_focus(xe::ui::apple::NavigationDirection::kDown);
    handled = YES;
  }
  if (actions.navigate_left) {
    move_focus(xe::ui::apple::NavigationDirection::kLeft);
    handled = YES;
  }
  if (actions.navigate_right) {
    move_focus(xe::ui::apple::NavigationDirection::kRight);
    handled = YES;
  }

  if (actions.section_prev && _inGameFocusGraph.SetCurrent(kInGameFocusResume)) {
    focus_changed = YES;
    handled = YES;
  }
  if (actions.section_next && _inGameFocusGraph.SetCurrent(kInGameFocusExit)) {
    focus_changed = YES;
    handled = YES;
  }

  if (actions.context) {
    [_host controllerNavigationPerformInGameMenuAction:XeniaIOSInGameMenuActionSettings];
    handled = YES;
  }
  if (actions.quick_action) {
    [_host controllerNavigationPerformInGameMenuAction:XeniaIOSInGameMenuActionLiveLog];
    handled = YES;
  }

  if (actions.accept) {
    [_host controllerNavigationPerformInGameMenuAction:InGameActionForFocus(
                                                       _inGameFocusGraph.current())];
    handled = YES;
  }

  if (focus_changed || handled) {
    [self applyInGameMenuFocusVisuals];
  }
  return handled;
}

- (void)poll:(NSTimer* __unused)timer {
  const bool navigation_enabled = [_host controllerNavigationHasConnectedController];
  if (!navigation_enabled) {
    if (_navigationWasEnabled) {
      _navigationWasEnabled = NO;
      _mapper.Reset();
      _launcherFocusGraph.Clear();
      _inGameFocusGraph.Clear();
      [self applyLauncherFocusVisuals];
      [self applyInGameMenuFocusVisuals];
    }
    return;
  }

  if (!_navigationWasEnabled) {
    _navigationWasEnabled = YES;
    [self refreshLauncherFocus];
  }

  xe::hid::X_INPUT_STATE state = {};
  bool has_state = [_host controllerNavigationReadEmulatorControllerState:&state];
  if (!has_state) {
    has_state = [self readNativeControllerState:&state];
  }
  if (!has_state) {
    _mapper.Reset();
    return;
  }

  xe::ui::apple::ControllerActionSet actions = _mapper.Update(state, GetNowMs());
  if (!actions.Any()) {
    return;
  }

  if ([self handlePresentedControllerActions:actions]) {
    return;
  }
  if ([self handleLauncherControllerActions:actions]) {
    return;
  }
  [self handleInGameControllerActions:actions];
}

- (BOOL)readNativeControllerState:(xe::hid::X_INPUT_STATE*)out_state {
  if (!out_state) {
    return NO;
  }

  NSArray<GCController*>* controllers = [GCController controllers];
  for (GCController* controller in controllers) {
    GCExtendedGamepad* gamepad = controller.extendedGamepad;
    if (!gamepad) {
      continue;
    }

    uint16_t buttons = 0;
    auto set_button = [&buttons](BOOL pressed, uint16_t mask) {
      if (pressed) {
        buttons |= mask;
      }
    };

    set_button(gamepad.dpad.up.pressed, xe::hid::X_INPUT_GAMEPAD_DPAD_UP);
    set_button(gamepad.dpad.down.pressed, xe::hid::X_INPUT_GAMEPAD_DPAD_DOWN);
    set_button(gamepad.dpad.left.pressed, xe::hid::X_INPUT_GAMEPAD_DPAD_LEFT);
    set_button(gamepad.dpad.right.pressed, xe::hid::X_INPUT_GAMEPAD_DPAD_RIGHT);
    set_button(gamepad.buttonA.pressed, xe::hid::X_INPUT_GAMEPAD_A);
    set_button(gamepad.buttonB.pressed, xe::hid::X_INPUT_GAMEPAD_B);
    set_button(gamepad.buttonX.pressed, xe::hid::X_INPUT_GAMEPAD_X);
    set_button(gamepad.buttonY.pressed, xe::hid::X_INPUT_GAMEPAD_Y);
    set_button(gamepad.leftShoulder.pressed, xe::hid::X_INPUT_GAMEPAD_LEFT_SHOULDER);
    set_button(gamepad.rightShoulder.pressed, xe::hid::X_INPUT_GAMEPAD_RIGHT_SHOULDER);

    if (@available(iOS 13.0, tvOS 13.0, macCatalyst 13.0, *)) {
      set_button(gamepad.buttonMenu.pressed, xe::hid::X_INPUT_GAMEPAD_START);
    }
    if (@available(iOS 14.0, tvOS 14.0, macCatalyst 14.0, *)) {
      set_button(gamepad.buttonOptions.pressed, xe::hid::X_INPUT_GAMEPAD_BACK);
    }
    if (@available(iOS 12.1, tvOS 12.1, macCatalyst 13.1, *)) {
      set_button(gamepad.leftThumbstickButton.pressed, xe::hid::X_INPUT_GAMEPAD_LEFT_THUMB);
      set_button(gamepad.rightThumbstickButton.pressed, xe::hid::X_INPUT_GAMEPAD_RIGHT_THUMB);
    }

    out_state->packet_number = ++_nativeControllerPacketNumber;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = ToTriggerAxis(gamepad.leftTrigger.value);
    out_state->gamepad.right_trigger = ToTriggerAxis(gamepad.rightTrigger.value);
    out_state->gamepad.thumb_lx = ToThumbAxis(gamepad.leftThumbstick.xAxis.value);
    out_state->gamepad.thumb_ly = ToThumbAxis(gamepad.leftThumbstick.yAxis.value);
    out_state->gamepad.thumb_rx = ToThumbAxis(gamepad.rightThumbstick.xAxis.value);
    out_state->gamepad.thumb_ry = ToThumbAxis(gamepad.rightThumbstick.yAxis.value);
    return YES;
  }
  return NO;
}

@end
