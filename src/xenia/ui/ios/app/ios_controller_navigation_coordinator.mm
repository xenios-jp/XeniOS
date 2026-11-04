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
#include <vector>

#import "xenia/ui/ios/settings/ios_log_view_controller.h"
#import "xenia/ui/ios/shared/ios_alert_actions.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

using IOSFocusNodeId = xe::ui::apple::FocusNodeId;
// The Library's nodes are its focus targets, so a node is the target the overlay draws.
static constexpr IOSFocusNodeId kLauncherFocusSearch = XeniaIOSLauncherFocusTargetSearch;
static constexpr IOSFocusNodeId kLauncherFocusSort = XeniaIOSLauncherFocusTargetSort;
static constexpr IOSFocusNodeId kLauncherFocusOnline = XeniaIOSLauncherFocusTargetOnline;
static constexpr IOSFocusNodeId kLauncherFocusImport = XeniaIOSLauncherFocusTargetImport;
static constexpr IOSFocusNodeId kLauncherFocusSettings = XeniaIOSLauncherFocusTargetSettings;
static constexpr IOSFocusNodeId kLauncherFocusProfile = XeniaIOSLauncherFocusTargetProfile;
static constexpr IOSFocusNodeId kLauncherFocusHeroPlay = XeniaIOSLauncherFocusTargetHeroPlay;
static constexpr IOSFocusNodeId kLauncherFocusHeroMore = XeniaIOSLauncherFocusTargetHeroMore;
static constexpr IOSFocusNodeId kLauncherFocusLibrary = XeniaIOSLauncherFocusTargetLibrary;
static constexpr IOSFocusNodeId kInGameFocusResume = 101;
static constexpr IOSFocusNodeId kInGameFocusEdit = 102;
static constexpr IOSFocusNodeId kInGameFocusAchievements = 103;
static constexpr IOSFocusNodeId kInGameFocusDisplay = 104;
static constexpr IOSFocusNodeId kInGameFocusSettings = 105;
static constexpr IOSFocusNodeId kInGameFocusLog = 106;
static constexpr IOSFocusNodeId kInGameFocusExit = 107;
static constexpr IOSFocusNodeId kInGameFocusGraphics = 108;
static constexpr IOSFocusNodeId kInGameFocusPauseEmulator = 109;

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
    case kInGameFocusPauseEmulator:
      return XeniaIOSInGameMenuActionPauseEmulator;
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

// B on a sheet, as a swipe down would: the presentation delegate may refuse,
// and hears about the dismissal, so whoever waits on the sheet is told.
void DismissLikeSwipe(UIViewController* presented) {
  // A sheet that can't be swiped away (Settings, Profiles) closes through its own Close button,
  // which saves what it has to first.
  if (presented.modalInPresentation) {
    UIViewController* top = [presented isKindOfClass:[UINavigationController class]]
                                ? ((UINavigationController*)presented).topViewController
                                : presented;
    if (TriggerBarButtonItem(top.navigationItem.leftBarButtonItem) ||
        TriggerBarButtonItem(top.navigationItem.rightBarButtonItem)) {
      return;
    }
  }
  UIPresentationController* presentation = presented.presentationController;
  id<UIAdaptivePresentationControllerDelegate> delegate = presentation.delegate;
  if (presented.modalInPresentation ||
      ([delegate respondsToSelector:@selector(presentationControllerShouldDismiss:)] &&
       ![delegate presentationControllerShouldDismiss:presentation])) {
    if ([delegate respondsToSelector:@selector(presentationControllerDidAttemptToDismiss:)]) {
      [delegate presentationControllerDidAttemptToDismiss:presentation];
    }
    return;
  }
  if ([delegate respondsToSelector:@selector(presentationControllerWillDismiss:)]) {
    [delegate presentationControllerWillDismiss:presentation];
  }
  [presented dismissViewControllerAnimated:YES
                                completion:^{
                                  if ([delegate respondsToSelector:@selector(
                                                    presentationControllerDidDismiss:)]) {
                                    [delegate presentationControllerDidDismiss:presentation];
                                  }
                                }];
}

// The table a page is built around: the page's own, or the largest one on screen in it (a plain
// view controller holding a table, or a split view's detail).
UITableView* PrimaryTableViewForController(UIViewController* controller) {
  if ([controller isKindOfClass:[UITableViewController class]]) {
    return ((UITableViewController*)controller).tableView;
  }
  if (!controller.isViewLoaded) {
    return nil;
  }
  UITableView* best = nil;
  CGFloat best_area = 0.0;
  NSMutableArray<UIView*>* queue = [NSMutableArray arrayWithObject:controller.view];
  while (queue.count) {
    UIView* view = queue.firstObject;
    [queue removeObjectAtIndex:0];
    if (view.hidden || view.alpha < 0.01) {
      continue;
    }
    if ([view isKindOfClass:[UITableView class]] && view.window) {
      const CGFloat area = CGRectGetWidth(view.bounds) * CGRectGetHeight(view.bounds);
      if (area > best_area) {
        best = (UITableView*)view;
        best_area = area;
      }
      continue;
    }
    [queue addObjectsFromArray:view.subviews];
  }
  return best;
}

// A control of |control_class| in the row: in its accessory or anywhere in its content.
UIControl* RowControlOfClass(UITableViewCell* cell, Class control_class) {
  NSMutableArray<UIView*>* queue = [NSMutableArray array];
  if (cell.accessoryView) {
    [queue addObject:cell.accessoryView];
  }
  [queue addObject:cell.contentView];
  while (queue.count) {
    UIView* view = queue.firstObject;
    [queue removeObjectAtIndex:0];
    if ([view isKindOfClass:control_class] && !view.hidden) {
      return (UIControl*)view;
    }
    [queue addObjectsFromArray:view.subviews];
  }
  return nil;
}

// The row under the controller's cursor is ringed, since toggle, slider and field rows don't
// show a selection, and a tap on a row clears it.
constexpr NSInteger kControllerCursorRingTag = 0x58430001;

// Round the whole row: from its content to where its divider ends, so the value and chevron on the
// trailing side are inside the ring.
CGRect CursorRingFrame(UITableView* table_view, NSIndexPath* path) {
  UITableViewCell* cell = [table_view cellForRowAtIndexPath:path];
  CGRect frame = [table_view rectForRowAtIndexPath:path];
  if (cell) {
    const CGRect content = [cell.contentView convertRect:cell.contentView.bounds
                                                  toView:table_view];
    const CGRect row = [cell convertRect:cell.bounds toView:table_view];
    const CGFloat right = MAX(CGRectGetMaxX(content),
                              CGRectGetMaxX(row) - table_view.separatorInset.right);
    frame = CGRectMake(CGRectGetMinX(content), CGRectGetMinY(content),
                       right - CGRectGetMinX(content), CGRectGetHeight(content));
  }
  return CGRectInset(frame, 2.0, 1.0);
}

void ShowCursorRing(UITableView* table_view, NSIndexPath* path) {
  UIView* ring = [table_view viewWithTag:kControllerCursorRingTag];
  if (!ring) {
    ring = [[[UIView alloc] init] autorelease];
    ring.tag = kControllerCursorRingTag;
    ring.userInteractionEnabled = NO;
    ring.layer.borderWidth = 2.5;
    ring.layer.cornerRadius = 12.0;
    ring.layer.cornerCurve = kCACornerCurveContinuous;
    [table_view addSubview:ring];
  }
  ring.layer.borderColor = [XeniaTheme accent].CGColor;
  ring.frame = CursorRingFrame(table_view, path);
  ring.hidden = NO;
  [table_view bringSubviewToFront:ring];
}

// Rows that expand, collapse or reload move under the ring after the press that drew it, so it is
// placed again from the cursor on every tick, and hidden when the cursor row is gone.
void FollowCursorRing(UITableView* table_view, NSIndexPath* path) {
  UIView* ring = [table_view viewWithTag:kControllerCursorRingTag];
  if (!ring || ring.hidden) {
    return;
  }
  if (!path || path.section >= table_view.numberOfSections ||
      path.row >= [table_view numberOfRowsInSection:path.section]) {
    ring.hidden = YES;
    return;
  }
  const CGRect frame = CursorRingFrame(table_view, path);
  if (!CGRectEqualToRect(ring.frame, frame)) {
    ring.frame = frame;
  }
}

}  // namespace

@implementation XeniaIOSControllerNavigationCoordinator {
  id<XeniaIOSControllerNavigationHost> _host;
  NSTimer* _timer;
  BOOL _started;
  xe::ui::apple::ControllerNavigationMapper _mapper;
  xe::ui::apple::FocusGraph _launcherFocusGraph;
  xe::ui::apple::FocusGraph _inGameFocusGraph;
  NSInteger _focusedGameIndex;
  BOOL _launcherLibraryFocusActive;
  BOOL _navigationWasEnabled;
  // The Library's focus ring waits for the controller's first press, so a connected but idle
  // controller doesn't leave a ring on a tile.
  BOOL _launcherFocusShown;
  // The header button the games' top row goes up to: the one used last, Import at first.
  IOSFocusNodeId _lastHeaderFocus;
  uint32_t _nativeControllerPacketNumber;
  // The cursor row of each table the controller has moved through.
  NSMapTable<UITableView*, NSIndexPath*>* _tableCursors;
  // The table whose cursor ring is showing, held weakly.
  NSHashTable<UITableView*>* _ringTable;
}

- (instancetype)initWithHost:(id<XeniaIOSControllerNavigationHost>)host {
  if (!(self = [super init])) {
    return nil;
  }
  _host = host;
  _focusedGameIndex = -1;
  _launcherLibraryFocusActive = NO;
  _lastHeaderFocus = kLauncherFocusImport;
  _navigationWasEnabled = NO;
  _nativeControllerPacketNumber = 0;
  _tableCursors = [[NSMapTable weakToStrongObjectsMapTable] retain];
  _ringTable = [[NSHashTable weakObjectsHashTable] retain];
  _mapper.Reset();
  return self;
}

- (void)dealloc {
  [self invalidate];
  [_tableCursors release];
  [_ringTable release];
  [super dealloc];
}

- (NSInteger)focusedGameIndex {
  return _focusedGameIndex;
}

- (void)start {
  if (_started) {
    return;
  }
  _started = YES;
  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [center addObserver:self
             selector:@selector(controllersChanged:)
                 name:GCControllerDidConnectNotification
               object:nil];
  [center addObserver:self
             selector:@selector(controllersChanged:)
                 name:GCControllerDidDisconnectNotification
               object:nil];
  [self updatePolling];
}

- (void)invalidate {
  if (_started) {
    _started = NO;
    [[NSNotificationCenter defaultCenter] removeObserver:self];
  }
  [self stopTimer];
}

- (void)controllersChanged:(NSNotification* __unused)note {
  [self updatePolling];
}

// Polls at 60 Hz only while a controller is connected, so touch-only use
// doesn't wake the app every frame.
- (void)updatePolling {
  if (_started && [_host controllerNavigationHasConnectedController]) {
    if (!_timer) {
      _timer = [[NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                                target:self
                                              selector:@selector(poll:)
                                              userInfo:nil
                                               repeats:YES] retain];
      _timer.tolerance = 0.01;
    }
    return;
  }
  if (_timer) {
    [self stopTimer];
    // Once more, to clear the focus the last controller left.
    [self poll:nil];
  }
}

- (void)stopTimer {
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

// The header's row, left to right, as it is laid out now: beside the art, Search and Sort lead it;
// upright they sit in their own row under Continue Playing.
- (std::vector<IOSFocusNodeId>)launcherHeaderRow {
  std::vector<IOSFocusNodeId> row;
  if ([_host controllerNavigationLauncherSideLayout] &&
      [_host controllerNavigationLauncherShowsSearch]) {
    row = {kLauncherFocusSearch, kLauncherFocusSort};
  }
  row.insert(row.end(),
             {kLauncherFocusOnline, kLauncherFocusImport, kLauncherFocusSettings,
              kLauncherFocusProfile});
  return row;
}

- (void)rebuildLauncherFocusGraph {
  IOSFocusNodeId previous_focus = _launcherFocusGraph.current();
  _launcherFocusGraph.Clear();

  const BOOL launcher_visible = [_host controllerNavigationLauncherVisible];
  const BOOL actions_enabled =
      launcher_visible && [_host controllerNavigationLauncherActionsEnabled];
  const BOOL side = [_host controllerNavigationLauncherSideLayout];
  const BOOL hero = actions_enabled && [_host controllerNavigationLauncherShowsHero];
  const BOOL search = actions_enabled && [_host controllerNavigationLauncherShowsSearch];
  const BOOL games = [_host controllerNavigationGameCount] > 0;
  const IOSFocusNodeId none = xe::ui::apple::kInvalidFocusNodeId;

  const std::vector<IOSFocusNodeId> header = [self launcherHeaderRow];
  if (std::find(header.begin(), header.end(), _lastHeaderFocus) == header.end()) {
    _lastHeaderFocus = kLauncherFocusImport;
  }
  // Upright, from the top down: header, Continue Playing, Search and Sort, the games. Beside the
  // art, the header runs across the top, the games under it, Continue Playing to their left.
  const IOSFocusNodeId games_node = games ? kLauncherFocusLibrary : none;
  const IOSFocusNodeId search_node = search && !side ? kLauncherFocusSearch : none;
  const IOSFocusNodeId hero_node = hero ? kLauncherFocusHeroPlay : none;
  IOSFocusNodeId under_header = side ? (games ? games_node : hero_node)
                                     : (hero ? hero_node : search ? search_node : games_node);

  auto add = [&](IOSFocusNodeId id, IOSFocusNodeId up, IOSFocusNodeId down, IOSFocusNodeId left,
                 IOSFocusNodeId right, bool enabled) {
    xe::ui::apple::FocusNode node;
    node.id = id;
    node.up = up;
    node.down = down;
    node.left = left;
    node.right = right;
    node.enabled = enabled;
    _launcherFocusGraph.AddOrUpdateNode(node);
  };

  for (size_t i = 0; i < header.size(); ++i) {
    add(header[i], none, under_header, i > 0 ? header[i - 1] : none,
        i + 1 < header.size() ? header[i + 1] : none, actions_enabled);
  }
  if (hero) {
    const IOSFocusNodeId hero_down = side ? none : (search ? search_node : games_node);
    add(kLauncherFocusHeroPlay, _lastHeaderFocus, hero_down, none, kLauncherFocusHeroMore, true);
    add(kLauncherFocusHeroMore, _lastHeaderFocus, hero_down, kLauncherFocusHeroPlay,
        side ? games_node : none, true);
  }
  if (search && !side) {
    const IOSFocusNodeId search_up = hero ? hero_node : _lastHeaderFocus;
    add(kLauncherFocusSearch, search_up, games_node, none, kLauncherFocusSort, true);
    add(kLauncherFocusSort, search_up, games_node, kLauncherFocusSearch, none, true);
  }
  IOSFocusNodeId games_up =
      side ? _lastHeaderFocus : (search ? search_node : hero ? hero_node : _lastHeaderFocus);
  add(kLauncherFocusLibrary, games_up, none, side && hero ? kLauncherFocusHeroMore : none, none,
      games);

  if (previous_focus != none && _launcherFocusGraph.SetCurrent(previous_focus)) {
    return;
  }
  // A controller starts on the games, not a button; one whose button went away goes there too.
  if (!_launcherFocusGraph.SetCurrent(kLauncherFocusLibrary)) {
    _launcherFocusGraph.SetCurrent(kLauncherFocusImport);
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

  // Row 1: Edit Controls, Settings, Quit
  xe::ui::apple::FocusNode edit_controls;
  edit_controls.id = kInGameFocusEdit;
  edit_controls.up = kInGameFocusResume;
  edit_controls.down = kInGameFocusGraphics;
  edit_controls.right = kInGameFocusSettings;
  edit_controls.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                     XeniaIOSInGameMenuActionEditControls];

  xe::ui::apple::FocusNode settings;
  settings.id = kInGameFocusSettings;
  settings.up = kInGameFocusResume;
  settings.down = kInGameFocusDisplay;
  settings.left = kInGameFocusEdit;
  settings.right = kInGameFocusExit;
  settings.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                XeniaIOSInGameMenuActionSettings];

  xe::ui::apple::FocusNode exit_node;
  exit_node.id = kInGameFocusExit;
  exit_node.up = kInGameFocusResume;
  exit_node.down = kInGameFocusAchievements;
  exit_node.left = kInGameFocusSettings;
  exit_node.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                 XeniaIOSInGameMenuActionExit];

  // Row 2: Graphics, Display, Achievements
  xe::ui::apple::FocusNode graphics;
  graphics.id = kInGameFocusGraphics;
  graphics.up = kInGameFocusEdit;
  graphics.down = kInGameFocusPauseEmulator;
  graphics.right = kInGameFocusDisplay;
  graphics.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                XeniaIOSInGameMenuActionGraphics];

  xe::ui::apple::FocusNode display_node;
  display_node.id = kInGameFocusDisplay;
  display_node.up = kInGameFocusSettings;
  display_node.down = kInGameFocusLog;
  display_node.left = kInGameFocusGraphics;
  display_node.right = kInGameFocusAchievements;
  display_node.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                    XeniaIOSInGameMenuActionDisplay];

  xe::ui::apple::FocusNode achievements;
  achievements.id = kInGameFocusAchievements;
  achievements.up = kInGameFocusExit;
  achievements.down = kInGameFocusLog;
  achievements.left = kInGameFocusDisplay;
  achievements.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                     XeniaIOSInGameMenuActionAchievements];

  // Footer: Pause Emulator, Live Log
  xe::ui::apple::FocusNode pause_emulator;
  pause_emulator.id = kInGameFocusPauseEmulator;
  pause_emulator.up = kInGameFocusGraphics;
  pause_emulator.down = kInGameFocusResume;
  pause_emulator.right = kInGameFocusLog;
  pause_emulator.enabled = [_host controllerNavigationInGameMenuActionEnabled:
                                      XeniaIOSInGameMenuActionPauseEmulator];

  xe::ui::apple::FocusNode log;
  log.id = kInGameFocusLog;
  log.up = kInGameFocusAchievements;
  log.left = kInGameFocusPauseEmulator;
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
  _inGameFocusGraph.AddOrUpdateNode(pause_emulator);
  _inGameFocusGraph.AddOrUpdateNode(log);

  if (previous_focus != xe::ui::apple::kInvalidFocusNodeId) {
    _inGameFocusGraph.SetCurrent(previous_focus);
  }
}

- (void)applyLauncherFocusVisuals {
  if (!_navigationWasEnabled || !_launcherFocusShown) {
    _launcherLibraryFocusActive = NO;
    [_host controllerNavigationApplyLauncherFocusEnabled:NO
                                             focusTarget:XeniaIOSLauncherFocusTargetNone];
    return;
  }

  IOSFocusNodeId current_focus = _launcherFocusGraph.current();
  BOOL library_focused = current_focus == kLauncherFocusLibrary;
  const std::vector<IOSFocusNodeId> header = [self launcherHeaderRow];
  if (std::find(header.begin(), header.end(), current_focus) != header.end()) {
    _lastHeaderFocus = current_focus;
  }

  if (library_focused && _focusedGameIndex < 0 &&
      [_host controllerNavigationGameCount] > 0) {
    [self setFocusedGameIndex:0 scroll:NO];
  }

  _launcherLibraryFocusActive = library_focused;
  [_host controllerNavigationApplyLauncherFocusEnabled:YES
                                          focusTarget:static_cast<XeniaIOSLauncherFocusTarget>(
                                                          current_focus)];
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

- (BOOL)handleControllerActionsForTableController:(UIViewController*)table_controller
                                          actions:
                                              (const xe::ui::apple::ControllerActionSet&)actions {
  UITableView* table_view = PrimaryTableViewForController(table_controller);
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

  // The cursor is kept here, per table, rather than read back from the selection: tapping a row
  // clears its selection, and the cursor stays where it was.
  NSIndexPath* selected = [_tableCursors objectForKey:table_view];
  if (selected && [all_paths indexOfObject:selected] == NSNotFound) {
    selected = nil;
  }
  NSInteger selected_index = 0;
  if (selected) {
    NSUInteger found = [all_paths indexOfObject:selected];
    if (found != NSNotFound) {
      selected_index = static_cast<NSInteger>(found);
    }
  } else {
    selected = all_paths.firstObject;
    [table_view scrollToRowAtIndexPath:selected
                      atScrollPosition:UITableViewScrollPositionNone
                              animated:NO];
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

  if (selected) {
    [_tableCursors setObject:selected forKey:table_view];
    if (handled) {
      [table_view scrollToRowAtIndexPath:selected
                        atScrollPosition:UITableViewScrollPositionMiddle
                                animated:YES];
    }
    [table_view layoutIfNeeded];
    ShowCursorRing(table_view, selected);
    [_ringTable removeAllObjects];
    [_ringTable addObject:table_view];
  }

  // Left and right change a slider or stepper in the row by one step.
  if ((actions.navigate_left || actions.navigate_right) && selected) {
    UITableViewCell* cell = [table_view cellForRowAtIndexPath:selected];
    const double direction = actions.navigate_right ? 1.0 : -1.0;
    if (UIStepper* stepper = (UIStepper*)RowControlOfClass(cell, [UIStepper class])) {
      const double value = std::min(stepper.maximumValue,
                                    std::max(stepper.minimumValue,
                                             stepper.value + direction * stepper.stepValue));
      if (value != stepper.value) {
        stepper.value = value;
        [stepper sendActionsForControlEvents:UIControlEventValueChanged];
      }
      handled = YES;
    } else if (UISlider* slider = (UISlider*)RowControlOfClass(cell, [UISlider class])) {
      const float step = (slider.maximumValue - slider.minimumValue) / 20.0f;
      [slider setValue:slider.value + static_cast<float>(direction) * step animated:YES];
      [slider sendActionsForControlEvents:UIControlEventValueChanged];
      [slider sendActionsForControlEvents:UIControlEventTouchUpInside];
      handled = YES;
    }
  }

  if (actions.accept && selected) {
    UITableViewCell* cell = [table_view cellForRowAtIndexPath:selected];
    UISwitch* toggle = (UISwitch*)RowControlOfClass(cell, [UISwitch class]);
    if (toggle && toggle.enabled) {
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
      DismissLikeSwipe(nav ? nav : table_controller);
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
    // Answered through the actions' handlers, as a tap is: a guest prompt
    // waits on them, and a bare dismiss would leave it waiting forever.
    UIAlertController* alert = (UIAlertController*)presented;
    if (actions.back) {
      XeniaAlertCancel(alert);
      return YES;
    }
    if (actions.accept) {
      XeniaAlertAccept(alert);
      return YES;
    }
    if (actions.navigate_up || actions.navigate_left) {
      XeniaAlertMoveSelection(alert, -1);
      return YES;
    }
    if (actions.navigate_down || actions.navigate_right) {
      XeniaAlertMoveSelection(alert, 1);
      return YES;
    }
    // Nothing behind an alert answers while it is up.
    return YES;
  }

  if ([presented isKindOfClass:[UINavigationController class]]) {
    UINavigationController* nav = (UINavigationController*)presented;
    UIViewController* top = nav.topViewController;
    if ([top isKindOfClass:[XeniaLogViewController class]] &&
        [(XeniaLogViewController*)top handleControllerActions:actions]) {
      return YES;
    }
    if (actions.back) {
      if (nav.viewControllers.count > 1) {
        [nav popViewControllerAnimated:YES];
      } else {
        DismissLikeSwipe(nav);
      }
      return YES;
    }
    if ([self handleControllerActionsForTableController:top actions:actions]) {
      return YES;
    }
    if (actions.quick_action) {
      TriggerBarButtonItem(top.navigationItem.rightBarButtonItem);
    }
    // A sheet takes every press: none reaches the Library or game behind it.
    return YES;
  }

  if (actions.back) {
    DismissLikeSwipe(presented);
    return YES;
  }
  [self handleControllerActionsForTableController:presented actions:actions];
  return YES;
}

- (BOOL)handleLauncherControllerActions:(const xe::ui::apple::ControllerActionSet&)actions {
  if (![_host controllerNavigationLauncherVisible]) {
    return NO;
  }
  // A game's page in place of the grid: its rows move and open with A, Y plays, B closes it.
  if (UIViewController* page = [_host controllerNavigationGameDetailController]) {
    if (actions.back) {
      [_host controllerNavigationCloseGameDetail];
    } else if (actions.quick_action) {
      [_host controllerNavigationPlayGameDetail];
    } else {
      [self handleControllerActionsForTableController:page actions:actions];
    }
    return YES;
  }
  // Typing in Search: A or B ends it, back on Search; the rest is the keyboard's.
  if ([_host controllerNavigationLauncherSearchEditing]) {
    if (actions.accept || actions.back) {
      [_host controllerNavigationEndLauncherSearch];
    }
    return YES;
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
        current_focus == kLauncherFocusLibrary ? _lastHeaderFocus : kLauncherFocusImport;
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
      case kLauncherFocusSearch:
      case kLauncherFocusSort:
      case kLauncherFocusOnline:
      case kLauncherFocusHeroPlay:
      case kLauncherFocusHeroMore:
        [_host controllerNavigationActivateLauncherTarget:static_cast<XeniaIOSLauncherFocusTarget>(
                                                              _launcherFocusGraph.current())];
        handled = YES;
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
      _launcherFocusShown = NO;
      _mapper.Reset();
      _launcherFocusGraph.Clear();
      _inGameFocusGraph.Clear();
      [self applyLauncherFocusVisuals];
      [self applyInGameMenuFocusVisuals];
    }
    return;
  }

  if (UITableView* ring_table = _ringTable.anyObject) {
    FollowCursorRing(ring_table, [_tableCursors objectForKey:ring_table]);
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
  // The first press on the Library shows where focus is rather than acting on it.
  if (!_launcherFocusShown && [_host controllerNavigationLauncherVisible] &&
      ![_host controllerNavigationInGameMenuVisible] &&
      ![_host controllerNavigationPresentedController] &&
      ![_host controllerNavigationGameDetailController]) {
    _launcherFocusShown = YES;
    [self applyLauncherFocusVisuals];
    return;
  }
  _launcherFocusShown = YES;

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

    set_button(gamepad.buttonMenu.pressed, xe::hid::X_INPUT_GAMEPAD_START);
    // Optional on a gamepad: nil, so never pressed, where it is missing.
    set_button(gamepad.buttonOptions.pressed, xe::hid::X_INPUT_GAMEPAD_BACK);
    set_button(gamepad.leftThumbstickButton.pressed, xe::hid::X_INPUT_GAMEPAD_LEFT_THUMB);
    set_button(gamepad.rightThumbstickButton.pressed, xe::hid::X_INPUT_GAMEPAD_RIGHT_THUMB);

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
