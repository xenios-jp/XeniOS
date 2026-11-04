/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_OVERLAY_VIEW_H_
#define XENIA_UI_IOS_LAUNCHER_OVERLAY_VIEW_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

// Lightweight snapshot the root controller builds from its IOSDiscoveredGame
// vector. The launcher view never imports files or launches games directly.
// A controller's focus in the Library, other than a game.
typedef NS_ENUM(NSInteger, XeniaIOSLauncherFocusTarget) {
  XeniaIOSLauncherFocusTargetNone = 0,
  XeniaIOSLauncherFocusTargetSearch,
  XeniaIOSLauncherFocusTargetSort,
  XeniaIOSLauncherFocusTargetOnline,
  XeniaIOSLauncherFocusTargetImport,
  XeniaIOSLauncherFocusTargetSettings,
  XeniaIOSLauncherFocusTargetProfile,
  XeniaIOSLauncherFocusTargetHeroPlay,
  XeniaIOSLauncherFocusTargetHeroMore,
  XeniaIOSLauncherFocusTargetLibrary,
};

@interface XeniaIOSLauncherGameSnapshot : NSObject
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* contentTypeName;
@property(nonatomic, copy) NSString* compatStatus;
@property(nonatomic, assign) BOOL hasCompatInfo;
@property(nonatomic, copy) NSString* compatPerf;
@property(nonatomic, copy) NSString* compatNotes;
@property(nonatomic, assign) uint32_t titleId;
@property(nonatomic, assign) BOOL supportsCompatibility;
@property(nonatomic, assign) BOOL supportsManageContent;
@property(nonatomic, assign) BOOL supportsDiscSelection;
@property(nonatomic, assign) BOOL supportsPatches;
@property(nonatomic, assign) BOOL supportsZarConversion;
@property(nonatomic, assign) BOOL supportsRemoteArt;
@property(nonatomic, assign) NSUInteger discCount;
@property(nonatomic, strong) NSData* iconData;
// Filled in by the launcher view: the decoded `iconData`, decoded once off the
// main thread and carried across refreshes. The root controller leaves it nil.
@property(nonatomic, strong) UIImage* decodedIcon;
// When the game was last started from XeniOS (XeniaIOSLastPlayedDate), or nil.
@property(nonatomic, copy) NSDate* lastPlayed;
@end

// When a game was last started, kept per title id in the user defaults. The
// most recent one leads the library as Continue Playing.
void XeniaIOSRecordGamePlayed(uint32_t title_id);
NSDate* XeniaIOSLastPlayedDate(uint32_t title_id);

// Replaces the launcher overlay previously constructed inline in
// XeniaViewController. Owns the nav bar, JIT status card,
// game collection view, and the (detached) status label. Communicates with the root
// controller exclusively through callback blocks and typed data snapshots.
@interface XeniaIOSLauncherOverlayView : UIView

#pragma mark - Callbacks (set by the root controller)

@property(nonatomic, copy) void (^settingsHandler)(void);
@property(nonatomic, copy) void (^profileHandler)(void);
@property(nonatomic, copy) void (^importHandler)(void);
// Back the Xbox 360 Dashboard's tile, which leads the library while it has
// games (an empty library offers the dashboard beside Import instead). The
// tile starts the dashboard, or sets it up; its menu also opens the system
// software page.
@property(nonatomic, copy) void (^dashboardHandler)(void);
@property(nonatomic, copy) void (^dashboardSystemSoftwareHandler)(void);
// The pull-down menu behind +: ways to add games, and converting the library to ZAR.
@property(nonatomic, retain) UIMenu* addMenu;
// Backs the "Open StikDebug" button on the JIT card. The button is hidden
// while this is nil.
@property(nonatomic, copy) void (^openStikDebugHandler)(void);
@property(nonatomic, copy) void (^gameLaunchedHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^copyLaunchURLHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^gameSettingsHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^touchLayoutHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^compatibilityHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^manageContentHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^discSelectionHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^patchesHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^zarConversionHandler)(NSUInteger gameIndex);
// The notice suggesting the library be converted to ZAR: Convert, and its close button.
@property(nonatomic, copy) void (^zarSuggestionHandler)(void);
@property(nonatomic, copy) void (^zarSuggestionDismissHandler)(void);
// The ⋯ beside Play on Continue Playing: the game's actions.
@property(nonatomic, copy) void (^gameActionsHandler)(NSUInteger gameIndex);

#pragma mark - Data ingestion

- (void)setGames:(NSArray<XeniaIOSLauncherGameSnapshot*>*)games;
- (void)setJITAcquired:(BOOL)acquired;
- (void)setDashboardInstalled:(BOOL)installed;
- (void)setJITStatusText:(NSString*)text;
- (void)setMemoryEntitlementEnabled:(BOOL)enabled;
- (void)setMemoryEntitlementStatusText:(NSString*)text;
// The ZAR suggestion's line, as "Save 18 GB" and what it takes; nil hides it.
- (void)setZarSuggestionTitle:(NSString*)title detail:(NSString*)detail;
// The focus index counts the grid's items, the Dashboard tile included; the
// game handlers above get indexes into the games.
- (void)setFocusedGameIndex:(NSInteger)index scroll:(BOOL)scroll;
- (NSInteger)focusedGameIndex;
- (void)reloadGames;
// Search and sort decide which games the grid shows and in what order, so the
// grid's items map to game indexes through these. -1 when there is none (the
// Dashboard tile, past the end, or a game the search hides).
- (NSInteger)libraryItemCount;
- (BOOL)isDashboardItem:(NSInteger)item;
- (NSInteger)gameIndexForItem:(NSInteger)item;
- (NSInteger)itemForGameIndex:(NSInteger)gameIndex;
@property(nonatomic, assign, getter=isActionsEnabled) BOOL actionsEnabled;

#pragma mark - Focus visual (called by root when controller nav state changes)

// What a controller's focus is on: one of the Library's buttons, or the games.
- (void)setControllerNavigationEnabled:(BOOL)enabled
                           focusTarget:(XeniaIOSLauncherFocusTarget)target;
// Presses the focused button: Search starts typing, Online and the hero's buttons act as if
// tapped. Sort's menu can't be opened from code, so it returns the same choices as an action sheet
// for the caller to present; everything else returns nil.
- (UIAlertController*)activateControllerFocusTarget:(XeniaIOSLauncherFocusTarget)target;
// Whether Continue Playing (Play and ⋯) and the Search row are on screen to be focused, and
// whether the art sits beside the games (landscape) rather than above them.
@property(nonatomic, readonly) BOOL controllerShowsHero;
@property(nonatomic, readonly) BOOL controllerShowsSearch;
@property(nonatomic, readonly) BOOL controllerSideLayout;
// Typing in Search: B or A ends it.
@property(nonatomic, readonly) BOOL controllerSearchEditing;
- (void)endControllerSearch;
// A game's page: Play, the round buttons, ⋯ and the compatibility capsule, in order, as shown.
@property(nonatomic, readonly) NSUInteger controllerDetailButtonCount;
// Rings the button at the index, or none for NSNotFound.
- (void)setControllerDetailButtonFocus:(NSUInteger)index;
// Presses it; a button with a menu returns the menu as an action sheet to present instead.
- (UIAlertController*)activateControllerDetailButton:(NSUInteger)index;

#pragma mark - Read-only accessors for root layout/state queries

@property(nonatomic, readonly) UICollectionView* gamesCollectionView;
@property(nonatomic, readonly) BOOL isOverlayVisible;
- (NSInteger)columnCount;
- (NSInteger)pageStep;
- (void)refreshChromeForCurrentTraits;

#pragma mark - Visibility

- (void)setOverlayVisible:(BOOL)visible animated:(BOOL)animated;

// A phone on its side opens a game's page in place: the art on the left turns to that game, Play
// gains touch layout and settings beside it, and the game's rows take the grid's place.
@property(nonatomic, readonly) BOOL canShowGameDetailInPlace;
@property(nonatomic, readonly) BOOL showsGameDetail;
// Runs once the rows have left, so their owner can let them go.
@property(nonatomic, copy) void (^gameDetailClosedHandler)(void);
- (void)showGameDetailForGameIndex:(NSUInteger)gameIndex
                          rowsView:(UIView*)rowsView
                          moreMenu:(UIMenu*)moreMenu;
- (void)hideGameDetailAnimated:(BOOL)animated;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_OVERLAY_VIEW_H_
