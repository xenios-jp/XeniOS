/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_live_invite_banner.h"

#import <QuartzCore/QuartzCore.h>

#include "xenia/base/logging.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {
// Long enough to read and reach for the screen; the invite stays in Friends after.
constexpr NSTimeInterval kBannerLifetime = 20.0;
constexpr CGFloat kBannerMaxWidth = 520.0;
// Over a game the player's thumbs are on the screen: a tap that lands on the
// banner as it slides in is the game's, not an answer. Its buttons wait this
// long after it shows (2026-11-07: an invite was accepted 0.7 s after it came,
// unseen).
constexpr NSTimeInterval kOverGameArmDelay = 1.2;
}  // namespace

@implementation XeniaIOSLiveInviteBanner {
  XeniaLiveInvite* invite_;
  void (^accept_)(XeniaLiveInvite*);
  void (^decline_)(XeniaLiveInvite*);
  UIImageView* cover_;
  NSTimer* timer_;
  // Taps before this (CACurrentMediaTime) are ignored.
  CFTimeInterval armed_at_;
}

+ (XeniaIOSLiveInviteBanner*)bannerInView:(UIView*)host {
  for (UIView* view in host.subviews) {
    if ([view isKindOfClass:[XeniaIOSLiveInviteBanner class]]) {
      return (XeniaIOSLiveInviteBanner*)view;
    }
  }
  return nil;
}

+ (void)dismissInView:(UIView*)host {
  [[self bannerInView:host] dismissAnimated:YES];
}

+ (void)showInvite:(XeniaLiveInvite*)invite
            inView:(UIView*)host
          overGame:(BOOL)over_game
            accept:(void (^)(XeniaLiveInvite*))accept
           decline:(void (^)(XeniaLiveInvite*))decline {
  if (!host) {
    return;
  }
  [[self bannerInView:host] dismissAnimated:NO];
  XeniaIOSLiveInviteBanner* banner =
      [[[XeniaIOSLiveInviteBanner alloc] initWithInvite:invite
                                               overGame:over_game
                                                 accept:accept
                                                decline:decline] autorelease];
  banner.translatesAutoresizingMaskIntoConstraints = NO;
  [host addSubview:banner];
  UILayoutGuide* safe = host.safeAreaLayoutGuide;
  NSLayoutConstraint* width = [banner.widthAnchor constraintEqualToConstant:kBannerMaxWidth];
  width.priority = UILayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    width,
    [banner.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
    [banner.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:12.0],
    [banner.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-12.0],
    [banner.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8.0],
  ]];
  [host layoutIfNeeded];
  banner.transform = CGAffineTransformMakeTranslation(0, -banner.bounds.size.height - 40.0);
  banner.alpha = 0.0;
  [UIView animateWithDuration:0.35
                        delay:0.0
       usingSpringWithDamping:0.85
        initialSpringVelocity:0.0
                      options:UIViewAnimationOptionAllowUserInteraction
                   animations:^{
                     banner.transform = CGAffineTransformIdentity;
                     banner.alpha = 1.0;
                   }
                   completion:nil];
  UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification,
                                  banner.accessibilityLabel);
}

- (instancetype)initWithInvite:(XeniaLiveInvite*)invite
                      overGame:(BOOL)over_game
                        accept:(void (^)(XeniaLiveInvite*))accept
                       decline:(void (^)(XeniaLiveInvite*))decline {
  if (!(self = [super initWithFrame:CGRectZero])) {
    return nil;
  }
  invite_ = [invite retain];
  accept_ = [accept copy];
  decline_ = [decline copy];
  armed_at_ = CACurrentMediaTime() + (over_game ? kOverGameArmDelay : 0.0);
  // Dark glass, as the library's header over its art, whatever is under it.
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  xe_apply_glass_card_chrome(self, NO);
  self.layer.cornerRadius = XeniaRadiusXl;
  self.layer.cornerCurve = kCACornerCurveContinuous;
  for (UIView* view in self.subviews) {
    if ([view isKindOfClass:[UIVisualEffectView class]]) {
      view.layer.cornerRadius = XeniaRadiusXl;
      view.layer.cornerCurve = kCACornerCurveContinuous;
      view.clipsToBounds = YES;
    }
  }
  xe_apply_shadow_token(self, XeniaShadowElevationElevated);

  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  NSString* game = [social nameForTitle:invite.titleId];

  UIImageView* avatar = [[[UIImageView alloc]
      initWithImage:xe_profile_avatar_image(invite.gamertag, YES, 40.0)] autorelease];
  avatar.translatesAutoresizingMaskIntoConstraints = NO;

  UILabel* title = [[[UILabel alloc] init] autorelease];
  title.text = [NSString stringWithFormat:NSLocalizedString(@"%@ invited you",
                                                            @"Invite banner title; %@ is the "
                                                            @"gamertag"),
                                          invite.gamertag];
  xe_apply_label_font(title, UIFontTextStyleHeadline, 17.0, UIFontWeightSemibold);
  title.textColor = [XeniaTheme textPrimary];
  title.numberOfLines = 2;
  UILabel* subtitle = [[[UILabel alloc] init] autorelease];
  NSString* what = invite.party ? NSLocalizedString(@"To their party",
                                                    @"Invite banner subtitle for a party invite")
                               : game;
  subtitle.text = invite.message.length
                      ? [NSString stringWithFormat:@"%@ · %@", what, invite.message]
                      : what;
  xe_apply_label_font(subtitle, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  subtitle.textColor = [XeniaTheme textSecondary];
  subtitle.numberOfLines = 2;
  UIStackView* text =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ title, subtitle ]] autorelease];
  text.axis = UILayoutConstraintAxisVertical;
  text.spacing = 2.0;

  cover_ = [[UIImageView alloc] init];
  cover_.translatesAutoresizingMaskIntoConstraints = NO;
  cover_.contentMode = UIViewContentModeScaleAspectFill;
  cover_.clipsToBounds = YES;
  cover_.layer.cornerRadius = XeniaRadiusXs;
  cover_.layer.cornerCurve = kCACornerCurveContinuous;
  cover_.backgroundColor = [[XeniaTheme textPrimary] colorWithAlphaComponent:0.08];
  cover_.image = xe_cached_game_art_thumbnail(invite.titleId);
  if (!cover_.image) {
    UIImageView* cover = [cover_ retain];
    xe_load_game_art_thumbnail(invite.titleId, 120.0, ^(UIImage* image) {
      if (image) {
        cover.image = image;
      }
      [cover release];
    });
  }

  cover_.hidden = invite.party;
  UIStackView* top =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ avatar, text, cover_ ]] autorelease];
  top.axis = UILayoutConstraintAxisHorizontal;
  top.alignment = UIStackViewAlignmentCenter;
  top.spacing = 12.0;

  UIButton* decline_button = [self buttonWithTitle:NSLocalizedString(@"Decline",
                                                                     @"Invite banner button")
                                           primary:NO
                                            action:@selector(declineTapped:)];
  UIButton* accept_button = [self
      buttonWithTitle:invite.party ? NSLocalizedString(@"Join Party",
                                                       @"Invite banner button: accept a party "
                                                       @"invite")
                                   : NSLocalizedString(@"Accept", @"Invite banner button")
              primary:YES
               action:@selector(acceptTapped:)];
  accept_button.accessibilityHint =
      invite.party ? NSLocalizedString(@"Joins the party. Your game keeps running.",
                                       @"VoiceOver hint on Join Party")
                   : [NSString stringWithFormat:NSLocalizedString(
                                                    @"Starts %@ and joins the game.",
                                                    @"VoiceOver hint on Accept; %@ is the game"),
                                                game];
  UIStackView* buttons = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ decline_button, accept_button ]] autorelease];
  buttons.axis = UILayoutConstraintAxisHorizontal;
  buttons.distribution = UIStackViewDistributionFillEqually;
  buttons.spacing = 10.0;

  UIStackView* content =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ top, buttons ]] autorelease];
  content.translatesAutoresizingMaskIntoConstraints = NO;
  content.axis = UILayoutConstraintAxisVertical;
  content.spacing = 12.0;
  [self addSubview:content];
  [NSLayoutConstraint activateConstraints:@[
    [avatar.widthAnchor constraintEqualToConstant:40.0],
    [avatar.heightAnchor constraintEqualToConstant:40.0],
    [cover_.widthAnchor constraintEqualToConstant:33.0],
    [cover_.heightAnchor constraintEqualToConstant:44.0],
    [content.topAnchor constraintEqualToAnchor:self.topAnchor constant:14.0],
    [content.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:14.0],
    [content.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-14.0],
    [content.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-14.0],
    [decline_button.heightAnchor constraintGreaterThanOrEqualToConstant:40.0],
  ]];

  // Swiping it up puts it away; the invite stays in Friends.
  UISwipeGestureRecognizer* swipe =
      [[[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swiped:)]
          autorelease];
  swipe.direction = UISwipeGestureRecognizerDirectionUp;
  [self addGestureRecognizer:swipe];

  self.accessibilityLabel =
      invite.party
          ? [NSString stringWithFormat:NSLocalizedString(@"%@ invited you to their party",
                                                         @"Invite banner, VoiceOver; %@ is the "
                                                         @"gamertag"),
                                       invite.gamertag]
          : [NSString stringWithFormat:NSLocalizedString(@"%1$@ invited you to %2$@",
                                                         @"Invite banner, VoiceOver; %1$@ is "
                                                         @"the gamertag, %2$@ the game"),
                                       invite.gamertag, game];
  self.accessibilityElements = @[ text, decline_button, accept_button ];
  text.isAccessibilityElement = YES;
  text.accessibilityLabel = self.accessibilityLabel;

  __unsafe_unretained XeniaIOSLiveInviteBanner* unsafe_self = self;
  timer_ = [[NSTimer scheduledTimerWithTimeInterval:kBannerLifetime
                                            repeats:NO
                                              block:^(NSTimer* __unused timer) {
                                                [unsafe_self dismissAnimated:YES];
                                              }] retain];
  return self;
}

- (void)dealloc {
  [timer_ invalidate];
  [timer_ release];
  [invite_ release];
  [accept_ release];
  [decline_ release];
  [cover_ release];
  [super dealloc];
}

- (UIButton*)buttonWithTitle:(NSString*)title primary:(BOOL)primary action:(SEL)action {
  // Accept as the library's white Play capsule; Decline as its glass ⋯.
  UIButtonConfiguration* config = primary ? [UIButtonConfiguration filledButtonConfiguration]
                                          : [UIButtonConfiguration glassButtonConfiguration];
  config.title = title;
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  if (primary) {
    config.image = [UIImage
        systemImageNamed:@"play.fill"
        withConfiguration:[UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleBody
                                                                           scale:UIImageSymbolScaleSmall]];
    config.imagePadding = 6.0;
    config.baseBackgroundColor = UIColor.whiteColor;
    config.baseForegroundColor = UIColor.blackColor;
    config.contentInsets = NSDirectionalEdgeInsetsMake(10.0, 20.0, 10.0, 22.0);
  } else {
    config.baseForegroundColor = UIColor.whiteColor;
    config.contentInsets = NSDirectionalEdgeInsetsMake(10.0, 18.0, 10.0, 18.0);
  }
  config.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
      NSDictionary<NSAttributedStringKey, id>* attributes) {
    NSMutableDictionary* updated = [[attributes mutableCopy] autorelease];
    updated[NSFontAttributeName] =
        xe_scaled_system_font(UIFontTextStyleHeadline, 16.0, UIFontWeightSemibold);
    return updated;
  };
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  return button;
}

- (void)dismissAnimated:(BOOL)animated {
  [timer_ invalidate];
  if (!self.superview) {
    return;
  }
  if (!animated) {
    [self removeFromSuperview];
    return;
  }
  [self retain];
  [UIView animateWithDuration:0.25
      animations:^{
        self.transform = CGAffineTransformMakeTranslation(0, -self.bounds.size.height - 40.0);
        self.alpha = 0.0;
      }
      completion:^(__unused BOOL finished) {
        [self removeFromSuperview];
        [self release];
      }];
}

- (void)swiped:(UISwipeGestureRecognizer*)__unused gesture {
  [self dismissAnimated:YES];
}

- (BOOL)armed {
  if (CACurrentMediaTime() >= armed_at_) {
    return YES;
  }
  XELOGI("iOS: ignored a tap on the invite banner as it appeared");
  return NO;
}

- (void)acceptTapped:(id)__unused sender {
  if (![self armed]) {
    return;
  }
  void (^accept)(XeniaLiveInvite*) = [[accept_ retain] autorelease];
  XeniaLiveInvite* invite = [[invite_ retain] autorelease];
  [self dismissAnimated:YES];
  if (accept) {
    accept(invite);
  }
}

- (void)declineTapped:(id)__unused sender {
  if (![self armed]) {
    return;
  }
  void (^decline)(XeniaLiveInvite*) = [[decline_ retain] autorelease];
  XeniaLiveInvite* invite = [[invite_ retain] autorelease];
  [self dismissAnimated:YES];
  if (decline) {
    decline(invite);
  }
}

@end
