/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_page_push.h"

#import <objc/runtime.h>

@interface XEPagePushTransition
    : NSObject <UIViewControllerTransitioningDelegate, UIViewControllerAnimatedTransitioning>
@property(nonatomic, retain) NSArray<UIView*>* covered;
@property(nonatomic, copy) void (^closeAll)(void);
@property(nonatomic, retain) UIView* dim;
@property(nonatomic) BOOL presenting;
// Set by ‹ and the edge swipe: back to the page, which stays open.
@property(nonatomic) BOOL backing;
@property(nonatomic, assign) UINavigationController* navigation;
@end

namespace {

char kPagePushKey;

// How far the page under it drifts back while the pushed page comes in.
constexpr CGFloat kCoveredDrift = 48.0;
// The page dim over the blurred art: the token range is 40-45%.
constexpr CGFloat kPageDim = 0.42;

UIView* PageShade(UIView* wash) {
  return ((UIVisualEffectView*)wash).contentView.subviews.firstObject;
}

XEPagePushTransition* PushOf(UIViewController* page) {
  UINavigationController* navigation = [page isKindOfClass:[UINavigationController class]]
                                           ? (UINavigationController*)page
                                           : page.navigationController;
  return navigation ? objc_getAssociatedObject(navigation, &kPagePushKey) : nil;
}

}  // namespace

@implementation XEPagePushTransition

@synthesize covered, closeAll, dim, presenting, backing, navigation;

- (void)dealloc {
  [covered release];
  [closeAll release];
  [dim release];
  [super dealloc];
}

- (void)backTapped {
  self.backing = YES;
  [self.navigation dismissViewControllerAnimated:YES completion:nil];
}

- (void)edgeSwiped:(UIScreenEdgePanGestureRecognizer*)swipe {
  if (swipe.state == UIGestureRecognizerStateBegan &&
      self.navigation.viewControllers.count <= 1) {
    [self backTapped];
  }
}

- (id<UIViewControllerAnimatedTransitioning>)
    animationControllerForPresentedController:(UIViewController* __unused)presented
                         presentingController:(UIViewController* __unused)presenting_controller
                             sourceController:(UIViewController* __unused)source {
  self.presenting = YES;
  return self;
}

- (id<UIViewControllerAnimatedTransitioning>)animationControllerForDismissedController:
    (UIViewController* __unused)dismissed {
  self.presenting = NO;
  return self;
}

- (NSTimeInterval)transitionDuration:
    (id<UIViewControllerContextTransitioning> __unused)context {
  return self.presenting ? 0.42 : 0.32;
}

- (void)setCoveredAway:(BOOL)away {
  for (UIView* view in self.covered) {
    view.alpha = away ? 0.0 : 1.0;
    view.transform = away ? CGAffineTransformMakeTranslation(-kCoveredDrift, 0.0)
                          : CGAffineTransformIdentity;
  }
}

// Slides in from the trailing edge like a push; the art under it stays where it is.
- (void)animateTransition:(id<UIViewControllerContextTransitioning>)context {
  UIView* container = context.containerView;
  const BOOL rtl = container.effectiveUserInterfaceLayoutDirection ==
                   UIUserInterfaceLayoutDirectionRightToLeft;
  const CGFloat width = CGRectGetWidth(container.bounds);
  const BOOL reduce_motion = UIAccessibilityIsReduceMotionEnabled();
  const CGAffineTransform offstage =
      reduce_motion ? CGAffineTransformIdentity
                    : CGAffineTransformMakeTranslation(rtl ? -width : width, 0.0);
  const NSTimeInterval duration = [self transitionDuration:context];
  if (self.presenting) {
    UIViewController* to = [context viewControllerForKey:UITransitionContextToViewControllerKey];
    UIView* page = [context viewForKey:UITransitionContextToViewKey];
    // The page's own backdrop (docs/ios_design_language.md, rule 1): the art blurred under the
    // page dim, so rows and text over it keep their contrast. Never the sharp art: busy detail
    // behind content fails legibility. Reduce Transparency turns the blur opaque by itself.
    if (!self.dim) {
      UIVisualEffectView* wash = [[[UIVisualEffectView alloc] initWithEffect:nil] autorelease];
      wash.userInteractionEnabled = NO;
      UIView* shade = [[[UIView alloc] initWithFrame:wash.contentView.bounds] autorelease];
      shade.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
      shade.backgroundColor = [UIColor colorWithWhite:0.0 alpha:kPageDim];
      [wash.contentView addSubview:shade];
      self.dim = wash;
    }
    self.dim.frame = container.bounds;
    self.dim.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    // A blur ignores its parent's alpha: fade it by animating its effect from nil.
    ((UIVisualEffectView*)self.dim).effect = nil;
    PageShade(self.dim).alpha = 0.0;
    [container addSubview:self.dim];
    page.frame = [context finalFrameForViewController:to];
    page.transform = offstage;
    page.alpha = reduce_motion ? 0.0 : 1.0;
    [container addSubview:page];
    [UIView animateWithDuration:duration
        delay:0.0
        usingSpringWithDamping:1.0
        initialSpringVelocity:0.0
        options:UIViewAnimationOptionAllowUserInteraction
        animations:^{
          page.transform = CGAffineTransformIdentity;
          page.alpha = 1.0;
          ((UIVisualEffectView*)self.dim).effect =
              [UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark];
          PageShade(self.dim).alpha = 1.0;
          [self setCoveredAway:YES];
        }
        completion:^(BOOL __unused finished) {
          [context completeTransition:![context transitionWasCancelled]];
        }];
    return;
  }
  UIView* page = [context viewForKey:UITransitionContextFromViewKey];
  const BOOL back = self.backing;
  [UIView animateWithDuration:duration
      delay:0.0
      usingSpringWithDamping:1.0
      initialSpringVelocity:0.0
      options:0
      animations:^{
        page.transform = offstage;
        if (reduce_motion) {
          page.alpha = 0.0;
        }
        ((UIVisualEffectView*)self.dim).effect = nil;
        PageShade(self.dim).alpha = 0.0;
        // Back to the page: it comes forward again. Closing everything leaves it for the
        // page's own close to take away.
        if (back) {
          [self setCoveredAway:NO];
        }
      }
      completion:^(BOOL __unused finished) {
        [page removeFromSuperview];
        [self.dim removeFromSuperview];
        const BOOL cancelled = [context transitionWasCancelled];
        [context completeTransition:!cancelled];
        if (cancelled) {
          return;
        }
        if (!back) {
          [self setCoveredAway:NO];
          if (self.closeAll) {
            self.closeAll();
          }
        }
        self.backing = NO;
      }];
}

@end

void XEPreparePagePush(UINavigationController* navigation, NSArray<UIView*>* covered,
                       void (^close_all)(void)) {
  XEPagePushTransition* push = [[[XEPagePushTransition alloc] init] autorelease];
  push.covered = covered;
  push.closeAll = close_all;
  push.navigation = navigation;
  // The presented controller holds its transitioning delegate weakly.
  objc_setAssociatedObject(navigation, &kPagePushKey, push, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  navigation.modalPresentationStyle = UIModalPresentationOverFullScreen;
  navigation.transitioningDelegate = push;
  navigation.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  navigation.view.backgroundColor = [UIColor clearColor];

  // A small centred title between ‹ and ✕, over the art.
  UINavigationBarAppearance* bar = [[[UINavigationBarAppearance alloc] init] autorelease];
  [bar configureWithTransparentBackground];
  navigation.navigationBar.standardAppearance = bar;
  navigation.navigationBar.scrollEdgeAppearance = bar;
  navigation.navigationBar.compactAppearance = bar;
  navigation.navigationBar.prefersLargeTitles = NO;

  UIViewController* root = navigation.viewControllers.firstObject;
  root.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  UIBarButtonItem* back = [[[UIBarButtonItem alloc]
      initWithImage:[UIImage systemImageNamed:@"chevron.backward"]
              style:UIBarButtonItemStylePlain
             target:push
             action:@selector(backTapped)] autorelease];
  back.accessibilityLabel = NSLocalizedString(@"Back", @"page pushed inside the game page");
  root.navigationItem.leftBarButtonItem = back;

  UIScreenEdgePanGestureRecognizer* swipe = [[[UIScreenEdgePanGestureRecognizer alloc]
      initWithTarget:push
              action:@selector(edgeSwiped:)] autorelease];
  swipe.edges = UIRectEdgeLeft;
  [navigation.view addGestureRecognizer:swipe];
}

BOOL XEIsPagePush(UIViewController* page) { return PushOf(page) != nil; }
