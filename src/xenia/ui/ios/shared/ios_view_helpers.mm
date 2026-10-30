/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_view_helpers.h"

#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"

NSString* ToNSString(const std::string& value) {
  return [NSString stringWithUTF8String:value.c_str()];
}

NSString* XEFormatTitleIDHexUpper(uint32_t title_id) {
  return [NSString stringWithFormat:@"%08X", title_id];
}

NSString* XEFormatTitleIDHexLower(uint32_t title_id) {
  return [NSString stringWithFormat:@"%08x", title_id];
}

UIColor* XEColorFromHexRGB(uint32_t rgb) {
  return [UIColor colorWithRed:((rgb >> 16) & 0xFF) / 255.0
                         green:((rgb >> 8) & 0xFF) / 255.0
                          blue:(rgb & 0xFF) / 255.0
                         alpha:1.0];
}

void XEApplyAccessibility(UIView* view, NSString* label, NSString* value,
                          NSString* hint, UIAccessibilityTraits traits) {
  if (!view) {
    return;
  }
  view.isAccessibilityElement = label.length > 0 || value.length > 0 ||
                                hint.length > 0 || traits != UIAccessibilityTraitNone;
  view.accessibilityLabel = label;
  view.accessibilityValue = value;
  view.accessibilityHint = hint;
  if (traits != UIAccessibilityTraitNone) {
    view.accessibilityTraits = traits;
  }
}

void XEConfigureTaskSheet(UINavigationController* navigation_controller,
                          UIView* host_view, CGSize preferred_size,
                          BOOL prevent_interactive_dismissal) {
  if (!navigation_controller) {
    return;
  }
  navigation_controller.navigationBar.prefersLargeTitles = NO;
  navigation_controller.preferredContentSize = preferred_size;
  navigation_controller.modalInPresentation = prevent_interactive_dismissal;

  const CGSize bounds_size = host_view ? host_view.bounds.size : UIScreen.mainScreen.bounds.size;
  const BOOL landscape_presentation = bounds_size.width > bounds_size.height;
  navigation_controller.modalPresentationStyle =
      (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) ? UIModalPresentationFormSheet
                                                            : UIModalPresentationPageSheet;
  if (@available(iOS 15.0, *)) {
    UISheetPresentationController* sheet = navigation_controller.sheetPresentationController;
    sheet.detents = landscape_presentation
                       ? @[ [UISheetPresentationControllerDetent largeDetent] ]
                       : @[
                         [UISheetPresentationControllerDetent mediumDetent],
                         [UISheetPresentationControllerDetent largeDetent]
                       ];
    sheet.prefersGrabberVisible = YES;
    sheet.prefersScrollingExpandsWhenScrolledToEdge = YES;
    if (UI_USER_INTERFACE_IDIOM() != UIUserInterfaceIdiomPad) {
      sheet.prefersEdgeAttachedInCompactHeight = YES;
      sheet.widthFollowsPreferredContentSizeWhenEdgeAttached = YES;
    }
  }
}

void XEConfigureDestinationControllerPresentation(
    UIViewController* view_controller, UIView* host_view,
    CGSize preferred_size, BOOL prevent_interactive_dismissal) {
  if (!view_controller) {
    return;
  }

  const CGSize bounds_size =
      host_view ? host_view.bounds.size : UIScreen.mainScreen.bounds.size;
  const UITraitCollection* traits =
      host_view ? host_view.traitCollection : UIScreen.mainScreen.traitCollection;
  const BOOL compact_width =
      traits.horizontalSizeClass == UIUserInterfaceSizeClassCompact;
  const BOOL fullscreen =
      UI_USER_INTERFACE_IDIOM() != UIUserInterfaceIdiomPad || compact_width;

  CGSize content_size = preferred_size;
  if (CGSizeEqualToSize(content_size, CGSizeZero)) {
    content_size = bounds_size;
  }
  if (!fullscreen && bounds_size.height > 0.0) {
    content_size.height = MAX(content_size.height, bounds_size.height - 24.0);
  }

  if ([view_controller isKindOfClass:[UINavigationController class]]) {
    UINavigationController* navigation_controller =
        (UINavigationController*)view_controller;
    navigation_controller.navigationBar.prefersLargeTitles = NO;
  }
  view_controller.preferredContentSize = content_size;
  view_controller.modalInPresentation = prevent_interactive_dismissal;
  view_controller.modalPresentationStyle =
      fullscreen ? UIModalPresentationFullScreen : UIModalPresentationPageSheet;

  if (@available(iOS 15.0, *)) {
    UISheetPresentationController* sheet =
        view_controller.sheetPresentationController;
    if (sheet) {
      sheet.detents = @[ [UISheetPresentationControllerDetent largeDetent] ];
      sheet.selectedDetentIdentifier =
          UISheetPresentationControllerDetentIdentifierLarge;
      sheet.prefersGrabberVisible = NO;
      sheet.prefersScrollingExpandsWhenScrolledToEdge = YES;
      sheet.prefersEdgeAttachedInCompactHeight = NO;
      sheet.widthFollowsPreferredContentSizeWhenEdgeAttached = NO;
    }
  }
}

void XEConfigureDestinationPresentation(
    UINavigationController* navigation_controller, UIView* host_view,
    CGSize preferred_size, BOOL prevent_interactive_dismissal) {
  XEConfigureDestinationControllerPresentation(navigation_controller, host_view,
                                               preferred_size,
                                               prevent_interactive_dismissal);
}

@implementation XeniaTraitObserverView

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }
  self.userInteractionEnabled = NO;
  self.hidden = YES;
  return self;
}

- (void)dealloc {
  [_onTraitChange release];
  [super dealloc];
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if (self.onTraitChange &&
      [self.traitCollection
          hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    self.onTraitChange(previousTraitCollection);
  }
}

@end

namespace {

// Returns the floating-window border CGColor for the given trait collection.
// 10% white in dark mode, 10% black in light mode.
inline UIColor* xe_floating_window_border_color() {
  return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
    return (traits.userInterfaceStyle == UIUserInterfaceStyleLight)
               ? [UIColor colorWithWhite:0.0 alpha:0.10]
               : [UIColor colorWithWhite:1.0 alpha:0.10];
  }];
}

// Inserts a UIVisualEffectView wrapping `effect` at index 0 of `host`, sized
// to fill, with the same corner radius as the host's layer. Returns the
// inserted view (autoreleased) for callers that need to reference it.
UIVisualEffectView* xe_install_chrome_backdrop(UIView* host, UIVisualEffect* effect) {
  UIVisualEffectView* backdrop = [[UIVisualEffectView alloc] initWithEffect:effect];
  backdrop.frame = host.bounds;
  backdrop.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  backdrop.layer.cornerRadius = host.layer.cornerRadius;
  backdrop.layer.masksToBounds = YES;
  backdrop.userInteractionEnabled = NO;
  [host insertSubview:backdrop atIndex:0];
  [backdrop autorelease];
  return backdrop;
}

}  // namespace

UIVisualEffect* xe_make_chrome_visual_effect(BOOL clear_variant) {
  if (@available(iOS 26.0, *)) {
    UIGlassEffectStyle style =
        clear_variant ? UIGlassEffectStyleClear : UIGlassEffectStyleRegular;
    return [UIGlassEffect effectWithStyle:style];
  }
  // iOS 18-25 fallback: the closest pre-glass UIBlurEffect for each variant.
  // SystemUltraThinMaterial reads as the most transparent, suitable for
  // surfaces sitting on top of rich content (clear-variant callers like the
  // compat hero over game artwork). SystemMaterial is the standard chrome
  // material every other caller has been using.
  UIBlurEffectStyle fallback_style = clear_variant
                                         ? UIBlurEffectStyleSystemUltraThinMaterial
                                         : UIBlurEffectStyleSystemMaterial;
  return [UIBlurEffect effectWithStyle:fallback_style];
}

void xe_apply_floating_window_chrome(UIView* view) {
  if (!view) {
    return;
  }
  view.backgroundColor = [UIColor clearColor];
  view.layer.cornerRadius = 20.0;
  view.layer.borderWidth = 1.0;
  view.layer.borderColor = xe_floating_window_border_color().CGColor;
  view.layer.shadowColor = [UIColor blackColor].CGColor;
  view.layer.shadowOpacity = 0.14f;
  view.layer.shadowRadius = 8.0f;
  view.layer.shadowOffset = CGSizeMake(0.0f, 8.0f);
  // The backdrop sits inside the floating view as a child UIVisualEffectView
  // so the shadow can render outside the masked-to-bounds layer. Picked by
  // xe_make_chrome_visual_effect — liquid glass on iOS 26+, SystemMaterial
  // on iOS 18-25.
  xe_install_chrome_backdrop(view, xe_make_chrome_visual_effect(NO));

  // Refresh the border CGColor on trait flips. The visual-effect view picks
  // up the new dark/light variant automatically when the trait collection
  // updates, so only the layer's frozen CGColor needs an explicit re-poke.
  // The observer is added as a subview of `view`, so its lifetime never
  // exceeds the host's. Capturing weakly (__unsafe_unretained) avoids
  // forming a retain cycle while remaining safe under MRC.
  XeniaTraitObserverView* observer = [[XeniaTraitObserverView alloc] initWithFrame:CGRectZero];
  __unsafe_unretained UIView* weak_view = view;
  observer.onTraitChange = ^(UITraitCollection* __unused previous) {
    weak_view.layer.borderColor = xe_floating_window_border_color().CGColor;
  };
  [view addSubview:observer];
  [observer release];
}

void xe_apply_glass_card_chrome(UIView* view, BOOL clear_variant) {
  if (!view) {
    return;
  }
  view.backgroundColor = [UIColor clearColor];
  view.layer.cornerRadius = XeniaRadiusLg;
  view.layer.borderWidth = 1.0;
  view.layer.borderColor = xe_floating_window_border_color().CGColor;
  xe_install_chrome_backdrop(view, xe_make_chrome_visual_effect(clear_variant));

  XeniaTraitObserverView* observer =
      [[XeniaTraitObserverView alloc] initWithFrame:CGRectZero];
  __unsafe_unretained UIView* weak_view = view;
  observer.onTraitChange = ^(UITraitCollection* __unused previous) {
    weak_view.layer.borderColor = xe_floating_window_border_color().CGColor;
  };
  [view addSubview:observer];
  [observer release];
}

void xe_apply_shadow_token(UIView* view, XeniaShadowElevation elevation) {
  if (!view) {
    return;
  }
  const XeniaShadowGeometry geometry = XeniaShadowGeometryForElevation(elevation);
  view.layer.shadowColor = [XeniaTheme shadowColorForElevation:elevation].CGColor;
  view.layer.shadowOpacity = 1.0f;
  view.layer.shadowRadius = geometry.radius;
  view.layer.shadowOffset = geometry.offset;

  // shadowColor is a frozen CGColor — refresh it on trait flips so the
  // dark/light alpha switch actually takes effect.
  XeniaTraitObserverView* observer =
      [[XeniaTraitObserverView alloc] initWithFrame:CGRectZero];
  __unsafe_unretained UIView* weak_view = view;
  observer.onTraitChange = ^(UITraitCollection* __unused previous) {
    weak_view.layer.shadowColor =
        [XeniaTheme shadowColorForElevation:elevation].CGColor;
  };
  [view addSubview:observer];
  [observer release];
}

void XEPresentOKAlert(UIViewController* presenter, NSString* title, NSString* message) {
  if (!presenter) {
    return;
  }
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title ?: @"Notice"
                                          message:message ?: @""
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  [presenter presentViewController:alert animated:YES completion:nil];
}

@implementation XESheetTableViewController

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskAll;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
  return xe_current_interface_orientation(self.view);
}

@end

@implementation XESheetViewController

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskAll;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
  return xe_current_interface_orientation(self.view);
}

@end
