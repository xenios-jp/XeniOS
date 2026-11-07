/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_view_helpers.h"

#import <objc/runtime.h>

#include <cmath>

#import "xenia/ui/ios/shared/ios_art_wash.h"
#import "xenia/ui/ios/shared/ios_glass_card.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

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
  // Pages open with a large title on the leading edge, which folds into the bar on scroll.
  navigation_controller.navigationBar.prefersLargeTitles = YES;
  navigation_controller.preferredContentSize = preferred_size;
  navigation_controller.modalInPresentation = prevent_interactive_dismissal;

  const CGSize bounds_size = host_view ? host_view.bounds.size : UIScreen.mainScreen.bounds.size;
  const BOOL landscape_presentation = bounds_size.width > bounds_size.height;
  navigation_controller.modalPresentationStyle =
      (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) ? UIModalPresentationFormSheet
                                                            : UIModalPresentationPageSheet;
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
    // Pages open with a large title on the leading edge, which folds into the bar on scroll.
    navigation_controller.navigationBar.prefersLargeTitles = YES;
  }
  view_controller.preferredContentSize = content_size;
  view_controller.modalInPresentation = prevent_interactive_dismissal;
  view_controller.modalPresentationStyle =
      fullscreen ? UIModalPresentationFullScreen : UIModalPresentationPageSheet;

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
  UIGlassEffectStyle style = clear_variant ? UIGlassEffectStyleClear : UIGlassEffectStyleRegular;
  return [UIGlassEffect effectWithStyle:style];
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
  // xe_make_chrome_visual_effect — liquid glass.
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
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:title
                                   ?: NSLocalizedString(@"Notice",
                                                        @"Fallback title of a plain alert")
                       message:message ?: @""
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button: dismiss")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  [presenter presentViewController:alert animated:YES completion:nil];
}

static char kXESheetCloseKey;
static char kXESheetActionsKey;

static void XELayoutSheetHeader(UIViewController* view_controller) {
  UIBarButtonItem* close = objc_getAssociatedObject(view_controller, &kXESheetCloseKey);
  NSArray<UIBarButtonItem*>* actions =
      objc_getAssociatedObject(view_controller, &kXESheetActionsKey);
  // UIKit lays rightBarButtonItems out from the trailing edge in.
  NSMutableArray<UIBarButtonItem*>* items = [NSMutableArray array];
  if (close) {
    [items addObject:close];
  }
  for (UIBarButtonItem* item in actions.reverseObjectEnumerator) {
    [items addObject:item];
  }
  if (![view_controller.navigationItem.rightBarButtonItems isEqualToArray:items]) {
    view_controller.navigationItem.rightBarButtonItems = items;
  }
}

void XESetSheetClose(UIViewController* view_controller, id target, SEL action) {
  UIBarButtonItem* close =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                                                     target:target
                                                     action:action] autorelease];
  close.accessibilityLabel = NSLocalizedString(@"Close", @"sheet close button");
  objc_setAssociatedObject(view_controller, &kXESheetCloseKey, close,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  XELayoutSheetHeader(view_controller);
}

void XESetSheetActions(UIViewController* view_controller, NSArray<UIBarButtonItem*>* actions) {
  objc_setAssociatedObject(view_controller, &kXESheetActionsKey, [[actions copy] autorelease],
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  XELayoutSheetHeader(view_controller);
}

UIBarButtonItem* XESheetCloseItem(UIViewController* view_controller) {
  return objc_getAssociatedObject(view_controller, &kXESheetCloseKey);
}

UIBarButtonItem* XESheetPrimaryAction(UIViewController* view_controller) {
  NSArray<UIBarButtonItem*>* actions =
      objc_getAssociatedObject(view_controller, &kXESheetActionsKey);
  return actions.lastObject;
}

void XEApplySheetHeader(UIViewController* view_controller) {
  // A large title is for the page a sheet opens on; pushed pages centre theirs in the bar.
  UINavigationController* navigation = view_controller.navigationController;
  if (navigation.viewControllers.firstObject != view_controller &&
      view_controller.navigationItem.largeTitleDisplayMode ==
          UINavigationItemLargeTitleDisplayModeAutomatic) {
    view_controller.navigationItem.largeTitleDisplayMode =
        UINavigationItemLargeTitleDisplayModeNever;
  }
  UIBarButtonItem* close = objc_getAssociatedObject(view_controller, &kXESheetCloseKey);
  if (!close) {
    // Pushed inside a sheet: the root's ✕ closes the whole sheet from here too.
    UIViewController* root = view_controller.navigationController.viewControllers.firstObject;
    UIBarButtonItem* root_close =
        root && root != view_controller ? objc_getAssociatedObject(root, &kXESheetCloseKey) : nil;
    if (!root_close) {
      return;  // Not closable (or not ours): leave the page's own items alone.
    }
    if (!objc_getAssociatedObject(view_controller, &kXESheetActionsKey)) {
      // Items the page set directly become its extra actions.
      objc_setAssociatedObject(view_controller, &kXESheetActionsKey,
                               view_controller.navigationItem.rightBarButtonItems.reverseObjectEnumerator.allObjects,
                               OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    XESetSheetClose(view_controller, root_close.target, root_close.action);
    return;
  }
  XELayoutSheetHeader(view_controller);
}

static char kXEFormCommitKey;
static const CGFloat kXEFormCommitHeight = 52.0;
static const CGFloat kXEFormCommitMargin = 16.0;

UIButton* XESetFormCommit(UIViewController* view_controller, NSString* title, id target,
                          SEL action) {
  UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
  config.title = title;
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.titleTextAttributesTransformer =
      ^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
        NSMutableDictionary* out = [[in mutableCopy] autorelease];
        out[NSFontAttributeName] = xe_scaled_system_font(UIFontTextStyleBody, 17.0,
                                                         UIFontWeightSemibold);
        return out;
      };
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  // White with black text; dimmed, not grey, while it can't be used.
  button.configurationUpdateHandler = ^(UIButton* b) {
    UIButtonConfiguration* c = [[b.configuration copy] autorelease];
    const BOOL usable = b.enabled || c.showsActivityIndicator;
    c.background.backgroundColor = [UIColor colorWithWhite:1.0 alpha:usable ? 1.0 : 0.3];
    c.baseForegroundColor = [UIColor colorWithWhite:0.0 alpha:usable ? 1.0 : 0.5];
    b.configuration = c;
  };
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.enabled = NO;
  [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
  XEDetachFormCommit(view_controller);
  objc_setAssociatedObject(view_controller, &kXEFormCommitKey, button,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  if (view_controller.viewIfLoaded.window) {
    XEAttachFormCommit(view_controller);
  }
  return button;
}

void XESetFormCommitBusy(UIViewController* view_controller, BOOL busy) {
  UIButton* button = objc_getAssociatedObject(view_controller, &kXEFormCommitKey);
  UIButtonConfiguration* config = [[button.configuration copy] autorelease];
  config.showsActivityIndicator = busy;
  config.imagePlacement = NSDirectionalRectEdgeLeading;
  button.configuration = config;
  button.userInteractionEnabled = !busy;
}

void XESetFormCommitHidden(UIViewController* view_controller, BOOL hidden) {
  UIButton* button = objc_getAssociatedObject(view_controller, &kXEFormCommitKey);
  button.hidden = hidden;
  // Rows scroll clear of the button only while it is there.
  // In a glass card the button keeps a full margin off the card's rounded bottom edge.
  const CGFloat card_extra = XEIsInGlassCard(view_controller) ? kXEFormCommitMargin * 0.5 : 0.0;
  const CGFloat inset =
      button && !hidden ? kXEFormCommitHeight + kXEFormCommitMargin + card_extra : 0.0;
  UIEdgeInsets safe = view_controller.additionalSafeAreaInsets;
  if (safe.bottom != inset) {
    safe.bottom = inset;
    view_controller.additionalSafeAreaInsets = safe;
  }
}

UIButton* XEFormCommitButton(UIViewController* view_controller) {
  UIButton* button = objc_getAssociatedObject(view_controller, &kXEFormCommitKey);
  if (!button.window || button.hidden || !button.enabled || !button.userInteractionEnabled) {
    return nil;
  }
  return button;
}

void XEAttachFormCommit(UIViewController* view_controller) {
  UIButton* button = objc_getAssociatedObject(view_controller, &kXEFormCommitKey);
  if (!button) {
    return;
  }
  // The navigation controller's view, not the page's own: a table's view scrolls.
  UIView* host = view_controller.navigationController.view ?: view_controller.view;
  if (button.superview != host) {
    [button removeFromSuperview];
    [host addSubview:button];
    // Pushed or popped to: fade in with the slide. Presented: it rides in with the sheet.
    id<UIViewControllerTransitionCoordinator> coordinator = view_controller.transitionCoordinator;
    const BOOL presenting = view_controller.isBeingPresented ||
                            view_controller.navigationController.isBeingPresented;
    if (coordinator.isAnimated && !presenting) {
      button.alpha = 0.0;
      [coordinator animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext>
                                                    __unused context) {
        button.alpha = 1.0;
      }
                                   completion:nil];
    } else {
      button.alpha = 1.0;
    }
    UILayoutGuide* safe = host.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
      [button.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor
                                           constant:kXEFormCommitMargin],
      [button.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor
                                            constant:-kXEFormCommitMargin],
      [button.heightAnchor constraintEqualToConstant:kXEFormCommitHeight],
      [button.bottomAnchor
          constraintEqualToAnchor:host.keyboardLayoutGuide.topAnchor
                         constant:XEIsInGlassCard(view_controller) ? -kXEFormCommitMargin
                                                                   : -kXEFormCommitMargin * 0.5],
    ]];
  }
  XESetFormCommitHidden(view_controller, button.hidden);
}

void XEDetachFormCommit(UIViewController* view_controller) {
  UIButton* button = objc_getAssociatedObject(view_controller, &kXEFormCommitKey);
  [button removeFromSuperview];
}

void XEFormCommitWillDisappear(UIViewController* view_controller) {
  UIButton* button = objc_getAssociatedObject(view_controller, &kXEFormCommitKey);
  if (!button.superview) {
    return;
  }
  if (view_controller.isBeingDismissed || view_controller.navigationController.isBeingDismissed) {
    return;  // Slides away with the sheet; viewDidDisappear takes it off.
  }
  id<UIViewControllerTransitionCoordinator> coordinator = view_controller.transitionCoordinator;
  if (!coordinator.isAnimated) {
    [button removeFromSuperview];
    return;
  }
  [coordinator animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext>
                                                __unused context) {
    button.alpha = 0.0;
  }
      completion:^(id<UIViewControllerTransitionCoordinatorContext> context) {
        if (context.isCancelled) {
          button.alpha = 1.0;  // The swipe back was let go: the page stays.
        } else {
          [button removeFromSuperview];
        }
      }];
}

@implementation XESheetTableViewController

@synthesize glassStyle;

- (void)viewDidLoad {
  [super viewDidLoad];
  // Dragging the list puts the keyboard away, as in Settings.
  self.tableView.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
  if (self.glassStyle) {
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    self.tableView.backgroundColor = [UIColor clearColor];
    self.tableView.separatorColor = [XeniaTheme glassHairline];
  }
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  XEApplySheetHeader(self);
  XEAttachFormCommit(self);
  if (self.glassStyle) {
    // Glass rows sit on the art wash, never on a flat sheet.
    XEUseArtWash(self);
  }
  XEInstallArtWashIfNeeded(self);
  id<UIViewControllerTransitionCoordinator> coordinator = self.transitionCoordinator;
  if (!self.isMovingToParentViewController && coordinator && XEIsInGlassCard(self)) {
    // Back from a pushed page, UIKit lays the section headers out mid-pop a few points too tall,
    // and the card would keep that. Size them again once the pop lands.
    UITableView* table = self.tableView;
    [coordinator animateAlongsideTransition:nil
                                 completion:^(id<UIViewControllerTransitionCoordinatorContext> c) {
                                   if (c.isCancelled) {
                                     return;
                                   }
                                   // Only fresh header views measure right; the reused ones
                                   // keep the mid-pop height.
                                   [UIView performWithoutAnimation:^{
                                     [table reloadData];
                                     [table layoutIfNeeded];
                                   }];
                                   XEFitGlassCardToTable(self);
                                 }];
  }
  XEFitGlassCardToTable(self);
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  XEFitGlassCardToTable(self);
}

- (void)viewWillDisappear:(BOOL)animated {
  [super viewWillDisappear:animated];
  // The keyboard goes down with the page, not after it.
  [self.view endEditing:YES];
  XEFormCommitWillDisappear(self);
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  XEDetachFormCommit(self);
}

- (void)tableView:(UITableView* __unused)tableView
  willDisplayCell:(UITableViewCell*)cell
forRowAtIndexPath:(NSIndexPath* __unused)indexPath {
  // A cell the page made clear on purpose (a heading drawn as a row) stays clear.
  if (self.glassStyle &&
      !(cell.backgroundColor && CGColorGetAlpha(cell.backgroundColor.CGColor) == 0.0)) {
    cell.backgroundColor = [XeniaTheme glassGroupFill];
  }
}

// Group titles as drawn: 13 pt semibold in the secondary colour, in sentence case.
- (void)tableView:(UITableView* __unused)tableView
    willDisplayHeaderView:(UIView*)view
               forSection:(NSInteger __unused)section {
  if (![view isKindOfClass:[UITableViewHeaderFooterView class]]) {
    return;
  }
  UITableViewHeaderFooterView* header = (UITableViewHeaderFooterView*)view;
  UIFont* font = xe_scaled_system_font(UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
  id configuration = header.contentConfiguration;
  if (![configuration isKindOfClass:[UIListContentConfiguration class]]) {
    // A title from titleForHeaderInSection: shows in the legacy label.
    header.textLabel.font = font;
    header.textLabel.textColor = [XeniaTheme textSecondary];
    return;
  }
  UIListContentConfiguration* content = [[configuration copy] autorelease];
  content.textProperties.font = font;
  content.textProperties.color = [XeniaTheme textSecondary];
  content.textProperties.transform = UIListContentTextTransformNone;
  header.contentConfiguration = content;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskAll;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
  return xe_current_interface_orientation(self.view);
}

@end

@implementation XESheetViewController

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  XEApplySheetHeader(self);
  XEAttachFormCommit(self);
  XEInstallArtWashIfNeeded(self);
}

- (void)viewWillDisappear:(BOOL)animated {
  [super viewWillDisappear:animated];
  XEFormCommitWillDisappear(self);
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  XEDetachFormCommit(self);
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskAll;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
  return xe_current_interface_orientation(self.view);
}

@end

UIImage* XERowSymbolImage(NSString* symbol, UIColor* state_color) {
  // A bare 22-pt symbol, centred in a fixed 29-pt box so every row's text starts at the same
  // place. White 85% unless the row carries a state (destructive, warning, the mint accent).
  static NSCache<NSString*, UIImage*>* cache = [[NSCache alloc] init];
  UIColor* color = state_color ?: [UIColor colorWithWhite:1.0 alpha:0.85];
  CGFloat r = 0, g = 0, b = 0, a = 0;
  [color getRed:&r green:&g blue:&b alpha:&a];
  NSString* key = [NSString stringWithFormat:@"%@|%.3f|%.3f|%.3f|%.3f", symbol, r, g, b, a];
  if (UIImage* cached = [cache objectForKey:key]) {
    return cached;
  }
  const CGFloat side = 29.0;
  const CGFloat glyph_max = 22.0;
  UIImage* glyph = [[UIImage systemImageNamed:symbol
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithPointSize:19.0
                                                                      weight:UIImageSymbolWeightMedium]]
      imageWithTintColor:color
           renderingMode:UIImageRenderingModeAlwaysOriginal];
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(side, side)] autorelease];
  UIImage* image = [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused context) {
    if (glyph) {
      // Fit wide symbols inside the box.
      CGSize size = glyph.size;
      const CGFloat scale = MIN(1.0, glyph_max / MAX(size.width, size.height));
      size = CGSizeMake(size.width * scale, size.height * scale);
      [glyph drawInRect:CGRectMake((side - size.width) * 0.5, (side - size.height) * 0.5,
                                   size.width, size.height)];
    }
  }];
  image = [image imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
  [cache setObject:image forKey:key];
  return image;
}

@implementation XEStatusHeaderView {
  UIView* dot_;
  UILabel* title_label_;
  UILabel* detail_label_;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }
  dot_ = [[UIView alloc] init];
  dot_.translatesAutoresizingMaskIntoConstraints = NO;
  dot_.layer.cornerRadius = 6.0;
  dot_.backgroundColor = [XeniaTheme textMuted];
  [NSLayoutConstraint activateConstraints:@[
    [dot_.widthAnchor constraintEqualToConstant:12.0],
    [dot_.heightAnchor constraintEqualToConstant:12.0],
  ]];
  title_label_ = [[UILabel alloc] init];
  title_label_.font = xe_scaled_system_font(UIFontTextStyleTitle1, 28.0, UIFontWeightBold);
  title_label_.adjustsFontForContentSizeCategory = YES;
  title_label_.textColor = [XeniaTheme textPrimary];
  title_label_.numberOfLines = 0;
  title_label_.accessibilityTraits = UIAccessibilityTraitHeader;
  UIStackView* headline =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ dot_, title_label_ ]] autorelease];
  headline.alignment = UIStackViewAlignmentCenter;
  headline.spacing = 10.0;
  detail_label_ = [[UILabel alloc] init];
  detail_label_.font = xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  detail_label_.adjustsFontForContentSizeCategory = YES;
  detail_label_.textColor = [XeniaTheme textSecondary];
  detail_label_.numberOfLines = 0;
  UIStackView* column =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ headline, detail_label_ ]] autorelease];
  column.axis = UILayoutConstraintAxisVertical;
  column.alignment = UIStackViewAlignmentLeading;
  column.spacing = 4.0;
  column.translatesAutoresizingMaskIntoConstraints = NO;
  [self addSubview:column];
  [NSLayoutConstraint activateConstraints:@[
    [column.topAnchor constraintEqualToAnchor:self.topAnchor constant:4.0],
    [column.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-6.0],
    [column.leadingAnchor constraintEqualToAnchor:self.layoutMarginsGuide.leadingAnchor],
    [column.trailingAnchor constraintEqualToAnchor:self.layoutMarginsGuide.trailingAnchor],
  ]];
  return self;
}

- (void)dealloc {
  [dot_ release];
  [title_label_ release];
  [detail_label_ release];
  [super dealloc];
}

- (UIView*)dot {
  return dot_;
}

- (UILabel*)titleLabel {
  return title_label_;
}

- (UILabel*)detailLabel {
  return detail_label_;
}

- (void)fitAsHeaderOfTable:(UITableView*)table {
  // The table sizes its header from the header's frame: fit it to the width it has, lined up
  // with the groups' text.
  const CGFloat width = CGRectGetWidth(table.bounds);
  detail_label_.hidden = detail_label_.text.length == 0;
  self.layoutMargins =
      UIEdgeInsetsMake(0, table.layoutMargins.left + 4.0, 0, table.layoutMargins.right);
  const CGFloat height =
      [self systemLayoutSizeFittingSize:CGSizeMake(width, UILayoutFittingCompressedSize.height)
          withHorizontalFittingPriority:UILayoutPriorityRequired
                verticalFittingPriority:UILayoutPriorityFittingSizeLevel]
          .height;
  if (table.tableHeaderView != self || std::fabs(CGRectGetHeight(self.frame) - height) >= 0.5 ||
      std::fabs(CGRectGetWidth(self.frame) - width) >= 0.5) {
    self.frame = CGRectMake(0, 0, width, height);
    table.tableHeaderView = self;
  }
}

@end
