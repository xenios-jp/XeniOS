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
UIImage* xe_settings_footer_image(NSString* asset_name, NSString* fallback_symbol_name,
                                  BOOL tintable);
UIButton* xe_make_settings_footer_button(NSString* asset_name, NSString* fallback_symbol_name,
                                         NSString* accessibility_label, NSInteger tag,
                                         BOOL tintable, id target, SEL action);

#endif  // XENIA_UI_IOS_THEME_CONTROLS_H_
