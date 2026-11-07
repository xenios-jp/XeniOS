/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_online_panel.h"

#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/launcher/ios_system_link_view_controller.h"
#import "xenia/ui/ios/shared/ios_live_social_bridge.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

constexpr CGFloat kPanelRadius = 28.0;

UILabel* NewLabel(NSString* text, UIFontTextStyle style, CGFloat size, UIFontWeight weight,
                  UIColor* color) {
  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.text = text;
  label.textColor = color;
  xe_apply_label_font(label, style, size, weight);
  return label;
}

// A small caption over a group of rows.
UIView* SectionLabel(NSString* text) {
  UILabel* label = NewLabel(text, UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold,
                            [UIColor colorWithWhite:1.0 alpha:0.62]);
  label.accessibilityTraits = UIAccessibilityTraitHeader;
  UIStackView* box = [[[UIStackView alloc] initWithArrangedSubviews:@[ label ]] autorelease];
  box.translatesAutoresizingMaskIntoConstraints = NO;
  box.axis = UILayoutConstraintAxisVertical;
  box.layoutMargins = UIEdgeInsetsMake(4, 4, 2, 0);
  box.layoutMarginsRelativeArrangement = YES;
  // Without a floor the scroll view's height pressure squeezes the label to nothing.
  [label.heightAnchor constraintGreaterThanOrEqualToConstant:16].active = YES;
  return box;
}

UIColor* HexColor(uint32_t rgb) {
  return [UIColor colorWithRed:((rgb >> 16) & 0xFF) / 255.0
                         green:((rgb >> 8) & 0xFF) / 255.0
                          blue:(rgb & 0xFF) / 255.0
                         alpha:1.0];
}

// A steady colour per name, so a player keeps theirs from list to list.
UIColor* AvatarColor(NSString* name) {
  static const uint32_t kPalette[] = {0x6C7BD1, 0xD17B9A, 0x5FB58A, 0xC9A15A, 0x7AA5D1, 0xB185D1};
  NSUInteger hash = 0;
  for (NSUInteger i = 0; i < name.length; ++i) {
    hash = hash * 31 + [name characterAtIndex:i];
  }
  return HexColor(kPalette[hash % (sizeof(kPalette) / sizeof(kPalette[0]))]);
}

// System blue, the colour of an invite and of "back".
UIColor* InviteBlue() { return HexColor(0x0A84FF); }

// A button: white for the one thing to do (Join), glass otherwise. |wide| is a full-width
// rounded button; otherwise a capsule.
UIButton* Capsule(NSString* title, NSString* image, BOOL primary, BOOL wide,
                  void (^action)(UIButton* sender)) {
  UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
  config.cornerStyle = wide ? UIButtonConfigurationCornerStyleFixed
                            : UIButtonConfigurationCornerStyleCapsule;
  if (wide) {
    config.background.cornerRadius = 14.0;
  }
  config.title = title;
  if (image) {
    config.image = [UIImage systemImageNamed:image];
    config.imagePadding = 6.0;
    config.preferredSymbolConfigurationForImage =
        [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote];
  }
  config.baseBackgroundColor = primary ? [UIColor whiteColor] : [UIColor colorWithWhite:1.0
                                                                                  alpha:0.12];
  config.baseForegroundColor = primary ? [UIColor blackColor] : [XeniaTheme textPrimary];
  config.contentInsets = wide ? NSDirectionalEdgeInsetsMake(13, 14, 13, 14)
                              : NSDirectionalEdgeInsetsMake(6, 14, 6, 14);
  UIFont* font = xe_scaled_system_font(UIFontTextStyleSubheadline, 14.0, UIFontWeightSemibold);
  config.titleTextAttributesTransformer =
      [[^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
        NSMutableDictionary<NSAttributedStringKey, id>* out = [[in mutableCopy] autorelease];
        out[NSFontAttributeName] = font;
        return out;
      } copy] autorelease];
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  if (action) {
    void (^handler)(UIButton*) = [[action copy] autorelease];
    [button addAction:[UIAction actionWithHandler:^(__kindof UIAction* a) {
              handler((UIButton*)a.sender);
            }]
     forControlEvents:UIControlEventTouchUpInside];
  }
  [button setContentHuggingPriority:UILayoutPriorityRequired
                            forAxis:UILayoutConstraintAxisHorizontal];
  [button setContentCompressionResistancePriority:UILayoutPriorityRequired
                                          forAxis:UILayoutConstraintAxisHorizontal];
  return button;
}

// A round initial in the player's own colour; accent for you.
UIView* Avatar(NSString* name, CGFloat size, BOOL highlighted) {
  UILabel* avatar = [[[UILabel alloc] init] autorelease];
  avatar.translatesAutoresizingMaskIntoConstraints = NO;
  avatar.text = name.length ? [[name substringToIndex:1] uppercaseString] : @"?";
  avatar.textAlignment = NSTextAlignmentCenter;
  avatar.textColor = highlighted ? [XeniaTheme accentFg] : [UIColor whiteColor];
  avatar.backgroundColor = highlighted ? [XeniaTheme accent] : AvatarColor(name);
  avatar.font = [UIFont systemFontOfSize:size * 0.42 weight:UIFontWeightSemibold];
  avatar.layer.cornerRadius = size / 2.0;
  avatar.layer.masksToBounds = YES;
  avatar.isAccessibilityElement = NO;
  [NSLayoutConstraint activateConstraints:@[
    [avatar.widthAnchor constraintEqualToConstant:size],
    [avatar.heightAnchor constraintEqualToConstant:size],
  ]];
  return avatar;
}

// An avatar with the green "online" dot at its corner.
UIView* AvatarWithPresence(NSString* name, CGFloat size, BOOL me, BOOL online) {
  UIView* box = [[[UIView alloc] init] autorelease];
  box.translatesAutoresizingMaskIntoConstraints = NO;
  UIView* avatar = Avatar(name, size, me);
  [box addSubview:avatar];
  [NSLayoutConstraint activateConstraints:@[
    [avatar.topAnchor constraintEqualToAnchor:box.topAnchor],
    [avatar.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
    [avatar.bottomAnchor constraintEqualToAnchor:box.bottomAnchor],
    [avatar.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],
  ]];
  if (online) {
    const CGFloat dot_size = MAX(10.0, size * 0.33);
    UIView* dot = [[[UIView alloc] init] autorelease];
    dot.translatesAutoresizingMaskIntoConstraints = NO;
    dot.backgroundColor = HexColor(0x30D158);
    dot.layer.cornerRadius = dot_size / 2.0;
    dot.layer.borderWidth = 2.0;
    dot.layer.borderColor = [UIColor colorWithWhite:0.17 alpha:1.0].CGColor;
    dot.isAccessibilityElement = NO;
    [box addSubview:dot];
    [NSLayoutConstraint activateConstraints:@[
      [dot.widthAnchor constraintEqualToConstant:dot_size],
      [dot.heightAnchor constraintEqualToConstant:dot_size],
      [dot.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:1],
      [dot.bottomAnchor constraintEqualToAnchor:box.bottomAnchor constant:1],
    ]];
  }
  return box;
}

// A row: something on the left, a name over a short detail, then the row's own buttons.
// |tapped|, when given, makes the name area a button.
UIView* Row(UIView* leading, NSString* name, NSString* detail, BOOL leader,
            NSArray<UIView*>* accessories, void (^tapped)(void)) {
  UILabel* name_label =
      NewLabel(name, UIFontTextStyleBody, 16.0, UIFontWeightMedium, [XeniaTheme textPrimary]);
  name_label.lineBreakMode = NSLineBreakByTruncatingTail;
  NSMutableArray<UIView*>* text = [NSMutableArray arrayWithObject:name_label];
  if (detail.length) {
    UILabel* detail_label = NewLabel(detail, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular,
                                     [XeniaTheme textSecondary]);
    detail_label.lineBreakMode = NSLineBreakByTruncatingTail;
    [text addObject:detail_label];
  }
  UIStackView* text_stack = [[[UIStackView alloc] initWithArrangedSubviews:text] autorelease];
  text_stack.axis = UILayoutConstraintAxisVertical;
  text_stack.spacing = 1.0;
  [text_stack setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                              forAxis:UILayoutConstraintAxisHorizontal];
  NSMutableArray<UIView*>* items = [NSMutableArray arrayWithObjects:leading, text_stack, nil];
  if (leader) {
    UIImageView* crown = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:@"crown.fill"]] autorelease];
    crown.tintColor = [XeniaTheme statusWarning];
    crown.preferredSymbolConfiguration =
        [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote];
    [crown setContentHuggingPriority:UILayoutPriorityRequired
                             forAxis:UILayoutConstraintAxisHorizontal];
    [items addObject:crown];
  }
  // Room is left here for a voice indicator when party chat comes.
  [items addObjectsFromArray:accessories];
  UIStackView* row = [[[UIStackView alloc] initWithArrangedSubviews:items] autorelease];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 12.0;
  row.layoutMargins = UIEdgeInsetsMake(8, 0, 8, 0);
  row.layoutMarginsRelativeArrangement = YES;
  row.isAccessibilityElement = NO;
  [row.heightAnchor constraintGreaterThanOrEqualToConstant:52.0].active = YES;
  name_label.accessibilityLabel =
      leader ? [NSString stringWithFormat:NSLocalizedString(@"%@, party leader",
                                                            @"Online panel, VoiceOver; %@ is "
                                                            @"the gamertag"),
                                          name]
             : name;
  if (tapped) {
    void (^handler)(void) = [[tapped copy] autorelease];
    UIButton* hit = [UIButton buttonWithType:UIButtonTypeCustom];
    hit.translatesAutoresizingMaskIntoConstraints = NO;
    hit.accessibilityLabel = name_label.accessibilityLabel;
    hit.accessibilityValue = detail;
    [hit addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
           handler();
         }]
  forControlEvents:UIControlEventTouchUpInside];
    // Under the row's own buttons, over the name. The views above it must let touches
    // through, or the stack views swallow them.
    leading.userInteractionEnabled = NO;
    text_stack.userInteractionEnabled = NO;
    BOOL has_button = NO;
    for (UIView* accessory in accessories) {
      has_button = has_button || [accessory isKindOfClass:[UIControl class]];
    }
    [row insertSubview:hit atIndex:0];
    [NSLayoutConstraint activateConstraints:@[
      [hit.leadingAnchor constraintEqualToAnchor:row.leadingAnchor],
      [hit.trailingAnchor constraintEqualToAnchor:has_button ? text_stack.trailingAnchor
                                                             : row.trailingAnchor],
      [hit.topAnchor constraintEqualToAnchor:row.topAnchor],
      [hit.bottomAnchor constraintEqualToAnchor:row.bottomAnchor],
    ]];
  }
  return row;
}

// A player row: avatar with presence, name over a short detail, the row's own buttons.
UIView* PlayerRow(NSString* name, NSString* detail, BOOL me, BOOL leader, BOOL online,
                  NSArray<UIView*>* accessories, void (^tapped)(void)) {
  UIView* row = Row(AvatarWithPresence(name, 36.0, me, online), name, detail, leader, accessories,
                    tapped);
  if (tapped) {
    ((UIView*)row).accessibilityHint = NSLocalizedString(@"Shows their gamer card.",
                                                         @"Online panel, VoiceOver: player row hint");
  }
  return row;
}

UIView* Chevron() {
  UIImageView* chevron =
      [[[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]] autorelease];
  chevron.tintColor = [UIColor colorWithWhite:1.0 alpha:0.34];
  chevron.preferredSymbolConfiguration =
      [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote
                                                       scale:UIImageSymbolScaleSmall];
  chevron.isAccessibilityElement = NO;
  [chevron setContentHuggingPriority:UILayoutPriorityRequired
                             forAxis:UILayoutConstraintAxisHorizontal];
  return chevron;
}

UIView* Hairline() {
  UIView* line = [[[UIView alloc] init] autorelease];
  line.translatesAutoresizingMaskIntoConstraints = NO;
  line.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.1];
  [line.heightAnchor constraintEqualToConstant:1.0 / UIScreen.mainScreen.scale].active = YES;
  return line;
}

// Rows in one rounded group, a hairline between them. |tint| replaces the quiet glass.
UIView* Group(NSArray<UIView*>* rows, UIColor* tint) {
  UIView* box = [[[UIView alloc] init] autorelease];
  box.translatesAutoresizingMaskIntoConstraints = NO;
  box.backgroundColor = tint ?: [UIColor colorWithWhite:1.0 alpha:0.07];
  box.layer.cornerRadius = 16.0;
  box.layer.cornerCurve = kCACornerCurveContinuous;
  NSMutableArray<UIView*>* items = [NSMutableArray array];
  for (UIView* row in rows) {
    if (items.count) {
      [items addObject:Hairline()];
    }
    [items addObject:row];
  }
  UIStackView* column = [[[UIStackView alloc] initWithArrangedSubviews:items] autorelease];
  column.translatesAutoresizingMaskIntoConstraints = NO;
  column.axis = UILayoutConstraintAxisVertical;
  [box addSubview:column];
  [NSLayoutConstraint activateConstraints:@[
    [column.topAnchor constraintEqualToAnchor:box.topAnchor],
    [column.bottomAnchor constraintEqualToAnchor:box.bottomAnchor],
    [column.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:12],
    [column.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-12],
  ]];
  return box;
}

// A round 32 pt icon button for the header, with an optional blue count on its corner.
UIButton* IconButton(NSString* symbol, NSString* label, NSInteger badge, void (^action)(void)) {
  UIButtonConfiguration* config = [UIButtonConfiguration plainButtonConfiguration];
  config.image = [UIImage systemImageNamed:symbol];
  config.baseForegroundColor = [XeniaTheme textPrimary];
  config.contentInsets = NSDirectionalEdgeInsetsZero;
  config.preferredSymbolConfigurationForImage =
      [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleSubheadline];
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.1];
  button.layer.cornerRadius = 16.0;
  button.accessibilityLabel = label;
  void (^handler)(void) = [[action copy] autorelease];
  [button addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
            handler();
          }]
   forControlEvents:UIControlEventTouchUpInside];
  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:32.0],
    [button.heightAnchor constraintEqualToConstant:32.0],
  ]];
  if (badge > 0) {
    UILabel* count = NewLabel([NSString stringWithFormat:@"%ld", (long)badge],
                              UIFontTextStyleCaption2, 11.0, UIFontWeightBold, [UIColor whiteColor]);
    count.textAlignment = NSTextAlignmentCenter;
    count.backgroundColor = InviteBlue();
    count.layer.cornerRadius = 9.0;
    count.layer.masksToBounds = YES;
    count.isAccessibilityElement = NO;
    [button addSubview:count];
    [NSLayoutConstraint activateConstraints:@[
      [count.heightAnchor constraintEqualToConstant:18.0],
      [count.widthAnchor constraintGreaterThanOrEqualToConstant:18.0],
      [count.topAnchor constraintEqualToAnchor:button.topAnchor constant:-5],
      [count.trailingAnchor constraintEqualToAnchor:button.trailingAnchor constant:5],
    ]];
    button.accessibilityValue = [NSString stringWithFormat:NSLocalizedString(@"%ld waiting",
                                                                             @"Online panel, "
                                                                             @"VoiceOver; %ld "
                                                                             @"requests waiting"),
                                                           (long)badge];
  }
  return button;
}

// A small round decline button for an invite or request.
UIButton* DeclineButton(NSString* label, void (^action)(void)) {
  UIButton* decline = [UIButton buttonWithType:UIButtonTypeSystem];
  decline.translatesAutoresizingMaskIntoConstraints = NO;
  [decline setImage:[UIImage systemImageNamed:@"xmark"] forState:UIControlStateNormal];
  decline.tintColor = [XeniaTheme textSecondary];
  decline.accessibilityLabel = label;
  void (^handler)(void) = [[action copy] autorelease];
  [decline addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
             handler();
           }]
    forControlEvents:UIControlEventTouchUpInside];
  [decline.widthAnchor constraintEqualToConstant:32.0].active = YES;
  return decline;
}

NSString* GamertagFor(uint64_t xuid) {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  if (!xuid) {
    return social.gamertag;
  }
  if (XeniaLiveFriend* peer = [social friendWithXuid:xuid]) {
    return peer.gamertag;
  }
  for (XeniaLivePartyMember* member in social.partyMembers) {
    if (member.xuid == xuid) {
      return member.gamertag;
    }
  }
  for (XeniaLiveFriend* peer in social.incomingRequests) {
    if (peer.xuid == xuid) {
      return peer.gamertag;
    }
  }
  for (XeniaLiveFriend* peer in social.outgoingRequests) {
    if (peer.xuid == xuid) {
      return peer.gamertag;
    }
  }
  return nil;
}

}  // namespace

#pragma mark - Gamer card

@implementation XeniaIOSGamerCardView {
  uint64_t _xuid;
  NSString* _gamertag;
  UIStackView* _stack;
  void (^_joinFriendHandler)(XeniaLiveFriend*);
}

@synthesize xuid = _xuid;

- (instancetype)initWithXuid:(uint64_t)xuid gamertag:(NSString*)gamertag {
  if ((self = [super initWithFrame:CGRectZero])) {
    _xuid = xuid;
    _gamertag = [gamertag copy];
    self.translatesAutoresizingMaskIntoConstraints = NO;
    _stack = [[UIStackView alloc] init];
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    _stack.axis = UILayoutConstraintAxisVertical;
    _stack.alignment = UIStackViewAlignmentCenter;
    _stack.spacing = 8.0;
    [self addSubview:_stack];
    [NSLayoutConstraint activateConstraints:@[
      [_stack.topAnchor constraintEqualToAnchor:self.topAnchor],
      [_stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
      [_stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
      [_stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(socialChanged:)
                                                 name:XeniaLiveSocialDidChangeNotification
                                               object:nil];
    [self reload];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_gamertag release];
  [_stack release];
  [_joinFriendHandler release];
  [super dealloc];
}

- (void (^)(XeniaLiveFriend*))joinFriendHandler {
  return _joinFriendHandler;
}

- (void)setJoinFriendHandler:(void (^)(XeniaLiveFriend*))handler {
  if (handler != _joinFriendHandler) {
    [_joinFriendHandler release];
    _joinFriendHandler = [handler copy];
    [self reload];
  }
}

- (void)socialChanged:(NSNotification* __unused)note {
  [self reload];
}

- (BOOL)inMyParty {
  for (XeniaLivePartyMember* member in [XeniaLiveSocial shared].partyMembers) {
    if (member.xuid == _xuid) {
      return YES;
    }
  }
  return NO;
}

- (void)reload {
  for (UIView* view in [[_stack.arrangedSubviews copy] autorelease]) {
    [view removeFromSuperview];
  }
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  const BOOL me = _xuid == 0;
  NSString* name = GamertagFor(_xuid) ?: _gamertag;
  if (!name.length) {
    name = NSLocalizedString(@"Player", @"Gamer card: a player whose gamertag isn't known");
  }
  XeniaLiveFriend* peer = me ? nil : [social friendWithXuid:_xuid];
  XeniaLiveFriend* incoming = nil;
  XeniaLiveFriend* outgoing = nil;
  for (XeniaLiveFriend* request in social.incomingRequests) {
    if (request.xuid == _xuid) {
      incoming = request;
    }
  }
  for (XeniaLiveFriend* request in social.outgoingRequests) {
    if (request.xuid == _xuid) {
      outgoing = request;
    }
  }

  [_stack addArrangedSubview:Avatar(name, 64.0, me)];
  UILabel* name_label =
      NewLabel(name, UIFontTextStyleTitle3, 20.0, UIFontWeightBold, [XeniaTheme textPrimary]);
  name_label.accessibilityTraits = UIAccessibilityTraitHeader;
  [_stack addArrangedSubview:name_label];
  [_stack setCustomSpacing:2.0 afterView:name_label];

  NSString* detail = nil;
  if (me) {
    detail = social.friendCode.length
                 ? [NSString stringWithFormat:NSLocalizedString(@"Your friend code: %@",
                                                                @"Gamer card for yourself; %@ "
                                                                @"is the code"),
                                              xe_ios_format_friend_code(social.friendCode)]
                 : NSLocalizedString(@"That's you.", @"Gamer card for yourself");
  } else if (peer) {
    detail = [social presenceTextForFriend:peer];
  } else if (incoming) {
    detail = NSLocalizedString(@"Wants to be friends", @"Gamer card: a friend request from them");
  } else if (outgoing) {
    detail = NSLocalizedString(@"Friend request sent", @"Gamer card: you asked them");
  } else if ([self inMyParty]) {
    detail = NSLocalizedString(@"In your party", @"Gamer card");
  }
  if (detail) {
    UILabel* detail_label =
        NewLabel(detail, UIFontTextStyleSubheadline, 14.0, UIFontWeightRegular,
                 [XeniaTheme textSecondary]);
    detail_label.numberOfLines = 2;
    detail_label.textAlignment = NSTextAlignmentCenter;
    [_stack addArrangedSubview:detail_label];
  }
  [_stack setCustomSpacing:14.0 afterView:_stack.arrangedSubviews.lastObject];

  if (me || social.state != XeniaLiveSocialStateOnline) {
    return;
  }
  NSMutableArray<UIButton*>* buttons = [NSMutableArray array];
  const uint64_t xuid = _xuid;
  void (^join)(XeniaLiveFriend*) = _joinFriendHandler;
  if (peer && peer.online && peer.joinable && join) {
    XeniaLiveFriend* held = [[peer retain] autorelease];
    [buttons addObject:Capsule(NSLocalizedString(@"Join Game", @"Gamer card button"),
                               @"arrow.right.circle.fill", YES, YES, ^(UIButton* __unused b) {
                                 join(held);
                               })];
  }
  if (peer && peer.online && social.runningGameOnline) {
    XeniaLiveFriend* held = [[peer retain] autorelease];
    [buttons
        addObject:Capsule(NSLocalizedString(@"Invite to Game", @"Gamer card button"),
                          @"paperplane", NO, YES, ^(UIButton* sender) {
                            if ([[XeniaLiveSocial shared] inviteFriendToRunningGame:held]) {
                              UIButtonConfiguration* config = sender.configuration;
                              config.title = NSLocalizedString(@"Invited", @"Gamer card button "
                                                                           @"after inviting");
                              config.image = [UIImage systemImageNamed:@"checkmark"];
                              sender.configuration = config;
                              sender.enabled = NO;
                            }
                          })];
  }
  if (peer && social.partyActionsAvailable && ![self inMyParty]) {
    XeniaLiveFriend* held = [[peer retain] autorelease];
    [buttons addObject:Capsule(NSLocalizedString(@"Invite to Party", @"Gamer card button"),
                               @"person.3", NO, YES, ^(UIButton* sender) {
                                 if ([[XeniaLiveSocial shared] inviteFriendToParty:held]) {
                                   UIButtonConfiguration* config = sender.configuration;
                                   config.title = NSLocalizedString(
                                       @"Invited to Party", @"Gamer card button after inviting");
                                   config.image = [UIImage systemImageNamed:@"checkmark"];
                                   sender.configuration = config;
                                   sender.enabled = NO;
                                 }
                               })];
  }
  if (!peer) {
    if (incoming) {
      [buttons addObject:Capsule(NSLocalizedString(@"Accept Friend Request", @"Gamer card button"),
                                 @"person.badge.plus", YES, YES, ^(UIButton* __unused b) {
                                   [[XeniaLiveSocial shared] acceptFriendRequest:xuid];
                                 })];
    } else if (!outgoing) {
      [buttons addObject:Capsule(NSLocalizedString(@"Add Friend", @"Gamer card button"),
                                 @"person.badge.plus", NO, YES, ^(UIButton* sender) {
                                   [[XeniaLiveSocial shared] acceptFriendRequest:xuid];
                                   sender.enabled = NO;
                                 })];
    }
  }
  for (UIButton* button in buttons) {
    [_stack addArrangedSubview:button];
    [button.widthAnchor constraintEqualToAnchor:_stack.widthAnchor].active = YES;
  }
}

@end

#pragma mark - Online panel

typedef NS_ENUM(NSInteger, XeniaOnlinePage) {
  XeniaOnlinePageList,
  XeniaOnlinePageParty,
  XeniaOnlinePageInvite,
  XeniaOnlinePageAdd,
  XeniaOnlinePageSearch,
  XeniaOnlinePageCard,
};

@implementation XeniaIOSOnlinePanel {
  UIStackView* _header;
  UIScrollView* _scroll;
  UIStackView* _stack;
  NSLayoutConstraint* _fitHeight;
  XeniaIOSGamerCardView* _card;
  UITextField* _codeField;
  UITextField* _searchField;
  NSString* _query;
  XeniaOnlinePage _page;
  XeniaOnlinePage _cardReturn;
  BOOL _leaveArmed;
  void (^_acceptInviteHandler)(XeniaLiveInvite*);
  void (^_joinFriendHandler)(XeniaLiveFriend*);
  void (^_allFriendsHandler)(void);
  void (^_closeHandler)(void);
  void (^_gamesHandler)(void);
  BOOL _embedded;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.translatesAutoresizingMaskIntoConstraints = NO;
    self.backgroundColor = [UIColor clearColor];
    xe_apply_floating_window_chrome(self);
    self.layer.cornerRadius = kPanelRadius;
    self.layer.cornerCurve = kCACornerCurveContinuous;
    for (UIView* view in self.subviews) {
      if ([view isKindOfClass:[UIVisualEffectView class]]) {
        view.layer.cornerRadius = kPanelRadius;
        view.layer.cornerCurve = kCACornerCurveContinuous;
      }
    }
    self.accessibilityViewIsModal = NO;
    self.accessibilityLabel = NSLocalizedString(@"Friends", @"Online panel, VoiceOver label");

    // The header stays put; the page below it scrolls.
    _header = [[UIStackView alloc] init];
    _header.translatesAutoresizingMaskIntoConstraints = NO;
    _header.axis = UILayoutConstraintAxisVertical;
    _header.spacing = 2.0;
    [self addSubview:_header];

    _scroll = [[UIScrollView alloc] init];
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
    _scroll.showsVerticalScrollIndicator = NO;
    _scroll.alwaysBounceVertical = NO;
    _scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [self addSubview:_scroll];

    _stack = [[UIStackView alloc] init];
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    _stack.axis = UILayoutConstraintAxisVertical;
    _stack.spacing = 8.0;
    [_scroll addSubview:_stack];

    // As tall as its content, unless the space it's given is shorter: then it scrolls.
    _fitHeight = [[_scroll.heightAnchor constraintEqualToAnchor:_stack.heightAnchor
                                                       constant:12.0] retain];
    _fitHeight.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
      [_header.topAnchor constraintEqualToAnchor:self.topAnchor constant:12],
      [_header.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:12],
      [_header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-12],
      [_scroll.topAnchor constraintEqualToAnchor:_header.bottomAnchor constant:6],
      [_scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
      [_scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
      [_scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
      [_stack.topAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.topAnchor],
      [_stack.bottomAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.bottomAnchor
                                          constant:-12],
      [_stack.leadingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.leadingAnchor
                                           constant:12],
      [_stack.trailingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.trailingAnchor
                                            constant:-12],
      _fitHeight,
    ]];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(socialChanged:)
                                                 name:XeniaLiveSocialDidChangeNotification
                                               object:nil];
    [self reload];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_header release];
  [_scroll release];
  [_stack release];
  [_fitHeight release];
  [_card release];
  [_codeField release];
  [_searchField release];
  [_query release];
  [_acceptInviteHandler release];
  [_joinFriendHandler release];
  [_allFriendsHandler release];
  [_closeHandler release];
  [_gamesHandler release];
  [super dealloc];
}

#define XE_COPY_BLOCK_PROPERTY(type, getter, setter, ivar) \
  -(type)getter {                                           \
    return ivar;                                            \
  }                                                         \
  -(void)setter : (type)value {                             \
    if (value != ivar) {                                    \
      [ivar release];                                       \
      ivar = [value copy];                                  \
    }                                                       \
  }

XE_COPY_BLOCK_PROPERTY(void (^)(XeniaLiveInvite*), acceptInviteHandler, setAcceptInviteHandler,
                       _acceptInviteHandler)
XE_COPY_BLOCK_PROPERTY(void (^)(XeniaLiveFriend*), joinFriendHandler, setJoinFriendHandler,
                       _joinFriendHandler)
XE_COPY_BLOCK_PROPERTY(void (^)(void), allFriendsHandler, setAllFriendsHandler,
                       _allFriendsHandler)
XE_COPY_BLOCK_PROPERTY(void (^)(void), closeHandler, setCloseHandler, _closeHandler)
XE_COPY_BLOCK_PROPERTY(void (^)(void), gamesHandler, setGamesHandler, _gamesHandler)

#undef XE_COPY_BLOCK_PROPERTY

- (BOOL)embedded {
  return _embedded;
}

// In a sheet the sheet is the window: drop the floating chrome and let the page fill it.
- (void)setEmbedded:(BOOL)embedded {
  _embedded = embedded;
  self.layer.cornerRadius = embedded ? 0.0 : kPanelRadius;
  self.layer.borderWidth = embedded ? 0.0 : 1.0;
  self.layer.shadowOpacity = embedded ? 0.0f : 0.14f;
  for (UIView* view in self.subviews) {
    if ([view isKindOfClass:[UIVisualEffectView class]]) {
      view.hidden = embedded;
    }
  }
  _fitHeight.active = !embedded;
  [self reload];
}

- (void)socialChanged:(NSNotification* __unused)note {
  // A gamer card refreshes itself; a field being typed in keeps its text.
  if (_page == XeniaOnlinePageCard || _codeField.isFirstResponder ||
      _searchField.isFirstResponder) {
    return;
  }
  [self reload];
}

- (void)clear {
  for (UIView* view in [[_stack.arrangedSubviews copy] autorelease]) {
    [view removeFromSuperview];
  }
}

- (void)clearHeader {
  for (UIView* view in [[_header.arrangedSubviews copy] autorelease]) {
    [view removeFromSuperview];
  }
}

#pragma mark Navigation

- (void)goTo:(XeniaOnlinePage)page {
  [_card release];
  _card = nil;
  _page = page;
  _leaveArmed = NO;
  [self reload];
  [_scroll setContentOffset:CGPointZero animated:NO];
  UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, _header);
}

- (void)showList {
  [_query release];
  _query = nil;
  [self goTo:XeniaOnlinePageList];
}

- (void)backToList:(id __unused)sender {
  [self showList];
}

- (void)backToParty:(id __unused)sender {
  [self goTo:XeniaOnlinePageParty];
}

- (void)backFromCard:(id __unused)sender {
  [self goTo:_cardReturn];
}

- (void)showParty {
  [self goTo:XeniaOnlinePageParty];
}

- (void)showInvite {
  [self goTo:XeniaOnlinePageInvite];
}

- (void)showAdd {
  [self goTo:XeniaOnlinePageAdd];
}

- (void)showSearch {
  [self goTo:XeniaOnlinePageSearch];
  [_searchField becomeFirstResponder];
}

- (void)showCardForXuid:(uint64_t)xuid gamertag:(NSString*)gamertag {
  if (_page != XeniaOnlinePageCard) {
    _cardReturn = _page;
  }
  [_searchField resignFirstResponder];
  [_card release];
  _card = nil;
  _page = XeniaOnlinePageCard;
  [self clearHeader];
  [self clear];
  [_header addArrangedSubview:[self navHeaderBack:[self titleForPage:_cardReturn]
                                           action:@selector(backFromCard:)
                                            title:nil]];
  _card = [[XeniaIOSGamerCardView alloc] initWithXuid:xuid gamertag:gamertag];
  _card.joinFriendHandler = _joinFriendHandler;
  [_stack addArrangedSubview:_card];
  [_scroll setContentOffset:CGPointZero animated:NO];
  UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, _card);
}

- (NSString*)titleForPage:(XeniaOnlinePage)page {
  switch (page) {
    case XeniaOnlinePageParty:
      return NSLocalizedString(@"Party", @"Online panel: back to the party page");
    case XeniaOnlinePageInvite:
      return NSLocalizedString(@"Invite", @"Online panel: back to the invite page");
    case XeniaOnlinePageAdd:
      return NSLocalizedString(@"Add Friend", @"Online panel: back to the add-friend page");
    case XeniaOnlinePageSearch:
      return NSLocalizedString(@"Search", @"Online panel: back to the search page");
    default:
      return NSLocalizedString(@"Friends", @"Online panel: back to the list");
  }
}

#pragma mark Header pieces

- (UIButton*)backButtonTo:(NSString*)title action:(SEL)action {
  UIButton* back = [UIButton buttonWithType:UIButtonTypeSystem];
  back.translatesAutoresizingMaskIntoConstraints = NO;
  UIButtonConfiguration* config = [UIButtonConfiguration plainButtonConfiguration];
  config.title = title;
  config.image = [UIImage systemImageNamed:@"chevron.left"];
  config.imagePadding = 2.0;
  config.baseForegroundColor = InviteBlue();
  config.contentInsets = NSDirectionalEdgeInsetsMake(4, 0, 4, 0);
  config.preferredSymbolConfigurationForImage = [UIImageSymbolConfiguration
      configurationWithTextStyle:UIFontTextStyleSubheadline
                           scale:UIImageSymbolScaleMedium];
  UIFont* font = xe_scaled_system_font(UIFontTextStyleBody, 16.0, UIFontWeightRegular);
  config.titleTextAttributesTransformer =
      [[^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
        NSMutableDictionary<NSAttributedStringKey, id>* out = [[in mutableCopy] autorelease];
        out[NSFontAttributeName] = font;
        return out;
      } copy] autorelease];
  back.configuration = config;
  back.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
  [back addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  return back;
}

// "‹ Online" on the left, a centred title, and room on the right to balance.
- (UIView*)navHeaderBack:(NSString*)back_title action:(SEL)action title:(NSString*)title {
  UIButton* back = [self backButtonTo:back_title action:action];
  [back.widthAnchor constraintEqualToConstant:92.0].active = YES;
  UILabel* label =
      NewLabel(title, UIFontTextStyleHeadline, 17.0, UIFontWeightSemibold, [XeniaTheme textPrimary]);
  label.textAlignment = NSTextAlignmentCenter;
  label.accessibilityTraits = UIAccessibilityTraitHeader;
  UIView* balance = [[[UIView alloc] init] autorelease];
  balance.translatesAutoresizingMaskIntoConstraints = NO;
  [balance.widthAnchor constraintEqualToConstant:92.0].active = YES;
  UIStackView* row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ back, label, balance ]] autorelease];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  [row.heightAnchor constraintGreaterThanOrEqualToConstant:36.0].active = YES;
  return row;
}

- (UIView*)titleHeader:(NSString*)title icons:(NSArray<UIView*>*)icons {
  UILabel* label =
      NewLabel(title, UIFontTextStyleTitle2, 22.0, UIFontWeightBold, [XeniaTheme textPrimary]);
  label.accessibilityTraits = UIAccessibilityTraitHeader;
  [label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                         forAxis:UILayoutConstraintAxisHorizontal];
  NSMutableArray<UIView*>* items = [NSMutableArray arrayWithObject:label];
  UIView* spacer = [[[UIView alloc] init] autorelease];
  spacer.translatesAutoresizingMaskIntoConstraints = NO;
  [spacer setContentHuggingPriority:UILayoutPriorityDefaultLow
                            forAxis:UILayoutConstraintAxisHorizontal];
  [items addObject:spacer];
  [items addObjectsFromArray:icons];
  UIStackView* row = [[[UIStackView alloc] initWithArrangedSubviews:items] autorelease];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 8.0;
  [row.heightAnchor constraintGreaterThanOrEqualToConstant:36.0].active = YES;
  return row;
}

- (UILabel*)caption:(NSString*)text center:(BOOL)center {
  UILabel* label = NewLabel(text, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular,
                            [XeniaTheme textSecondary]);
  label.numberOfLines = 0;
  if (center) {
    label.textAlignment = NSTextAlignmentCenter;
  }
  return label;
}

#pragma mark Building

- (void)reload {
  if (_page == XeniaOnlinePageCard && _card) {
    return;
  }
  [self clearHeader];
  [self clear];
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  if (social.state != XeniaLiveSocialStateOnline) {
    [_header addArrangedSubview:[self titleHeader:NSLocalizedString(@"Friends",
                                                                    @"Online panel title")
                                            icons:@[]]];
    [_stack addArrangedSubview:[self caption:social.statusText center:NO]];
    return;
  }
  switch (_page) {
    case XeniaOnlinePageParty:
      [self buildParty];
      break;
    case XeniaOnlinePageInvite:
      [self buildInvite];
      break;
    case XeniaOnlinePageAdd:
      [self buildAdd];
      break;
    case XeniaOnlinePageSearch:
      [self buildSearch];
      break;
    default:
      [self buildList];
      break;
  }
}

- (void (^)(void))cardOpener:(uint64_t)xuid gamertag:(NSString*)gamertag {
  __unsafe_unretained XeniaIOSOnlinePanel* unsafe_self = self;
  NSString* name = [[gamertag copy] autorelease];
  return [[^{
    [unsafe_self showCardForXuid:xuid gamertag:name];
  } copy] autorelease];
}

// Join (white) for a friend in a game you can join; Invite while yours is online.
- (UIButton*)actionForFriend:(XeniaLiveFriend*)peer {
  __unsafe_unretained XeniaIOSOnlinePanel* unsafe_self = self;
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  XeniaLiveFriend* held = [[peer retain] autorelease];
  if (peer.joinable && ![social friendInYourGame:peer]) {
    return Capsule(NSLocalizedString(@"Join", @"Online panel button: join a friend's game"), nil,
                   YES, NO, ^(UIButton* __unused b) {
                     if (unsafe_self->_joinFriendHandler) {
                       unsafe_self->_joinFriendHandler(held);
                     }
                   });
  }
  if (social.runningGameOnline && ![social friendInYourGame:peer]) {
    return Capsule(NSLocalizedString(@"Invite", @"Online panel button"), nil, NO, NO,
                   ^(UIButton* sender) {
                     if ([[XeniaLiveSocial shared] inviteFriendToRunningGame:held]) {
                       UIButtonConfiguration* config = sender.configuration;
                       config.title =
                           NSLocalizedString(@"Invited", @"Online panel button after inviting");
                       sender.configuration = config;
                       sender.enabled = NO;
                     }
                   });
  }
  return nil;
}

// A row of friends for a group, each with its own action.
- (NSArray<UIView*>*)rowsForFriends:(NSArray<XeniaLiveFriend*>*)friends {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  NSMutableArray<UIView*>* rows = [NSMutableArray array];
  for (XeniaLiveFriend* peer in friends) {
    UIButton* action = peer.online ? [self actionForFriend:peer] : nil;
    NSString* detail = peer.online ? [social presenceTextForFriend:peer]
                                   : NSLocalizedString(@"Offline", @"Online panel: a friend who "
                                                                   @"is offline");
    [rows addObject:PlayerRow(peer.gamertag, detail, NO, NO, peer.online,
                              action ? @[ action ] : @[],
                              [self cardOpener:peer.xuid gamertag:peer.gamertag])];
  }
  return rows;
}

- (UIView*)avatarStackFor:(NSArray<NSString*>*)names {
  UIView* box = [[[UIView alloc] init] autorelease];
  box.translatesAutoresizingMaskIntoConstraints = NO;
  const CGFloat size = 28.0;
  const CGFloat step = 19.0;
  const NSUInteger count = MIN(names.count, (NSUInteger)4);
  for (NSUInteger i = 0; i < count; ++i) {
    UIView* avatar = Avatar(names[i], size, NO);
    avatar.layer.borderWidth = 2.0;
    avatar.layer.borderColor = [UIColor colorWithWhite:0.2 alpha:1.0].CGColor;
    [box addSubview:avatar];
    [NSLayoutConstraint activateConstraints:@[
      [avatar.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:i * step],
      [avatar.centerYAnchor constraintEqualToAnchor:box.centerYAnchor],
    ]];
  }
  [NSLayoutConstraint activateConstraints:@[
    [box.widthAnchor constraintEqualToConstant:size + (MAX(count, (NSUInteger)1) - 1) * step],
    [box.heightAnchor constraintEqualToConstant:36.0],
  ]];
  box.isAccessibilityElement = NO;
  return box;
}

- (UIView*)iconTile:(NSString*)symbol {
  UIView* tile = [[[UIView alloc] init] autorelease];
  tile.translatesAutoresizingMaskIntoConstraints = NO;
  tile.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.12];
  tile.layer.cornerRadius = 18.0;
  UIImageView* image = [[[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:symbol]] autorelease];
  image.translatesAutoresizingMaskIntoConstraints = NO;
  image.tintColor = [XeniaTheme textPrimary];
  image.preferredSymbolConfiguration =
      [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleBody];
  [tile addSubview:image];
  [NSLayoutConstraint activateConstraints:@[
    [tile.widthAnchor constraintEqualToConstant:36.0],
    [tile.heightAnchor constraintEqualToConstant:36.0],
    [image.centerXAnchor constraintEqualToAnchor:tile.centerXAnchor],
    [image.centerYAnchor constraintEqualToAnchor:tile.centerYAnchor],
  ]];
  tile.isAccessibilityElement = NO;
  return tile;
}

- (void)buildList {
  __unsafe_unretained XeniaIOSOnlinePanel* unsafe_self = self;
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  const NSInteger requests = (NSInteger)social.incomingRequests.count;
  NSMutableArray<UIView*>* icons = [NSMutableArray array];
  [icons addObject:IconButton(@"magnifyingglass",
                              NSLocalizedString(@"Search friends", @"Online panel: search button"),
                              0, ^{ [unsafe_self showSearch]; })];
  [icons addObject:IconButton(@"person.badge.plus",
                              NSLocalizedString(@"Add friend", @"Online panel: add-friend button"),
                              requests, ^{ [unsafe_self showAdd]; })];
  if (_embedded && _closeHandler) {
    void (^close)(void) = [[_closeHandler copy] autorelease];
    [icons addObject:IconButton(@"xmark", NSLocalizedString(@"Close", @"Online sheet: close button"),
                                0, close)];
  }
  [_header addArrangedSubview:[self titleHeader:NSLocalizedString(@"Friends", @"Online panel title")
                                          icons:icons]];
  const NSInteger online_count = social.onlineFriendCount;
  NSString* subtitle =
      online_count == 0
          ? NSLocalizedString(@"No friends online", @"Online panel: subtitle")
      : online_count == 1
          ? NSLocalizedString(@"1 friend online", @"Online panel: subtitle")
          : [NSString localizedStringWithFormat:NSLocalizedString(@"%ld friends online",
                                                                  @"Online panel: subtitle; %ld "
                                                                  @"is how many"),
                                                long(online_count)];
  UILabel* sub = [self caption:subtitle center:NO];
  sub.accessibilityLabel = subtitle;
  [_header addArrangedSubview:sub];

  // Invites waiting, in blue.
  NSArray<XeniaLiveInvite*>* invites = social.invites;
  if (invites.count) {
    NSMutableArray<UIView*>* rows = [NSMutableArray array];
    for (XeniaLiveInvite* invite in invites) {
      XeniaLiveInvite* held = [[invite retain] autorelease];
      NSString* detail = invite.party ? NSLocalizedString(@"Invited you to a party",
                                                          @"Online panel invite")
                                      : [social nameForTitle:invite.titleId];
      UIButton* decline = DeclineButton(NSLocalizedString(@"Decline", @"Online panel: decline "
                                                                      @"invite"),
                                        ^{ [[XeniaLiveSocial shared] declineInvite:held]; });
      UIButton* join =
          Capsule(invite.party ? NSLocalizedString(@"Join Party", @"Online panel invite button")
                               : NSLocalizedString(@"Join", @"Online panel invite button"),
                  nil, YES, NO, ^(UIButton* __unused b) {
                    if (held.party) {
                      [[XeniaLiveSocial shared] acceptPartyInvite:held];
                    } else if (unsafe_self->_acceptInviteHandler) {
                      unsafe_self->_acceptInviteHandler(held);
                    }
                  });
      [rows addObject:PlayerRow(invite.gamertag, detail, NO, NO, YES, @[ decline, join ],
                                [self cardOpener:invite.fromXuid gamertag:invite.gamertag])];
    }
    [_stack addArrangedSubview:Group(rows, [InviteBlue() colorWithAlphaComponent:0.24])];
  }

  // The party, which can be in other games, or a way to start one.
  if (social.partyActionsAvailable) {
    NSArray<XeniaLivePartyMember*>* members = social.partyMembers;
    UIView* row;
    if (members.count) {
      NSMutableArray<NSString*>* names = [NSMutableArray array];
      for (XeniaLivePartyMember* member in members) {
        [names addObject:member.gamertag ?: @"?"];
      }
      NSString* detail =
          members.count == 1
              ? NSLocalizedString(@"Just you", @"Online panel: a party with only you")
              : [NSString localizedStringWithFormat:NSLocalizedString(@"%ld players",
                                                                      @"Online panel: party size"),
                                                    long(members.count)];
      row = Row([self avatarStackFor:names], NSLocalizedString(@"Party", @"Online panel row"),
                detail, NO, @[ Chevron() ], ^{ [unsafe_self showParty]; });
    } else {
      row = Row([self iconTile:@"person.3.fill"],
                NSLocalizedString(@"Start a Party", @"Online panel row"),
                NSLocalizedString(@"Play together", @"Online panel: under Start a Party"), NO,
                @[ Chevron() ], ^{
                  [[XeniaLiveSocial shared] createParty];
                  [unsafe_self showParty];
                });
    }
    [_stack addArrangedSubview:Group(@[ row ], nil)];
  }

  // Online friends: those in your game first, then who's playing, then the rest.
  NSMutableArray<XeniaLiveFriend*>* in_game = [NSMutableArray array];
  NSMutableArray<XeniaLiveFriend*>* playing = [NSMutableArray array];
  NSMutableArray<XeniaLiveFriend*>* idle = [NSMutableArray array];
  for (XeniaLiveFriend* peer in social.friends) {
    if (!peer.online) {
      continue;
    }
    if ([social friendInYourGame:peer]) {
      [in_game addObject:peer];
    } else if (peer.titleId) {
      [playing addObject:peer];
    } else {
      [idle addObject:peer];
    }
  }
  NSArray* sections = @[
    @[ NSLocalizedString(@"In Your Game", @"Online panel section"), in_game ],
    @[ NSLocalizedString(@"Playing", @"Online panel section"), playing ],
    @[ NSLocalizedString(@"Online", @"Online panel section: friends online"), idle ],
  ];
  for (NSArray* section in sections) {
    NSArray<XeniaLiveFriend*>* friends = section[1];
    if (!friends.count) {
      continue;
    }
    [_stack addArrangedSubview:SectionLabel(section[0])];
    [_stack addArrangedSubview:Group([self rowsForFriends:friends], nil)];
  }
  if (_embedded && _gamesHandler) {
    void (^open_games)(void) = [[_gamesHandler copy] autorelease];
    const NSInteger players = xe_ios_system_link_online_total();
    NSString* detail =
        !xe_ios_system_link_online_connected()
            ? NSLocalizedString(@"Not connected", @"Online panel: games row while the lobby "
                                                  @"doesn't answer")
        : players > 0 ? [NSString localizedStringWithFormat:
                                      NSLocalizedString(@"%ld playing now",
                                                        @"Online panel: games row; %ld is how many "
                                                        @"players are in public lobbies"),
                                      long(players)]
                      : NSLocalizedString(@"Nobody's playing yet", @"Online panel: games row");
    UIImageView* symbol = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:@"gamecontroller.fill"]] autorelease];
    symbol.tintColor = [UIColor whiteColor];
    symbol.contentMode = UIViewContentModeCenter;
    symbol.translatesAutoresizingMaskIntoConstraints = NO;
    UIView* tile = [[[UIView alloc] init] autorelease];
    tile.translatesAutoresizingMaskIntoConstraints = NO;
    tile.backgroundColor = HexColor(0x5E5CE6);
    tile.layer.cornerRadius = 10.0;
    [tile addSubview:symbol];
    [NSLayoutConstraint activateConstraints:@[
      [tile.widthAnchor constraintEqualToConstant:36.0],
      [tile.heightAnchor constraintEqualToConstant:36.0],
      [symbol.centerXAnchor constraintEqualToAnchor:tile.centerXAnchor],
      [symbol.centerYAnchor constraintEqualToAnchor:tile.centerYAnchor],
    ]];
    tile.isAccessibilityElement = NO;
    [_stack addArrangedSubview:SectionLabel(NSLocalizedString(@"Games Online", @"Online panel section"))];
    [_stack addArrangedSubview:Group(@[ Row(tile, NSLocalizedString(@"Public Games", @"Online panel row"),
                                           detail, NO, @[ Chevron() ], open_games) ],
                                     nil)];
  }
  if (!in_game.count && !playing.count && !idle.count && !invites.count) {
    UILabel* empty = [self caption:NSLocalizedString(@"Nobody's online right now. Search to see "
                                                     @"all your friends, or add one with a code.",
                                                     @"Online panel: no friends online")
                            center:YES];
    [_stack addArrangedSubview:empty];
  }
}

- (void)buildParty {
  __unsafe_unretained XeniaIOSOnlinePanel* unsafe_self = self;
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  NSArray<XeniaLivePartyMember*>* members = social.partyMembers;
  if (!members.count) {
    _page = XeniaOnlinePageList;
    [self buildList];
    return;
  }
  [_header addArrangedSubview:[self navHeaderBack:NSLocalizedString(@"Online", @"Online panel: "
                                                                                @"back to the list")
                                           action:@selector(backToList:)
                                            title:NSLocalizedString(@"Party",
                                                                    @"Online panel: party page")]];
  XeniaLivePartyMember* leader = nil;
  for (XeniaLivePartyMember* member in members) {
    if (member.leader) {
      leader = member;
    }
  }
  NSString* count = members.count == 1
                        ? NSLocalizedString(@"Just you so far", @"Online panel: party page")
                        : [NSString localizedStringWithFormat:NSLocalizedString(@"%ld players",
                                                                                @"Online panel: "
                                                                                @"party size"),
                                                              long(members.count)];
  NSString* subtitle = (leader && !leader.me && members.count > 1)
                           ? [NSString stringWithFormat:NSLocalizedString(@"%@ · %@ leads",
                                                                          @"Online panel: party "
                                                                          @"size, then who "
                                                                          @"leads"),
                                                        count, leader.gamertag]
                           : count;
  [_header addArrangedSubview:[self caption:subtitle center:YES]];

  // Four to a row.
  for (NSUInteger start = 0; start < members.count; start += 4) {
    NSMutableArray<UIView*>* cells = [NSMutableArray array];
    for (NSUInteger i = start; i < start + 4; ++i) {
      if (i >= members.count) {
        UIView* empty = [[[UIView alloc] init] autorelease];
        empty.translatesAutoresizingMaskIntoConstraints = NO;
        [cells addObject:empty];
        continue;
      }
      XeniaLivePartyMember* member = members[i];
      XeniaLiveFriend* peer = member.me ? nil : [social friendWithXuid:member.xuid];
      UIView* avatar = AvatarWithPresence(member.gamertag, 48.0, member.me,
                                          member.me || peer.online);
      UIView* top = avatar;
      if (member.leader) {
        UIView* box = [[[UIView alloc] init] autorelease];
        box.translatesAutoresizingMaskIntoConstraints = NO;
        [box addSubview:avatar];
        UIView* crown = [[[UIView alloc] init] autorelease];
        crown.translatesAutoresizingMaskIntoConstraints = NO;
        crown.backgroundColor = [XeniaTheme statusWarning];
        crown.layer.cornerRadius = 10.0;
        UIImageView* glyph = [[[UIImageView alloc]
            initWithImage:[UIImage systemImageNamed:@"crown.fill"]] autorelease];
        glyph.translatesAutoresizingMaskIntoConstraints = NO;
        glyph.tintColor = [UIColor blackColor];
        glyph.preferredSymbolConfiguration =
            [UIImageSymbolConfiguration configurationWithPointSize:10.0];
        [crown addSubview:glyph];
        [box addSubview:crown];
        [NSLayoutConstraint activateConstraints:@[
          [avatar.topAnchor constraintEqualToAnchor:box.topAnchor constant:4],
          [avatar.leadingAnchor constraintEqualToAnchor:box.leadingAnchor],
          [avatar.bottomAnchor constraintEqualToAnchor:box.bottomAnchor],
          [avatar.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-4],
          [crown.widthAnchor constraintEqualToConstant:20],
          [crown.heightAnchor constraintEqualToConstant:20],
          [crown.topAnchor constraintEqualToAnchor:box.topAnchor],
          [crown.trailingAnchor constraintEqualToAnchor:box.trailingAnchor],
          [glyph.centerXAnchor constraintEqualToAnchor:crown.centerXAnchor],
          [glyph.centerYAnchor constraintEqualToAnchor:crown.centerYAnchor],
        ]];
        top = box;
      }
      NSString* shown = member.me ? NSLocalizedString(@"You", @"Online panel: your own cell")
                                  : member.gamertag;
      UILabel* name = NewLabel(shown, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular,
                               [XeniaTheme textPrimary]);
      name.textAlignment = NSTextAlignmentCenter;
      name.lineBreakMode = NSLineBreakByTruncatingTail;
      name.accessibilityLabel = member.leader
                                    ? [NSString stringWithFormat:NSLocalizedString(
                                                                     @"%@, party leader",
                                                                     @"Online panel, VoiceOver; "
                                                                     @"%@ is the gamertag"),
                                                                 shown]
                                    : shown;
      UIStackView* cell =
          [[[UIStackView alloc] initWithArrangedSubviews:@[ top, name ]] autorelease];
      cell.axis = UILayoutConstraintAxisVertical;
      cell.alignment = UIStackViewAlignmentCenter;
      cell.spacing = 4.0;
      cell.isAccessibilityElement = NO;
      [name.widthAnchor constraintEqualToAnchor:cell.widthAnchor].active = YES;
      if (!member.me) {
        void (^open)(void) = [self cardOpener:member.xuid gamertag:member.gamertag];
        UIButton* hit = [UIButton buttonWithType:UIButtonTypeCustom];
        hit.translatesAutoresizingMaskIntoConstraints = NO;
        hit.accessibilityLabel = name.accessibilityLabel;
        [hit addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
               open();
             }]
      forControlEvents:UIControlEventTouchUpInside];
        [cell addSubview:hit];
        [NSLayoutConstraint activateConstraints:@[
          [hit.topAnchor constraintEqualToAnchor:cell.topAnchor],
          [hit.bottomAnchor constraintEqualToAnchor:cell.bottomAnchor],
          [hit.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor],
          [hit.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor],
        ]];
      }
      [cells addObject:cell];
    }
    UIStackView* row = [[[UIStackView alloc] initWithArrangedSubviews:cells] autorelease];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.distribution = UIStackViewDistributionFillEqually;
    row.alignment = UIStackViewAlignmentTop;
    row.spacing = 4.0;
    [_stack addArrangedSubview:row];
    [_stack setCustomSpacing:12.0 afterView:row];
  }

  UIButton* invite =
      Capsule(NSLocalizedString(@"Invite", @"Online panel button: invite friends to the party"),
              @"person.badge.plus", YES, YES, ^(UIButton* __unused b) { [unsafe_self showInvite]; });
  UIButton* leave = Capsule(NSLocalizedString(@"Leave", @"Online panel button: leave the party"),
                            nil, NO, YES, nil);
  UIButtonConfiguration* leave_config = leave.configuration;
  leave_config.baseForegroundColor = [XeniaTheme statusError];
  leave.configuration = leave_config;
  [leave addAction:[UIAction actionWithHandler:^(__kindof UIAction* action) {
           UIButton* button = (UIButton*)action.sender;
           if (!unsafe_self->_leaveArmed) {
             // One more tap to leave.
             unsafe_self->_leaveArmed = YES;
             UIButtonConfiguration* config = button.configuration;
             config.title = NSLocalizedString(@"Tap to Leave", @"Online panel: confirm leaving "
                                                               @"the party");
             button.configuration = config;
             return;
           }
           unsafe_self->_leaveArmed = NO;
           [[XeniaLiveSocial shared] leaveParty];
           [unsafe_self showList];
         }]
 forControlEvents:UIControlEventTouchUpInside];
  UIStackView* actions =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ invite, leave ]] autorelease];
  actions.axis = UILayoutConstraintAxisHorizontal;
  actions.distribution = UIStackViewDistributionFillEqually;
  actions.spacing = 8.0;
  [_stack addArrangedSubview:actions];
}

- (void)buildInvite {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  [_header addArrangedSubview:[self navHeaderBack:NSLocalizedString(@"Party", @"Online panel: "
                                                                              @"back to the party")
                                           action:@selector(backToParty:)
                                            title:NSLocalizedString(@"Invite",
                                                                    @"Online panel: invite page")]];
  NSMutableArray<UIView*>* rows = [NSMutableArray array];
  for (XeniaLiveFriend* peer in social.friends) {
    if (!peer.online) {
      continue;
    }
    BOOL member = NO;
    for (XeniaLivePartyMember* m in social.partyMembers) {
      member = member || m.xuid == peer.xuid;
    }
    if (member) {
      continue;
    }
    XeniaLiveFriend* held = [[peer retain] autorelease];
    UIButton* invite = Capsule(NSLocalizedString(@"Invite", @"Online panel button"), nil, NO, NO,
                               ^(UIButton* sender) {
                                 if ([[XeniaLiveSocial shared] inviteFriendToParty:held]) {
                                   UIButtonConfiguration* config = sender.configuration;
                                   config.title = NSLocalizedString(@"Invited", @"Online panel "
                                                                                @"button after "
                                                                                @"inviting");
                                   sender.configuration = config;
                                   sender.enabled = NO;
                                 }
                               });
    [rows addObject:PlayerRow(peer.gamertag, [social presenceTextForFriend:peer], NO, NO, YES,
                              @[ invite ], [self cardOpener:peer.xuid gamertag:peer.gamertag])];
  }
  if (rows.count) {
    [_stack addArrangedSubview:Group(rows, nil)];
  } else {
    [_stack addArrangedSubview:[self caption:NSLocalizedString(@"No friends are online to "
                                                               @"invite.",
                                                               @"Online panel: nobody to invite")
                                      center:YES]];
  }
}

- (void)buildAdd {
  __unsafe_unretained XeniaIOSOnlinePanel* unsafe_self = self;
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  [_header addArrangedSubview:[self navHeaderBack:NSLocalizedString(@"Online", @"Online panel: "
                                                                                @"back to the list")
                                           action:@selector(backToList:)
                                            title:NSLocalizedString(@"Add Friend",
                                                                    @"Online panel: add-friend "
                                                                    @"page")]];

  // Your own code, to read out or copy.
  if (social.friendCode.length) {
    NSString* formatted = xe_ios_format_friend_code(social.friendCode);
    UILabel* mine_title = [self caption:NSLocalizedString(@"Your code", @"Online panel")
                                 center:YES];
    UILabel* mine = NewLabel(formatted, UIFontTextStyleTitle2, 24.0, UIFontWeightBold,
                             [XeniaTheme textPrimary]);
    mine.textAlignment = NSTextAlignmentCenter;
    UIButton* copy = Capsule(NSLocalizedString(@"Copy", @"Online panel: copy your friend code"),
                             @"doc.on.doc", NO, NO, ^(UIButton* sender) {
                               [UIPasteboard generalPasteboard].string = formatted;
                               UIButtonConfiguration* config = sender.configuration;
                               config.title = NSLocalizedString(@"Copied", @"Online panel: after "
                                                                           @"copying");
                               config.image = [UIImage systemImageNamed:@"checkmark"];
                               sender.configuration = config;
                             });
    UIStackView* card =
        [[[UIStackView alloc] initWithArrangedSubviews:@[ mine_title, mine, copy ]] autorelease];
    card.axis = UILayoutConstraintAxisVertical;
    card.alignment = UIStackViewAlignmentCenter;
    card.spacing = 6.0;
    card.layoutMargins = UIEdgeInsetsMake(12, 12, 12, 12);
    card.layoutMarginsRelativeArrangement = YES;
    [_stack addArrangedSubview:Group(@[ card ], nil)];
  }

  // Add by code: a field and a button.
  if (!_codeField) {
    _codeField = [[UITextField alloc] init];
    _codeField.translatesAutoresizingMaskIntoConstraints = NO;
    _codeField.placeholder = NSLocalizedString(@"Friend code", @"Online panel: add-friend field");
    _codeField.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters;
    _codeField.autocorrectionType = UITextAutocorrectionTypeNo;
    _codeField.spellCheckingType = UITextSpellCheckingTypeNo;
    _codeField.returnKeyType = UIReturnKeyDone;
    _codeField.textColor = [XeniaTheme textPrimary];
    _codeField.font = xe_scaled_system_font(UIFontTextStyleBody, 16.0, UIFontWeightMedium);
    _codeField.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.1];
    _codeField.layer.cornerRadius = 14.0;
    _codeField.layer.cornerCurve = kCACornerCurveContinuous;
    _codeField.leftView = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 14, 1)] autorelease];
    _codeField.leftViewMode = UITextFieldViewModeAlways;
    [_codeField.heightAnchor constraintEqualToConstant:44.0].active = YES;
    [_codeField addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
                  [unsafe_self addFriendFromField];
                }]
         forControlEvents:UIControlEventEditingDidEndOnExit];
  }
  UIButton* add = Capsule(NSLocalizedString(@"Add", @"Online panel: add a friend by code"), nil,
                          YES, NO, ^(UIButton* __unused b) { [unsafe_self addFriendFromField]; });
  UIStackView* add_row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ _codeField, add ]] autorelease];
  add_row.axis = UILayoutConstraintAxisHorizontal;
  add_row.alignment = UIStackViewAlignmentCenter;
  add_row.spacing = 8.0;
  [_stack addArrangedSubview:add_row];

  NSArray<XeniaLiveFriend*>* incoming = social.incomingRequests;
  if (incoming.count) {
    [_stack addArrangedSubview:SectionLabel([NSString
                                   stringWithFormat:NSLocalizedString(@"Requests · %ld",
                                                                      @"Online panel section; %ld "
                                                                      @"is how many"),
                                                    long(incoming.count)])];
    NSMutableArray<UIView*>* rows = [NSMutableArray array];
    for (XeniaLiveFriend* peer in incoming) {
      const uint64_t xuid = peer.xuid;
      UIButton* decline = DeclineButton(NSLocalizedString(@"Decline", @"Online panel: decline "
                                                                      @"request"),
                                        ^{ [[XeniaLiveSocial shared] declineFriendRequest:xuid]; });
      UIButton* accept = Capsule(NSLocalizedString(@"Accept", @"Online panel: accept request"),
                                 nil, YES, NO, ^(UIButton* __unused b) {
                                   [[XeniaLiveSocial shared] acceptFriendRequest:xuid];
                                 });
      [rows addObject:PlayerRow(peer.gamertag,
                                NSLocalizedString(@"Wants to be friends",
                                                  @"Online panel: a friend request"),
                                NO, NO, NO, @[ decline, accept ],
                                [self cardOpener:peer.xuid gamertag:peer.gamertag])];
    }
    [_stack addArrangedSubview:Group(rows, nil)];
  }
  NSArray<XeniaLiveFriend*>* outgoing = social.outgoingRequests;
  if (outgoing.count) {
    [_stack addArrangedSubview:SectionLabel(NSLocalizedString(@"Sent", @"Online panel section: "
                                                                       @"friend requests sent"))];
    NSMutableArray<UIView*>* rows = [NSMutableArray array];
    for (XeniaLiveFriend* peer in outgoing) {
      [rows addObject:PlayerRow(peer.gamertag, NSLocalizedString(@"Waiting for them",
                                                                 @"Online panel: sent request"),
                                NO, NO, NO, @[], nil)];
    }
    [_stack addArrangedSubview:Group(rows, nil)];
  }
}

- (void)buildSearch {
  __unsafe_unretained XeniaIOSOnlinePanel* unsafe_self = self;
  if (!_searchField) {
    _searchField = [[UITextField alloc] init];
    _searchField.translatesAutoresizingMaskIntoConstraints = NO;
    _searchField.placeholder = NSLocalizedString(@"Search friends", @"Online panel: search field");
    _searchField.autocorrectionType = UITextAutocorrectionTypeNo;
    _searchField.spellCheckingType = UITextSpellCheckingTypeNo;
    _searchField.returnKeyType = UIReturnKeySearch;
    _searchField.clearButtonMode = UITextFieldViewModeWhileEditing;
    _searchField.textColor = [XeniaTheme textPrimary];
    _searchField.tintColor = InviteBlue();
    _searchField.font = xe_scaled_system_font(UIFontTextStyleBody, 16.0, UIFontWeightRegular);
    _searchField.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.1];
    _searchField.layer.cornerRadius = 12.0;
    _searchField.layer.cornerCurve = kCACornerCurveContinuous;
    UIImageView* glass = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:@"magnifyingglass"]] autorelease];
    glass.tintColor = [XeniaTheme textSecondary];
    glass.contentMode = UIViewContentModeCenter;
    glass.frame = CGRectMake(0, 0, 34, 36);
    _searchField.leftView = glass;
    _searchField.leftViewMode = UITextFieldViewModeAlways;
    [_searchField.heightAnchor constraintEqualToConstant:36.0].active = YES;
    [_searchField addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
                    [unsafe_self searchTextChanged];
                  }]
           forControlEvents:UIControlEventEditingChanged];
    [_searchField addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
                    [unsafe_self->_searchField resignFirstResponder];
                  }]
           forControlEvents:UIControlEventEditingDidEndOnExit];
  }
  _searchField.text = _query ?: @"";
  UIButton* cancel = [UIButton buttonWithType:UIButtonTypeSystem];
  cancel.translatesAutoresizingMaskIntoConstraints = NO;
  [cancel setTitle:NSLocalizedString(@"Cancel", @"Online panel: leave search") forState:UIControlStateNormal];
  cancel.tintColor = InviteBlue();
  cancel.titleLabel.font = xe_scaled_system_font(UIFontTextStyleBody, 16.0, UIFontWeightRegular);
  [cancel addTarget:self action:@selector(cancelSearch:) forControlEvents:UIControlEventTouchUpInside];
  [cancel setContentHuggingPriority:UILayoutPriorityRequired
                            forAxis:UILayoutConstraintAxisHorizontal];
  UIStackView* row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ _searchField, cancel ]] autorelease];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 10.0;
  [_header addArrangedSubview:row];
  [self buildSearchResults];
}

- (void)cancelSearch:(id __unused)sender {
  [_searchField resignFirstResponder];
  [self showList];
}

- (void)searchTextChanged {
  [_query release];
  _query = [_searchField.text copy];
  [self clear];
  [self buildSearchResults];
}

// Every friend, online first, filtered by what's typed.
- (void)buildSearchResults {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  NSMutableArray<XeniaLiveFriend*>* matches = [NSMutableArray array];
  for (XeniaLiveFriend* peer in social.friends) {
    if (!_query.length || [peer.gamertag localizedCaseInsensitiveContainsString:_query]) {
      [matches addObject:peer];
    }
  }
  [matches sortUsingComparator:^NSComparisonResult(XeniaLiveFriend* a, XeniaLiveFriend* b) {
    if (a.online != b.online) {
      return a.online ? NSOrderedAscending : NSOrderedDescending;
    }
    return [a.gamertag localizedCaseInsensitiveCompare:b.gamertag];
  }];
  if (matches.count) {
    NSString* count = matches.count == 1 ? NSLocalizedString(@"1 friend", @"Online panel: search "
                                                                          @"results")
                                         : [NSString localizedStringWithFormat:
                                                         NSLocalizedString(@"%ld friends",
                                                                           @"Online panel: search "
                                                                           @"results; %ld is how "
                                                                           @"many"),
                                                         long(matches.count)];
    [_stack addArrangedSubview:SectionLabel(count)];
    [_stack addArrangedSubview:Group([self rowsForFriends:matches], nil)];
  } else {
    [_stack addArrangedSubview:[self caption:social.friends.count
                                                 ? NSLocalizedString(@"No friends match.",
                                                                     @"Online panel: search found "
                                                                     @"nobody")
                                                 : NSLocalizedString(@"You don't have friends "
                                                                     @"yet. Add one with a code.",
                                                                     @"Online panel: no friends")
                                      center:YES]];
  }
}

- (void)addFriendFromField {
  NSString* code = xe_ios_normalize_friend_code(_codeField.text ?: @"");
  if (!code) {
    _codeField.text = @"";
    _codeField.placeholder =
        NSLocalizedString(@"That code isn't right", @"Online panel: add-friend field, bad code");
    return;
  }
  [[XeniaLiveSocial shared] addFriendWithCode:code];
  _codeField.text = @"";
  _codeField.placeholder =
      NSLocalizedString(@"Request sent", @"Online panel: add-friend field after sending");
  [_codeField resignFirstResponder];
}

- (uint32_t)runningTitle {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  return social.runningTitleProvider ? social.runningTitleProvider() : 0;
}

@end

#pragma mark - Gamer card over the game

@interface XeniaIOSGamerCardOverlay : UIView
@property(nonatomic, copy) void (^done)(void);
@end

@implementation XeniaIOSGamerCardOverlay

- (void)dealloc {
  [_done release];
  [super dealloc];
}

- (void)close {
  void (^done)(void) = [[_done retain] autorelease];
  self.done = nil;
  [UIView animateWithDuration:0.2
      animations:^{
        self.alpha = 0.0;
      }
      completion:^(BOOL __unused finished) {
        [self removeFromSuperview];
        if (done) {
          done();
        }
      }];
}

@end

#pragma mark - Sheet

@implementation XeniaIOSOnlineSheetViewController {
  XeniaIOSOnlinePanel* _panel;
  void (^_acceptInviteHandler)(XeniaLiveInvite*);
  void (^_joinFriendHandler)(XeniaLiveFriend*);
  void (^_gamesHandler)(void);
}

- (void)dealloc {
  [_panel release];
  [_acceptInviteHandler release];
  [_joinFriendHandler release];
  [_gamesHandler release];
  [super dealloc];
}

- (void (^)(XeniaLiveInvite*))acceptInviteHandler {
  return _acceptInviteHandler;
}
- (void)setAcceptInviteHandler:(void (^)(XeniaLiveInvite*))handler {
  if (handler != _acceptInviteHandler) {
    [_acceptInviteHandler release];
    _acceptInviteHandler = [handler copy];
  }
}
- (void (^)(XeniaLiveFriend*))joinFriendHandler {
  return _joinFriendHandler;
}
- (void)setJoinFriendHandler:(void (^)(XeniaLiveFriend*))handler {
  if (handler != _joinFriendHandler) {
    [_joinFriendHandler release];
    _joinFriendHandler = [handler copy];
  }
}
- (void (^)(void))gamesHandler {
  return _gamesHandler;
}
- (void)setGamesHandler:(void (^)(void))handler {
  if (handler != _gamesHandler) {
    [_gamesHandler release];
    _gamesHandler = [handler copy];
  }
}

- (void)viewDidLoad {
  [super viewDidLoad];
  // Dark glass whatever the system setting, like the panel over a game.
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  self.view.backgroundColor = [UIColor clearColor];
  UIVisualEffectView* backdrop = [[[UIVisualEffectView alloc]
      initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThickMaterialDark]]
      autorelease];
  backdrop.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:backdrop];

  __unsafe_unretained XeniaIOSOnlineSheetViewController* unsafe_self = self;
  _panel = [[XeniaIOSOnlinePanel alloc] initWithFrame:CGRectZero];
  _panel.closeHandler = ^{
    [unsafe_self dismissViewControllerAnimated:YES completion:nil];
  };
  _panel.gamesHandler = ^{
    if (unsafe_self->_gamesHandler) {
      unsafe_self->_gamesHandler();
    }
  };
  _panel.acceptInviteHandler = ^(XeniaLiveInvite* invite) {
    void (^handler)(XeniaLiveInvite*) = [[unsafe_self->_acceptInviteHandler copy] autorelease];
    XeniaLiveInvite* held = [[invite retain] autorelease];
    [unsafe_self dismissViewControllerAnimated:YES
                                    completion:^{
                                      if (handler) handler(held);
                                    }];
  };
  _panel.joinFriendHandler = ^(XeniaLiveFriend* peer) {
    void (^handler)(XeniaLiveFriend*) = [[unsafe_self->_joinFriendHandler copy] autorelease];
    XeniaLiveFriend* held = [[peer retain] autorelease];
    [unsafe_self dismissViewControllerAnimated:YES
                                    completion:^{
                                      if (handler) handler(held);
                                    }];
  };
  _panel.embedded = YES;  // after the handlers, so the first page has its close button
  [self.view addSubview:_panel];
  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [backdrop.topAnchor constraintEqualToAnchor:self.view.topAnchor],
    [backdrop.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [backdrop.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [backdrop.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [_panel.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
    [_panel.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [_panel.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:8],
    [_panel.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-8],
  ]];
  [[XeniaLiveSocial shared] refresh];
}

@end

void xe_ios_present_gamer_card(UIView* host, uint64_t xuid, void (^join_friend)(XeniaLiveFriend*),
                               void (^done)(void)) {
  if (!host) {
    if (done) {
      done();
    }
    return;
  }
  XeniaIOSGamerCardOverlay* overlay =
      [[[XeniaIOSGamerCardOverlay alloc] initWithFrame:host.bounds] autorelease];
  overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  overlay.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.35];
  // Over the game, like the menu: dark, whatever the device's look.
  overlay.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  overlay.done = done;
  overlay.accessibilityViewIsModal = YES;

  UIView* card = [[[UIView alloc] init] autorelease];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  card.backgroundColor = [UIColor clearColor];
  xe_apply_floating_window_chrome(card);
  card.layer.cornerRadius = kPanelRadius;
  card.layer.cornerCurve = kCACornerCurveContinuous;
  for (UIView* view in card.subviews) {
    if ([view isKindOfClass:[UIVisualEffectView class]]) {
      view.layer.cornerRadius = kPanelRadius;
      view.layer.cornerCurve = kCACornerCurveContinuous;
    }
  }
  [overlay addSubview:card];

  __unsafe_unretained XeniaIOSGamerCardOverlay* unsafe_overlay = overlay;
  XeniaIOSGamerCardView* content = [[[XeniaIOSGamerCardView alloc] initWithXuid:xuid
                                                                       gamertag:nil] autorelease];
  if (join_friend) {
    void (^join)(XeniaLiveFriend*) = [[join_friend copy] autorelease];
    content.joinFriendHandler = ^(XeniaLiveFriend* peer) {
      [unsafe_overlay close];
      join(peer);
    };
  }
  UIButton* close = Capsule(NSLocalizedString(@"Close", @"Gamer card button"), nil, NO, YES,
                            ^(UIButton* __unused b) {
                              [unsafe_overlay close];
                            });
  UIStackView* stack =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ content, close ]] autorelease];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 8.0;
  [card addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [card.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
    [card.centerYAnchor constraintEqualToAnchor:overlay.safeAreaLayoutGuide.centerYAnchor],
    [card.widthAnchor constraintEqualToConstant:300.0],
    [card.topAnchor constraintGreaterThanOrEqualToAnchor:overlay.safeAreaLayoutGuide.topAnchor
                                                constant:12.0],
    [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:22.0],
    [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-18.0],
    [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:18.0],
    [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-18.0],
  ]];
  // A tap beside the card closes it, like B on a controller would.
  UIButton* outside = [UIButton buttonWithType:UIButtonTypeCustom];
  outside.frame = overlay.bounds;
  outside.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  outside.isAccessibilityElement = NO;
  [outside addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
             [unsafe_overlay close];
           }]
    forControlEvents:UIControlEventTouchUpInside];
  [overlay insertSubview:outside atIndex:0];

  overlay.alpha = 0.0;
  [host addSubview:overlay];
  [UIView animateWithDuration:0.2
                   animations:^{
                     overlay.alpha = 1.0;
                   }];
  UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, content);
}


@implementation XeniaIOSPublicGamesViewController {
  NSDictionary<NSNumber*, NSString*>* _library;
  void (^_openGame)(uint32_t);
  UIStackView* _list;
  UILabel* _subtitle;
  UIView* _dot;
  NSMutableSet<NSNumber*>* _coverFetches;
}

- (instancetype)initWithLibraryNames:(NSDictionary<NSNumber*, NSString*>*)names
                            openGame:(void (^)(uint32_t))open_game {
  if ((self = [super initWithNibName:nil bundle:nil])) {
    _library = [names copy];
    _openGame = [open_game copy];
    _coverFetches = [[NSMutableSet alloc] init];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_library release];
  [_openGame release];
  [_list release];
  [_subtitle release];
  [_dot release];
  [_coverFetches release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  // Dark glass like the Friends sheet: the sheet's own material shows through. The title and the
  // ✕ are the navigation bar's, as on Profiles.
  self.view.backgroundColor = [UIColor clearColor];
  self.title = NSLocalizedString(@"Public Games", @"Public Games: page title");
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeAlways;
  self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                           target:self
                           action:@selector(closeTapped)] autorelease];

  _dot = [[UIView alloc] init];
  _dot.translatesAutoresizingMaskIntoConstraints = NO;
  _dot.backgroundColor = HexColor(0x30D158);
  _dot.layer.cornerRadius = 4.5;
  [NSLayoutConstraint activateConstraints:@[
    [_dot.widthAnchor constraintEqualToConstant:9],
    [_dot.heightAnchor constraintEqualToConstant:9],
  ]];
  _subtitle = [NewLabel(@"", UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular,
                        [UIColor colorWithWhite:1.0 alpha:0.62]) retain];
  UIStackView* subtitle_row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ _dot, _subtitle ]] autorelease];
  subtitle_row.spacing = 7;
  subtitle_row.alignment = UIStackViewAlignmentCenter;

  _list = [[UIStackView alloc] init];
  _list.translatesAutoresizingMaskIntoConstraints = NO;
  _list.axis = UILayoutConstraintAxisVertical;
  _list.spacing = 8;

  UIStackView* header = [[[UIStackView alloc] initWithArrangedSubviews:@[ subtitle_row ]]
      autorelease];
  header.translatesAutoresizingMaskIntoConstraints = NO;
  header.axis = UILayoutConstraintAxisVertical;
  header.alignment = UIStackViewAlignmentLeading;
  header.spacing = 4;

  UIScrollView* scroll = [[[UIScrollView alloc] init] autorelease];
  scroll.translatesAutoresizingMaskIntoConstraints = NO;
  scroll.alwaysBounceVertical = YES;
  scroll.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  UIStackView* content = [[[UIStackView alloc] initWithArrangedSubviews:@[ header, _list ]]
      autorelease];
  content.translatesAutoresizingMaskIntoConstraints = NO;
  content.axis = UILayoutConstraintAxisVertical;
  content.spacing = 18;
  [scroll addSubview:content];
  [self.view addSubview:scroll];

  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  UILayoutGuide* column = [[[UILayoutGuide alloc] init] autorelease];
  [self.view addLayoutGuide:column];
  NSLayoutConstraint* fill = [column.widthAnchor constraintEqualToAnchor:safe.widthAnchor
                                                                constant:-32];
  fill.priority = UILayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    [column.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
    [column.widthAnchor constraintLessThanOrEqualToConstant:720],
    fill,
    [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor constant:4],
    [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [content.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
    [content.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor
                                         constant:-24],
    [content.leadingAnchor constraintEqualToAnchor:column.leadingAnchor],
    [content.trailingAnchor constraintEqualToAnchor:column.trailingAnchor],
    [scroll.contentLayoutGuide.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
  ]];

  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(reload)
                                               name:XeniaIOSSystemLinkOnlineDidChangeNotification
                                             object:nil];
  [self reload];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  self.navigationController.navigationBar.prefersLargeTitles = YES;
  self.navigationController.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  xe_ios_system_link_keep_online(YES);
  xe_ios_system_link_refresh_online();
}

- (void)closeTapped {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  xe_ios_system_link_keep_online(NO);
}

- (UIView*)coverForTitle:(uint32_t)title_id {
  UIImage* cover = xe_cached_game_art_thumbnail(title_id) ?: xe_cached_game_art(title_id);
  if (!cover && ![_coverFetches containsObject:@(title_id)]) {
    [_coverFetches addObject:@(title_id)];
    __unsafe_unretained XeniaIOSPublicGamesViewController* unsafe_self = self;
    xe_fetch_game_art(title_id, ^(UIImage* image) {
      if (image && unsafe_self.isViewLoaded) {
        [unsafe_self reload];
      }
    });
  }
  UIImageView* view = [[[UIImageView alloc] initWithImage:cover] autorelease];
  view.translatesAutoresizingMaskIntoConstraints = NO;
  view.contentMode = UIViewContentModeScaleAspectFill;
  view.clipsToBounds = YES;
  view.layer.cornerRadius = 6;
  view.layer.cornerCurve = kCACornerCurveContinuous;
  view.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.1];
  if (!cover) {
    view.image = [UIImage systemImageNamed:@"gamecontroller.fill"];
    view.contentMode = UIViewContentModeCenter;
    view.tintColor = [UIColor colorWithWhite:1.0 alpha:0.4];
  }
  [NSLayoutConstraint activateConstraints:@[
    [view.widthAnchor constraintEqualToConstant:48],
    [view.heightAnchor constraintEqualToConstant:64],
  ]];
  return view;
}

- (void)reload {
  for (UIView* row in [[_list.arrangedSubviews copy] autorelease]) {
    [_list removeArrangedSubview:row];
    [row removeFromSuperview];
  }
  const BOOL connected = xe_ios_system_link_online_connected();
  const NSInteger others = xe_ios_system_link_players_elsewhere();
  _dot.hidden = others <= 0;
  _subtitle.text =
      !connected ? NSLocalizedString(@"Not connected", @"Public Games: subtitle when the lobby "
                                                       @"can't be reached")
      : others <= 0 ? NSLocalizedString(@"Nobody else is online", @"Public Games: subtitle")
      : others == 1 ? NSLocalizedString(@"1 player online", @"Public Games: subtitle")
                    : [NSString localizedStringWithFormat:NSLocalizedString(
                                                              @"%ld players online",
                                                              @"Public Games: subtitle; %ld is "
                                                              @"how many other players are on "
                                                              @"XeniOS"),
                                                          long(others)];

  // Games with someone in them, the library's first, then by how many are playing.
  NSDictionary<NSNumber*, NSNumber*>* counts = xe_ios_system_link_online_playing();
  if (!counts.count) {
    counts = xe_ios_system_link_online_titles();
  }
  NSMutableArray<NSNumber*>* games = [NSMutableArray array];
  for (NSNumber* title_id in counts) {
    if (counts[title_id].integerValue > 0) {
      [games addObject:title_id];
    }
  }
  [games sortUsingComparator:^NSComparisonResult(NSNumber* a, NSNumber* b) {
    const BOOL a_mine = _library[a] != nil, b_mine = _library[b] != nil;
    if (a_mine != b_mine) {
      return a_mine ? NSOrderedAscending : NSOrderedDescending;
    }
    return [counts[b] compare:counts[a]];
  }];

  if (!games.count) {
    UILabel* empty = NewLabel(connected ? NSLocalizedString(@"No public games right now",
                                                            @"Public Games: empty state")
                                        : NSLocalizedString(@"Can't reach XeniOS online",
                                                            @"Public Games: empty state"),
                              UIFontTextStyleHeadline, 17.0, UIFontWeightSemibold,
                              [UIColor colorWithWhite:1.0 alpha:0.7]);
    empty.textAlignment = NSTextAlignmentCenter;
    UILabel* hint = NewLabel(connected ? NSLocalizedString(@"Start a game from your library and "
                                                           @"it shows up here.",
                                                           @"Public Games: empty state hint")
                                       : NSLocalizedString(@"Check your connection and try again.",
                                                           @"Public Games: empty state hint"),
                             UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular,
                             [UIColor colorWithWhite:1.0 alpha:0.5]);
    hint.textAlignment = NSTextAlignmentCenter;
    hint.numberOfLines = 0;
    UIStackView* box = [[[UIStackView alloc] initWithArrangedSubviews:@[ empty, hint ]] autorelease];
    box.axis = UILayoutConstraintAxisVertical;
    box.spacing = 6;
    box.layoutMargins = UIEdgeInsetsMake(80, 0, 0, 0);
    box.layoutMarginsRelativeArrangement = YES;
    [_list addArrangedSubview:box];
    return;
  }

  __unsafe_unretained XeniaIOSPublicGamesViewController* unsafe_self = self;
  NSMutableArray<UIView*>* rows = [NSMutableArray array];
  for (NSNumber* number in games) {
    const uint32_t title_id = number.unsignedIntValue;
    const NSInteger playing = counts[number].integerValue;
    NSString* library_name = _library[number];
    NSString* name = library_name ?: xe_ios_game_name_for_title(title_id);
    NSString* detail = [NSString localizedStringWithFormat:NSLocalizedString(
                                                               @"%ld playing",
                                                               @"Public Games: row detail; %ld "
                                                               @"is how many are in the game"),
                                                           long(playing)];
    void (^tapped)(void) = nil;
    if (library_name) {
      tapped = ^{
        void (^open)(uint32_t) = [[unsafe_self->_openGame copy] autorelease];
        [unsafe_self dismissViewControllerAnimated:YES
                                        completion:^{
                                          if (open) open(title_id);
                                        }];
      };
    }
    UIView* row = Row([self coverForTitle:title_id], name, detail, NO,
                      library_name ? @[ Chevron() ] : @[], tapped);
    [rows addObject:row];
  }
  [_list addArrangedSubview:Group(rows, nil)];
}

@end
