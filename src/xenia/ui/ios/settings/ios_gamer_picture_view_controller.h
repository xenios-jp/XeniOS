/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SETTINGS_IOS_GAMER_PICTURE_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_SETTINGS_IOS_GAMER_PICTURE_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// One of the system software's gamer pictures.
@interface XeniaGamerPicture : NSObject
@property(nonatomic, copy) NSString* key;
@property(nonatomic, retain) UIImage* image;
@end

// Gamer Picture: a grid of the built-in pictures; tapping one calls |choose| and goes back when
// it returns YES. With none (no system software) it says how to get them.
@interface XeniaGamerPictureViewController : UICollectionViewController
- (instancetype)initWithPictures:(NSArray<XeniaGamerPicture*>*)pictures
                      currentKey:(nullable NSString*)current_key
                          choose:(BOOL (^)(NSString* key))choose;
@end

NS_ASSUME_NONNULL_END

#endif  // XENIA_UI_IOS_SETTINGS_IOS_GAMER_PICTURE_VIEW_CONTROLLER_H_
