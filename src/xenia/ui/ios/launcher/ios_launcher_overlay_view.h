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
@interface XeniaIOSLauncherGameSnapshot : NSObject
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* contentTypeName;
@property(nonatomic, copy) NSString* compatStatus;
@property(nonatomic, assign) BOOL hasCompatInfo;
@property(nonatomic, assign) uint32_t titleId;
@property(nonatomic, assign) BOOL supportsCompatibility;
@property(nonatomic, assign) BOOL supportsManageContent;
@property(nonatomic, assign) BOOL supportsDiscSelection;
@property(nonatomic, assign) BOOL supportsPatches;
@property(nonatomic, assign) BOOL supportsZarConversion;
@property(nonatomic, assign) BOOL supportsRemoteArt;
@property(nonatomic, assign) NSUInteger discCount;
@property(nonatomic, strong) NSData* iconData;
@end

// Replaces the launcher overlay previously constructed inline in
// XeniaViewController. Owns the nav bar, JIT status indicators,
// game collection view, and status label. Communicates with the root
// controller exclusively through callback blocks and typed data snapshots.
@interface XeniaIOSLauncherOverlayView : UIView

#pragma mark - Callbacks (set by the root controller)

@property(nonatomic, copy) void (^settingsHandler)(void);
@property(nonatomic, copy) void (^profileHandler)(void);
@property(nonatomic, copy) void (^importHandler)(void);
@property(nonatomic, copy) void (^bulkZarConversionHandler)(void);
@property(nonatomic, copy) void (^gameLaunchedHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^copyLaunchURLHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^gameSettingsHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^touchLayoutHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^compatibilityHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^manageContentHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^discSelectionHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^patchesHandler)(NSUInteger gameIndex);
@property(nonatomic, copy) void (^zarConversionHandler)(NSUInteger gameIndex);

#pragma mark - Data ingestion

- (void)setGames:(NSArray<XeniaIOSLauncherGameSnapshot*>*)games;
- (void)setJITAcquired:(BOOL)acquired;
- (void)setJITStatusText:(NSString*)text;
- (void)setMemoryEntitlementEnabled:(BOOL)enabled;
- (void)setMemoryEntitlementStatusText:(NSString*)text;
- (void)setFocusedGameIndex:(NSInteger)index scroll:(BOOL)scroll;
- (NSInteger)focusedGameIndex;
- (void)reloadGames;
@property(nonatomic, assign, getter=isActionsEnabled) BOOL actionsEnabled;

#pragma mark - Focus visual (called by root when controller nav state changes)

- (void)setControllerNavigationEnabled:(BOOL)enabled
                       settingsFocused:(BOOL)settingsFocused
                        profileFocused:(BOOL)profileFocused
                         importFocused:(BOOL)importFocused
                    libraryFocusActive:(BOOL)libraryFocusActive;

#pragma mark - Read-only accessors for root layout/state queries

@property(nonatomic, readonly) UILabel* statusLabel;
@property(nonatomic, readonly) UICollectionView* gamesCollectionView;
@property(nonatomic, readonly) BOOL isOverlayVisible;
- (NSInteger)columnCount;
- (NSInteger)pageStep;
- (void)refreshChromeForCurrentTraits;

#pragma mark - Visibility

- (void)setOverlayVisible:(BOOL)visible animated:(BOOL)animated;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_OVERLAY_VIEW_H_
