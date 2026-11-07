/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_art_wash.h"

#import "xenia/ui/ios/shared/ios_glass_card.h"

#import <CoreImage/CoreImage.h>
#import <objc/runtime.h>

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

static NSString* const kXEArtWashDidChange = @"XEArtWashDidChange";
static UIImage* g_art_wash_image = nil;
static char kXEUsesArtWashKey;

UIImage* XEBlurredBackdropImage(UIImage* image) {
  if (!image.CGImage) {
    return nil;
  }
  static CIContext* context = [[CIContext contextWithOptions:nil] retain];
  CIImage* input = [CIImage imageWithCGImage:image.CGImage];
  const CGFloat longest = MAX(input.extent.size.width, input.extent.size.height);
  if (longest <= 0.0) {
    return nil;
  }
  const CGFloat scale = MIN(1.0, 160.0 / longest);
  const CGRect extent = CGRectMake(0.0, 0.0, floor(input.extent.size.width * scale),
                                   floor(input.extent.size.height * scale));
  CIImage* small = [input imageByApplyingTransform:CGAffineTransformMakeScale(scale, scale)];
  CIImage* blurred = [[small imageByClampingToExtent]
      imageByApplyingGaussianBlurWithSigma:MAX(6.0, extent.size.width / 14.0)];
  CGImageRef rendered = [context createCGImage:blurred fromRect:extent];
  if (!rendered) {
    return nil;
  }
  UIImage* result = [UIImage imageWithCGImage:rendered];
  CGImageRelease(rendered);
  return result;
}

void XESetArtWashImage(UIImage* blurred) {
  if (blurred == g_art_wash_image) {
    return;
  }
  [g_art_wash_image release];
  g_art_wash_image = [blurred retain];
  [[NSNotificationCenter defaultCenter] postNotificationName:kXEArtWashDidChange object:nil];
}

UIImage* XEArtWashImage(void) { return g_art_wash_image; }

@implementation XEArtWashView {
  UIImageView* _art;
  UIView* _dim;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.userInteractionEnabled = NO;
    self.isAccessibilityElement = NO;
    self.backgroundColor = [XeniaTheme bgPrimary];
    _art = [[UIImageView alloc] init];
    _art.contentMode = UIViewContentModeScaleAspectFill;
    _art.clipsToBounds = YES;
    [self addSubview:_art];
    // The library's own dim, so a sheet reads as the same place with the lights lowered.
    _dim = [[UIView alloc] init];
    _dim.backgroundColor = [[XeniaTheme bgPrimary] colorWithAlphaComponent:0.55];
    [self addSubview:_dim];
    [self artChanged];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(artChanged)
                                                 name:kXEArtWashDidChange
                                               object:nil];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_art release];
  [_dim release];
  [super dealloc];
}

- (void)artChanged {
  UIImage* image = XEArtWashImage();
  if (_art.image == image) {
    return;
  }
  if (!self.window) {
    _art.image = image;
    return;
  }
  [UIView transitionWithView:_art
                    duration:0.35
                     options:UIViewAnimationOptionTransitionCrossDissolve
                  animations:^{
                    self->_art.image = image;
                  }
                  completion:nil];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  _art.frame = self.bounds;
  _dim.frame = self.bounds;
}

@end

void XEUseArtWash(UIViewController* page) {
  objc_setAssociatedObject(page, &kXEUsesArtWashKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  page.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  if ([page isKindOfClass:[XESheetTableViewController class]]) {
    XESheetTableViewController* table = (XESheetTableViewController*)page;
    table.glassStyle = YES;
    table.tableView.backgroundColor = [UIColor clearColor];
    table.tableView.separatorColor = [XeniaTheme glassHairline];
  }
  if (page.isViewLoaded) {
    page.view.backgroundColor = [UIColor clearColor];
  }
  XEInstallArtWashIfNeeded(page);
}

void XEInstallArtWashIfNeeded(UIViewController* page) {
  if (![objc_getAssociatedObject(page, &kXEUsesArtWashKey) boolValue]) {
    return;
  }
  // A glass card is its own backdrop.
  if (XEIsInGlassCard(page)) {
    return;
  }
  // One wash for the whole container: pushes and the iPad's two columns share it.
  UIViewController* container = page.splitViewController ?: page.navigationController;
  if (!container) {
    return;
  }
  container.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  UIView* host = container.view;
  for (UIView* subview in host.subviews) {
    if ([subview isKindOfClass:[XEArtWashView class]]) {
      return;
    }
  }
  host.backgroundColor = [XeniaTheme bgPrimary];
  XEArtWashView* wash = [[[XEArtWashView alloc] initWithFrame:host.bounds] autorelease];
  wash.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [host insertSubview:wash atIndex:0];
  // In a split view the columns' own navigation controllers draw over it: keep them clear.
  if (page.splitViewController) {
    for (UIViewController* column in page.splitViewController.viewControllers) {
      column.view.backgroundColor = [UIColor clearColor];
    }
  }
}
