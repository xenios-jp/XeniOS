/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_THEME_CONTROLS_H_
#define XENIA_UI_IOS_THEME_CONTROLS_H_

#import <UIKit/UIKit.h>

// UILabel that adds an inset around its text. Useful for pill / chip styling.
@interface XeniaPaddedLabel : UILabel
@property(nonatomic) UIEdgeInsets padding;
@end

// A profile's avatar: its gamer picture if one was set with xe_set_profile_picture, else a
// round avatar with its initials (accent when signed in, gray otherwise).
UIImage* xe_profile_avatar_image(NSString* gamertag, BOOL signed_in, CGFloat diameter);
// Remembers a local profile's gamer picture (PNG) for its avatar; nil or empty forgets it.
void xe_set_profile_picture(NSString* gamertag, NSData* png);
BOOL xe_profile_has_picture(NSString* gamertag);

// Dynamic-type aware font helpers.
UIFont* xe_scaled_system_font(UIFontTextStyle text_style, CGFloat point_size, UIFontWeight weight);
UIFont* xe_scaled_monospaced_font(UIFontTextStyle text_style, CGFloat point_size,
                                  UIFontWeight weight);
void xe_apply_label_font(UILabel* label, UIFontTextStyle text_style, CGFloat point_size,
                         UIFontWeight weight);
void xe_apply_monospaced_label_font(UILabel* label, UIFontTextStyle text_style, CGFloat point_size,
                                    UIFontWeight weight);
void xe_apply_button_title_font(UIButton* button, UIFontTextStyle text_style, CGFloat point_size,
                                UIFontWeight weight);
void xe_apply_text_view_font(UITextView* text_view, UIFontTextStyle text_style, CGFloat point_size,
                             UIFontWeight weight, BOOL monospaced);

// Reusable controls.
XeniaPaddedLabel* xe_make_tag_pill(NSString* text, UIColor* text_color);
UIButton* xe_make_ios_sheet_close_button(id target, SEL action);

#endif  // XENIA_UI_IOS_THEME_CONTROLS_H_
