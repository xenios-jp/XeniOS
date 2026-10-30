/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_HERO_VIEW_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_HERO_VIEW_H_

#import <UIKit/UIKit.h>

#include <cstdint>

@interface XeniaGameCompatibilityHeroView : NSObject
@property(nonatomic, readonly) UIView* backgroundView;
@property(nonatomic, readonly) UIImage* heroArtwork;
@property(nonatomic, readonly) UIImage* heroBackgroundArtwork;

- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                    closeTarget:(id)close_target
                    closeAction:(SEL)close_action;
- (void)setCompatInfo:(NSDictionary*)compat_info summarySource:(NSDictionary*)summary_source;
- (void)setHeroArtwork:(UIImage*)image;
- (void)setHeroBackgroundArtwork:(UIImage*)image;
- (void)loadArtworkIfNeeded;
- (void)buildIfNeededWithTableView:(UITableView*)table_view controllerView:(UIView*)controller_view;
- (void)layoutInTableView:(UITableView*)table_view
           controllerView:(UIView*)controller_view
                 hostView:(UIView*)host_view;
- (void)layoutOverlayFrames;
- (void)updateGradientFrames;
- (void)ensureTopGlowAnimation;
- (void)updateTraitColors;
- (void)setHidden:(BOOL)hidden;
- (void)hideAndRemoveFromSuperview;
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_HERO_VIEW_H_
