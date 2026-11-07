/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_glass_card.h"

#include <cmath>

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

@interface XEGlassCardPresentationController : UIPresentationController
- (void)fitAnimated:(BOOL)animated;
@end

@interface XEGlassCardTransition
    : NSObject <UIViewControllerTransitioningDelegate, UIViewControllerAnimatedTransitioning>
@property(nonatomic) BOOL presenting;
@end

namespace {

// Below the card's last group.
constexpr CGFloat kCardBottomPadding = 10.0;
// How much of a grouped table's first header margin hides under the card's bar.
constexpr CGFloat kFirstHeaderTuck = 18.0;

UINavigationController* CardNavigation(UIViewController* page) {
  if ([page isKindOfClass:[UINavigationController class]]) {
    return (UINavigationController*)page;
  }
  return page.navigationController;
}

XEGlassCardPresentationController* CardPresentation(UIViewController* page) {
  UINavigationController* navigation = CardNavigation(page);
  if (!navigation || navigation.presentingViewController == nil) {
    return nil;
  }
  UIPresentationController* presentation = navigation.presentationController;
  return [presentation isKindOfClass:[XEGlassCardPresentationController class]]
             ? (XEGlassCardPresentationController*)presentation
             : nil;
}

// The card's large leading title, in the bar beside its buttons.
UIBarButtonItem* LeadingTitleItem(NSString* title) {
  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.text = title;
  label.textColor = [XeniaTheme textPrimary];
  label.font = xe_scaled_system_font(UIFontTextStyleTitle1, 28.0, UIFontWeightBold);
  label.accessibilityTraits = UIAccessibilityTraitHeader;
  [label sizeToFit];
  UIBarButtonItem* item = [[[UIBarButtonItem alloc] initWithCustomView:label] autorelease];
  if (@available(iOS 26.0, *)) {
    item.hidesSharedBackground = YES;
  }
  return item;
}

}  // namespace

@implementation XEGlassCardPresentationController {
  UIView* _dim;
  CGSize _laidOutContainerSize;
}

- (void)dealloc {
  [_dim release];
  [super dealloc];
}

- (UINavigationController*)navigation {
  return CardNavigation(self.presentedViewController);
}

- (CGRect)frameOfPresentedViewInContainerView {
  UIView* container = self.containerView;
  if (!container) {
    return CGRectZero;
  }
  const CGRect bounds = container.bounds;
  const UIEdgeInsets safe = container.safeAreaInsets;
  const CGFloat inset = [XeniaTheme glassCardInset];
  const CGFloat available_width = bounds.size.width - safe.left - safe.right - 2.0 * inset;
  const CGFloat width = MIN(available_width, kXEGlassCardMaxWidth);
  const CGFloat top = safe.top + 4.0;
  const CGFloat max_height = bounds.size.height - top - MAX(safe.bottom, inset);
  CGFloat height = max_height;
  UINavigationController* navigation = [self navigation];
  const CGFloat content = navigation.topViewController.preferredContentSize.height;
  if (content > 0.0) {
    const CGFloat bar = navigation.navigationBarHidden
                            ? 0.0
                            : [navigation.navigationBar sizeThatFits:bounds.size].height;
    height = MIN(max_height, std::ceil(bar + content + kCardBottomPadding));
  }
  const CGFloat x = safe.left + inset + (available_width - width) / 2.0;
  return CGRectMake(x, top, width, height);
}

- (void)fitAnimated:(BOOL)animated {
  if (!self.containerView || self.presentedViewController.isBeingDismissed) {
    return;
  }
  const CGRect frame = [self frameOfPresentedViewInContainerView];
  if (CGRectEqualToRect(frame, self.presentedView.frame)) {
    return;
  }
  UIView* card = self.presentedView;
  if (UIAccessibilityIsReduceMotionEnabled()) {
    card.frame = frame;
    return;
  }
  // A push or pop resizes the card with the slide itself (and follows an edge swipe), rather
  // than snapping to the new page's height before it slides in.
  id<UIViewControllerTransitionCoordinator> coordinator = [self navigation].transitionCoordinator;
  if (coordinator.isAnimated &&
      [coordinator
          animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext> __unused c) {
            card.frame = frame;
            [card layoutIfNeeded];
          }
                          completion:nil]) {
    return;
  }
  if (!animated) {
    card.frame = frame;
    return;
  }
  [UIView animateWithDuration:0.35
                        delay:0.0
       usingSpringWithDamping:0.9
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionBeginFromCurrentState |
                              UIViewAnimationOptionAllowUserInteraction
                   animations:^{
                     card.frame = frame;
                     [card layoutIfNeeded];
                   }
                   completion:nil];
}

- (void)containerViewWillLayoutSubviews {
  [super containerViewWillLayoutSubviews];
  _dim.frame = self.containerView.bounds;
  // Only a new container size (rotation, split view) moves the card here; page changes resize
  // it through fitAnimated:, animated.
  const CGSize size = self.containerView.bounds.size;
  if (!self.presentedViewController.isBeingPresented &&
      !CGSizeEqualToSize(size, _laidOutContainerSize)) {
    self.presentedView.frame = [self frameOfPresentedViewInContainerView];
  }
  _laidOutContainerSize = size;
}

- (void)preferredContentSizeDidChangeForChildContentContainer:(id<UIContentContainer>)container {
  [super preferredContentSizeDidChangeForChildContentContainer:container];
  [self fitAnimated:YES];
}

- (void)dimTapped:(UITapGestureRecognizer* __unused)tap {
  [self.presentedViewController dismissViewControllerAnimated:YES completion:nil];
}

- (void)presentationTransitionWillBegin {
  [super presentationTransitionWillBegin];
  UINavigationController* navigation = [self navigation];
  UIView* card = navigation.view;
  card.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  XEInstallGlassCardSurface(card);

  UINavigationBarAppearance* bar = [[[UINavigationBarAppearance alloc] init] autorelease];
  [bar configureWithTransparentBackground];
  navigation.navigationBar.standardAppearance = bar;
  navigation.navigationBar.scrollEdgeAppearance = bar;
  navigation.navigationBar.compactAppearance = bar;
  navigation.navigationBar.prefersLargeTitles = NO;

  UIViewController* root = navigation.viewControllers.firstObject;
  if (root.title.length) {
    root.navigationItem.leftBarButtonItem = LeadingTitleItem(root.title);
    root.navigationItem.titleView = [[[UIView alloc] init] autorelease];
  }
  root.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;

  _dim = [[UIView alloc] initWithFrame:self.containerView.bounds];
  _dim.backgroundColor = [XeniaTheme glassDim];
  _dim.alpha = 0.0;
  _dim.isAccessibilityElement = NO;
  [_dim addGestureRecognizer:[[[UITapGestureRecognizer alloc]
                                 initWithTarget:self
                                         action:@selector(dimTapped:)] autorelease]];
  [self.containerView insertSubview:_dim atIndex:0];
  UIView* dim = _dim;
  [self.presentedViewController.transitionCoordinator
      animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext> __unused c) {
        dim.alpha = 1.0;
      }
                      completion:nil];
}

- (void)dismissalTransitionWillBegin {
  [super dismissalTransitionWillBegin];
  UIView* dim = _dim;
  [self.presentedViewController.transitionCoordinator
      animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext> __unused c) {
        dim.alpha = 0.0;
      }
                      completion:nil];
}

@end

@implementation XEGlassCardTransition

@synthesize presenting;

- (UIPresentationController*)
    presentationControllerForPresentedViewController:(UIViewController*)presented
                            presentingViewController:(UIViewController*)presenting_controller
                                sourceViewController:(UIViewController* __unused)source {
  return [[[XEGlassCardPresentationController alloc]
      initWithPresentedViewController:presented
             presentingViewController:presenting_controller] autorelease];
}

- (id<UIViewControllerAnimatedTransitioning>)
    animationControllerForPresentedController:(UIViewController* __unused)presented
                         presentingController:(UIViewController* __unused)presenting_controller
                             sourceController:(UIViewController* __unused)source {
  XEGlassCardTransition* animator = [[[XEGlassCardTransition alloc] init] autorelease];
  animator.presenting = YES;
  return animator;
}

- (id<UIViewControllerAnimatedTransitioning>)animationControllerForDismissedController:
    (UIViewController* __unused)dismissed {
  return [[[XEGlassCardTransition alloc] init] autorelease];
}

- (NSTimeInterval)transitionDuration:
    (id<UIViewControllerContextTransitioning> __unused)context {
  return self.presenting ? 0.38 : 0.24;
}

// The card settles in from a little smaller and fades out the same way: it grows out of the
// top of the screen, where the button that opened it sits.
- (void)animateTransition:(id<UIViewControllerContextTransitioning>)context {
  const BOOL reduce_motion = UIAccessibilityIsReduceMotionEnabled();
  const CGAffineTransform small =
      reduce_motion ? CGAffineTransformIdentity : CGAffineTransformMakeScale(0.92, 0.92);
  if (self.presenting) {
    UIViewController* to = [context viewControllerForKey:UITransitionContextToViewControllerKey];
    UIView* card = [context viewForKey:UITransitionContextToViewKey];
    [context.containerView addSubview:card];
    card.layer.anchorPoint = CGPointMake(0.5, 0.0);
    card.frame = [context finalFrameForViewController:to];
    card.alpha = 0.0;
    card.transform = small;
    [UIView animateWithDuration:[self transitionDuration:context]
        delay:0.0
        usingSpringWithDamping:0.86
        initialSpringVelocity:0.0
        options:UIViewAnimationOptionAllowUserInteraction
        animations:^{
          card.alpha = 1.0;
          card.transform = CGAffineTransformIdentity;
        }
        completion:^(BOOL __unused finished) {
          [context completeTransition:![context transitionWasCancelled]];
        }];
    return;
  }
  UIView* card = [context viewForKey:UITransitionContextFromViewKey];
  [UIView animateWithDuration:[self transitionDuration:context]
      delay:0.0
      options:UIViewAnimationOptionCurveEaseIn
      animations:^{
        card.alpha = 0.0;
        card.transform = small;
      }
      completion:^(BOOL __unused finished) {
        [card removeFromSuperview];
        [context completeTransition:![context transitionWasCancelled]];
      }];
}

@end

void XEPrepareGlassCard(UINavigationController* navigation) {
  // The presented controller holds its transitioning delegate weakly: one for the whole app.
  static XEGlassCardTransition* transition = [[XEGlassCardTransition alloc] init];
  navigation.modalPresentationStyle = UIModalPresentationCustom;
  navigation.transitioningDelegate = transition;
  navigation.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
}

void XEInstallGlassCardSurface(UIView* card) {
  card.backgroundColor = [UIColor clearColor];
  UIGlassEffect* effect = [UIGlassEffect effectWithStyle:UIGlassEffectStyleRegular];
  // Enough shade that captions read over the Halo 3 hero, little enough that the art still
  // shows through as colour.
  effect.tintColor = [XeniaTheme glassCardTint];
  UIVisualEffectView* glass = [[[UIVisualEffectView alloc] initWithEffect:effect] autorelease];
  glass.frame = card.bounds;
  glass.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  glass.userInteractionEnabled = NO;
  glass.cornerConfiguration = [UICornerConfiguration
      configurationWithUniformRadius:[UICornerRadius fixedRadius:[XeniaTheme glassCardRadius]]];
  [card insertSubview:glass atIndex:0];
  // The content clips to the same shape; the glass draws its own rim.
  card.layer.cornerRadius = [XeniaTheme glassCardRadius];
  card.layer.cornerCurve = kCACornerCurveContinuous;
  card.layer.masksToBounds = YES;
}

BOOL XEIsInGlassCard(UIViewController* page) { return CardPresentation(page) != nil; }

void XEFitGlassCardToTable(UITableViewController* page) {
  XEGlassCardPresentationController* presentation = CardPresentation(page);
  if (!presentation || page.navigationController.topViewController != page) {
    return;
  }
  UITableView* table = page.tableView;
  table.alwaysBounceVertical = NO;
  // The first caption sits right under the card's title, not a grouped table's gap below it:
  // no section padding, and the top of the first header's own margin tucked under the bar.
  table.sectionHeaderTopPadding = 0.0;
  if (table.contentInset.top != -kFirstHeaderTuck) {
    table.contentInset = UIEdgeInsetsMake(-kFirstHeaderTuck, 0.0, 0.0, 0.0);
  }
  // The tucked header would otherwise read as scrolled, and the bar would draw its edge line.
  if (@available(iOS 26.0, *)) {
    table.topEdgeEffect.hidden = YES;
  }
  // A form's commit button sits under the rows, in the bottom safe-area inset it adds.
  XEFitGlassCardToHeight(page, table.contentSize.height - kFirstHeaderTuck +
                                   page.additionalSafeAreaInsets.bottom);
}

void XEFitGlassCardToHeight(UIViewController* page, CGFloat content_height) {
  XEGlassCardPresentationController* presentation = CardPresentation(page);
  if (!presentation || page.navigationController.topViewController != page) {
    return;
  }
  const CGFloat height = std::ceil(content_height);
  if (height > 0.0 && std::fabs(page.preferredContentSize.height - height) >= 0.5) {
    page.preferredContentSize = CGSizeMake(0.0, height);
  }
  // Also after a pop, when the page below comes back with the size it had.
  [presentation fitAnimated:page.view.window != nil];
}
