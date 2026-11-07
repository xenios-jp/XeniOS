/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_online_panel.h"

#import <LinkPresentation/LinkPresentation.h>

#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/launcher/ios_system_link_view_controller.h"
#import "xenia/ui/ios/settings/ios_live_account_view_controllers.h"
#import "xenia/ui/ios/shared/ios_art_wash.h"
#import "xenia/ui/ios/shared/ios_live_social_bridge.h"
#import "xenia/ui/ios/shared/ios_glass_card.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

constexpr CGFloat kPanelRadius = 28.0;

// The panel's sizes are drawn for the small floating card over a game. On the home screen it is a
// whole sheet, so everything grows by this factor to read like the rest of the app's pages.
CGFloat gPanelScale = 1.0;
inline CGFloat S(CGFloat value) { return value * gPanelScale; }

UILabel* NewLabel(NSString* text, UIFontTextStyle style, CGFloat size, UIFontWeight weight,
                  UIColor* color) {
  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.text = text;
  label.textColor = color;
  xe_apply_label_font(label, style, S(size), weight);
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
  box.layoutMargins = UIEdgeInsetsMake(8, 16, 0, 16);
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

// A code letter by letter, so VoiceOver doesn't read it as a word.
NSString* SpokenCode(NSString* code) {
  NSMutableArray<NSString*>* letters = [NSMutableArray array];
  for (NSUInteger i = 0; i < code.length; ++i) {
    [letters addObject:[code substringWithRange:NSMakeRange(i, 1)]];
  }
  return [letters componentsJoinedByString:@" "];
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
  size = S(size);
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
    dot.backgroundColor = [XeniaTheme accent];
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
  [row.heightAnchor constraintGreaterThanOrEqualToConstant:S([XeniaTheme glassRowMinHeight])].active = YES;
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
  line.backgroundColor = [XeniaTheme glassHairline];
  [line.heightAnchor constraintEqualToConstant:1.0 / UIScreen.mainScreen.scale].active = YES;
  return line;
}

// Rows in one rounded group, a hairline between them. |tint| replaces the quiet glass.
UIView* Group(NSArray<UIView*>* rows, UIColor* tint) {
  UIView* box = [[[UIView alloc] init] autorelease];
  box.translatesAutoresizingMaskIntoConstraints = NO;
  box.backgroundColor = tint ?: [XeniaTheme glassGroupFill];
  box.layer.cornerRadius = [XeniaTheme glassGroupRadius];
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
    [column.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:16],
    [column.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-16],
  ]];
  return box;
}

// A plain row symbol, no tile: white on the glass, in a fixed box so row titles line up.
UIView* RowSymbol(NSString* name) {
  UIImageView* symbol = [[[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:name
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithTextStyle:UIFontTextStyleBody]]]
      autorelease];
  symbol.translatesAutoresizingMaskIntoConstraints = NO;
  symbol.tintColor = [XeniaTheme textPrimary];
  symbol.contentMode = UIViewContentModeCenter;
  symbol.isAccessibilityElement = NO;
  [symbol.widthAnchor constraintEqualToConstant:S(26.0)].active = YES;
  return symbol;
}

// The one empty-state shape: symbol, one bold line, one plain line, in a glass group.
UIView* EmptyStateWithButton(NSString* symbol_name, NSString* title, NSString* detail,
                             NSString* button_title, void (^action)(void)) {
  NSMutableArray<UIView*>* items = [NSMutableArray array];
  if (symbol_name) {
    UIImageView* symbol = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:symbol_name
                              withConfiguration:[UIImageSymbolConfiguration
                                                    configurationWithTextStyle:UIFontTextStyleTitle2]]]
        autorelease];
    symbol.tintColor = [XeniaTheme textSecondary];
    symbol.contentMode = UIViewContentModeCenter;
    symbol.isAccessibilityElement = NO;
    [items addObject:symbol];
  }
  UILabel* title_label =
      NewLabel(title, UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold,
               [XeniaTheme textPrimary]);
  title_label.numberOfLines = 0;
  title_label.textAlignment = NSTextAlignmentCenter;
  [items addObject:title_label];
  if (detail.length) {
    UILabel* detail_label = NewLabel(detail, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular,
                                     [XeniaTheme textSecondary]);
    detail_label.numberOfLines = 0;
    detail_label.textAlignment = NSTextAlignmentCenter;
    [items addObject:detail_label];
  }
  UIButton* button = nil;
  if (button_title.length && action) {
    void (^handler)(void) = [[action copy] autorelease];
    button = Capsule(button_title, nil, YES, NO, ^(UIButton* __unused sender) {
      handler();
    });
    [items addObject:button];
  }
  UIStackView* column = [[[UIStackView alloc] initWithArrangedSubviews:items] autorelease];
  column.axis = UILayoutConstraintAxisVertical;
  column.alignment = UIStackViewAlignmentCenter;
  column.spacing = 4.0;
  if (symbol_name) {
    [column setCustomSpacing:10.0 afterView:items[0]];
  }
  column.layoutMargins = UIEdgeInsetsMake(20, 4, 20, 4);
  column.layoutMarginsRelativeArrangement = YES;
  if (button) {
    [column setCustomSpacing:14.0 afterView:items[items.count - 2]];
  }
  return Group(@[ column ], nil);
}

UIView* EmptyState(NSString* symbol_name, NSString* title, NSString* detail) {
  return EmptyStateWithButton(symbol_name, title, detail, nil, nil);
}

// A round 32 pt icon button for the header, with an optional blue count on its corner.
UIButton* IconButton(NSString* symbol, NSString* label, NSInteger badge, void (^action)(void)) {
  UIButtonConfiguration* config = [UIButtonConfiguration plainButtonConfiguration];
  config.image = [UIImage systemImageNamed:symbol];
  config.baseForegroundColor = [XeniaTheme textPrimary];
  config.contentInsets = NSDirectionalEdgeInsetsZero;
  config.preferredSymbolConfigurationForImage =
      [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleSubheadline];
  // The configuration draws the fill: a layer corner radius set on the button doesn't survive
  // its background view, which left these as rounded squares.
  config.background.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.1];
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.accessibilityLabel = label;
  void (^handler)(void) = [[action copy] autorelease];
  [button addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
            handler();
          }]
   forControlEvents:UIControlEventTouchUpInside];
  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:S([XeniaTheme glassControlSize])],
    [button.heightAnchor constraintEqualToConstant:S([XeniaTheme glassControlSize])],
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

#pragma mark - Friend link, shared

// Messages, Mail, AirDrop and Copy take the text and the link apart, and show the link's preview.
// Other apps' share extensions (Discord, say) often keep only one of two items, so they get
// both in one text.
static BOOL XeniaShareKeepsItemsApart(UIActivityType type) {
  return [type isEqualToString:UIActivityTypeMessage] || [type isEqualToString:UIActivityTypeMail] ||
         [type isEqualToString:UIActivityTypeAirDrop] ||
         [type isEqualToString:UIActivityTypeCopyToPasteboard];
}

// The text in the share sheet; with the link in it for apps that keep one item.
@interface XeniaShareTextItem : NSObject <UIActivityItemSource>
- (instancetype)initWithText:(NSString*)text url:(NSURL*)url;
@end

@implementation XeniaShareTextItem {
  NSString* _text;
  NSURL* _url;
}

- (instancetype)initWithText:(NSString*)text url:(NSURL*)url {
  if ((self = [super init])) {
    _text = [text copy];
    _url = [url retain];
  }
  return self;
}

- (void)dealloc {
  [_text release];
  [_url release];
  [super dealloc];
}

- (id)activityViewControllerPlaceholderItem:(UIActivityViewController* __unused)controller {
  return _text;
}

- (id)activityViewController:(UIActivityViewController* __unused)controller
         itemForActivityType:(UIActivityType)type {
  if (!_url || XeniaShareKeepsItemsApart(type)) {
    return _text;
  }
  return [NSString stringWithFormat:@"%@\n%@", _text, _url.absoluteString];
}

@end

// A link, for the apps that keep it apart from the text (in the text for the others).
@interface XeniaShareURLItem : NSObject <UIActivityItemSource>
- (instancetype)initWithURL:(NSURL*)url;
@end

@implementation XeniaShareURLItem {
  NSURL* _url;
}

- (instancetype)initWithURL:(NSURL*)url {
  if ((self = [super init])) {
    _url = [url retain];
  }
  return self;
}

- (void)dealloc {
  [_url release];
  [super dealloc];
}

- (id)activityViewControllerPlaceholderItem:(UIActivityViewController* __unused)controller {
  return _url;
}

- (id)activityViewController:(UIActivityViewController* __unused)controller
         itemForActivityType:(UIActivityType)type {
  return XeniaShareKeepsItemsApart(type) ? _url : nil;
}

@end

// The link in the share sheet, with a title and the QR code as its preview (not a bare URL).
@interface XeniaFriendLinkItem : NSObject <UIActivityItemSource>
@property(nonatomic, readonly) NSURL* url;
- (instancetype)initWithCode:(NSString*)code;
@end

@implementation XeniaFriendLinkItem {
  NSString* _code;
  NSURL* _url;
}

- (instancetype)initWithCode:(NSString*)code {
  if ((self = [super init])) {
    _code = [code copy];
    _url = [[NSURL URLWithString:xe_ios_friend_link(code)] retain];
  }
  return self;
}

- (void)dealloc {
  [_code release];
  [_url release];
  [super dealloc];
}

- (id)activityViewControllerPlaceholderItem:(UIActivityViewController* __unused)controller {
  return _url;
}

- (id)activityViewController:(UIActivityViewController* __unused)controller
         itemForActivityType:(UIActivityType)type {
  // In the text for the others.
  return XeniaShareKeepsItemsApart(type) ? _url : nil;
}

- (NSURL*)url {
  return _url;
}

- (LPLinkMetadata*)activityViewControllerLinkMetadata:
    (UIActivityViewController* __unused)controller {
  LPLinkMetadata* metadata = [[[LPLinkMetadata alloc] init] autorelease];
  metadata.originalURL = _url;
  metadata.URL = _url;
  metadata.title = [NSString
      stringWithFormat:NSLocalizedString(@"Add me as a friend · %@",
                                         @"Share sheet title; %@ is the user's friend code"),
                       xe_ios_format_friend_code(_code)];
  // The QR code on white, so the preview is something to scan as well as a link.
  UIImage* qr = xe_ios_qr_image(_url.absoluteString, 120);
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(144, 144)] autorelease];
  UIImage* tile = [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused c) {
    [[UIColor whiteColor] setFill];
    UIRectFill(CGRectMake(0, 0, 144, 144));
    [qr drawInRect:CGRectMake(12, 12, 120, 120)];
  }];
  metadata.iconProvider = [[[NSItemProvider alloc] initWithObject:tile] autorelease];
  return metadata;
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
  NSLayoutConstraint* _headerTop;
  NSLayoutConstraint* _headerLeading;
  NSLayoutConstraint* _headerTrailing;
  NSLayoutConstraint* _stackBottom;
  UIScrollView* _scroll;
  UIStackView* _stack;
  NSLayoutConstraint* _fitHeight;
  XeniaIOSGamerCardView* _card;
  UITextField* _codeField;
  UITextField* _searchField;
  NSString* _query;
  // Under Add Friend's field: what happened to the last request (sent, or why the lobby said no).
  NSString* _addStatus;
  BOOL _addStatusFailed;
  XeniaOnlinePage _page;
  XeniaOnlinePage _cardReturn;
  BOOL _leaveArmed;
  void (^_acceptInviteHandler)(XeniaLiveInvite*);
  void (^_joinFriendHandler)(XeniaLiveFriend*);
  void (^_allFriendsHandler)(void);
  void (^_closeHandler)(void);
  BOOL _embedded;
  BOOL _fitsContent;
  // Start a Party was tapped: the party arrives from the server a moment later, and its page
  // opens then.
  BOOL _openPartyWhenReady;
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
    _headerTop = [[_header.topAnchor constraintEqualToAnchor:self.topAnchor] retain];
    _headerLeading = [[_header.leadingAnchor constraintEqualToAnchor:self.leadingAnchor] retain];
    _headerTrailing = [[_header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor] retain];
    _stackBottom =
        [[_stack.bottomAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.bottomAnchor]
            retain];
    [self updateCardInsets];
    [NSLayoutConstraint activateConstraints:@[
      _headerTop,
      _headerLeading,
      _headerTrailing,
      [_scroll.topAnchor constraintEqualToAnchor:_header.bottomAnchor constant:6],
      [_scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
      [_scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
      [_scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
      [_stack.topAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.topAnchor],
      _stackBottom,
      [_stack.leadingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.leadingAnchor
                                           constant:16],
      [_stack.trailingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.trailingAnchor
                                            constant:-16],
      _fitHeight,
    ]];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(socialChanged:)
                                                 name:XeniaLiveSocialDidChangeNotification
                                               object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(requestFailed:)
                                                 name:XeniaLiveSocialRequestFailedNotification
                                               object:nil];
    [self reload];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_header release];
  [_headerTop release];
  [_headerLeading release];
  [_headerTrailing release];
  [_stackBottom release];
  [_scroll release];
  [_stack release];
  [_fitHeight release];
  [_card release];
  [_codeField release];
  [_searchField release];
  [_query release];
  [_addStatus release];
  [_acceptInviteHandler release];
  [_joinFriendHandler release];
  [_allFriendsHandler release];
  [_closeHandler release];
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

#undef XE_COPY_BLOCK_PROPERTY

- (BOOL)embedded {
  return _embedded;
}

// Embedded, the panel is a glass card over the library: its title and buttons sit where a glass
// card's bar puts them (Profiles, Public Games), and its last row 16 above the card's edge.
// Floating beside the game menu it keeps its own.
- (void)updateCardInsets {
  _headerTop.constant = _embedded ? 10.0 : 14.0;
  _headerLeading.constant = _embedded ? 16.0 : 20.0;
  _headerTrailing.constant = _embedded ? -16.0 : -14.0;
  const CGFloat bottom = _embedded ? 16.0 : 12.0;
  _stackBottom.constant = -bottom;
  _fitHeight.constant = bottom;
}

// In a sheet the sheet is the window: drop the floating chrome and let the page fill it.
- (void)setEmbedded:(BOOL)embedded {
  _embedded = embedded;
  [self updateCardInsets];
  self.layer.cornerRadius = embedded ? 0.0 : kPanelRadius;
  self.layer.borderWidth = embedded ? 0.0 : 1.0;
  self.layer.shadowOpacity = embedded ? 0.0f : 0.14f;
  for (UIView* view in self.subviews) {
    if ([view isKindOfClass:[UIVisualEffectView class]]) {
      view.hidden = embedded;
    }
  }
  _fitHeight.active = !embedded || _fitsContent;
  [self reload];
}

- (BOOL)fitsContent {
  return _fitsContent;
}

- (void)setFitsContent:(BOOL)fits {
  _fitsContent = fits;
  _fitHeight.active = fits || !_embedded;
}

- (void)socialChanged:(NSNotification* __unused)note {
  // A gamer card refreshes itself; a field being typed in keeps its text.
  if (_page == XeniaOnlinePageCard || _codeField.isFirstResponder ||
      _searchField.isFirstResponder) {
    return;
  }
  if (_openPartyWhenReady && [XeniaLiveSocial shared].partyMembers.count) {
    _openPartyWhenReady = NO;
    [self showParty];
    return;
  }
  [self reload];
}

- (void)requestFailed:(NSNotification*)note {
  NSString* message = note.userInfo[XeniaLiveSocialMessageKey];
  if (_page != XeniaOnlinePageAdd || !_addStatus || !message.length) {
    return;
  }
  [self setAddStatus:message failed:YES];
}

- (void)setAddStatus:(NSString*)status failed:(BOOL)failed {
  [_addStatus autorelease];
  _addStatus = [status copy];
  _addStatusFailed = failed;
  if (_page == XeniaOnlinePageAdd) {
    [self reload];
  }
  if (status.length) {
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, status);
  }
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
  if (page != XeniaOnlinePageParty) {
    _openPartyWhenReady = NO;
  }
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
  [_addStatus release];
  _addStatus = nil;
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
  __unsafe_unretained XeniaIOSOnlinePanel* unsafe_self = self;
  UIButton* back = IconButton(@"chevron.left",
                              [NSString stringWithFormat:NSLocalizedString(@"Back to %@",
                                                                           @"VoiceOver; %@ is a page"),
                                                         back_title],
                              0, ^{
                                [unsafe_self performSelector:action];
                              });
  UILabel* label =
      NewLabel(title, UIFontTextStyleHeadline, 17.0, UIFontWeightSemibold, [XeniaTheme textPrimary]);
  label.textAlignment = NSTextAlignmentCenter;
  label.accessibilityTraits = UIAccessibilityTraitHeader;
  UIView* balance = [[[UIView alloc] init] autorelease];
  balance.translatesAutoresizingMaskIntoConstraints = NO;
  [balance.widthAnchor constraintEqualToConstant:S([XeniaTheme glassControlSize])].active = YES;
  UIStackView* row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ back, label, balance ]] autorelease];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  [row.heightAnchor constraintGreaterThanOrEqualToConstant:S(36.0)].active = YES;
  return row;
}

- (UIView*)titleHeader:(NSString*)title icons:(NSArray<UIView*>*)icons {
  // The card's large leading title; the smaller card beside the in-game menu keeps a title-2.
  UILabel* label = _embedded ? NewLabel(title, UIFontTextStyleTitle1, 28.0, UIFontWeightBold,
                                        [XeniaTheme textPrimary])
                             : NewLabel(title, UIFontTextStyleTitle2, 22.0, UIFontWeightBold,
                                        [XeniaTheme textPrimary]);
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
  // A glass card's bar spaces its buttons 12 apart.
  row.spacing = _embedded ? 12.0 : 8.0;
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
    [_stack addArrangedSubview:EmptyState(nil, social.statusText, nil)];
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
      [avatar.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:S(i * step)],
      [avatar.centerYAnchor constraintEqualToAnchor:box.centerYAnchor],
    ]];
  }
  [NSLayoutConstraint activateConstraints:@[
    [box.widthAnchor constraintEqualToConstant:S(size + (MAX(count, (NSUInteger)1) - 1) * step)],
    [box.heightAnchor constraintEqualToConstant:S(36.0)],
  ]];
  box.isAccessibilityElement = NO;
  return box;
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
    [_stack addArrangedSubview:SectionLabel(NSLocalizedString(@"Invites",
                                                              @"Online panel section"))];
    [_stack addArrangedSubview:Group(rows, [InviteBlue() colorWithAlphaComponent:0.24])];
  }

  // Online now, in one group: those in your game first, then who's playing, then the rest.
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
  NSMutableArray<XeniaLiveFriend*>* online = [NSMutableArray arrayWithArray:in_game];
  [online addObjectsFromArray:playing];
  [online addObjectsFromArray:idle];
  [_stack addArrangedSubview:SectionLabel(NSLocalizedString(@"Online now",
                                                            @"Online panel section"))];
  if (online.count) {
    [_stack addArrangedSubview:Group([self rowsForFriends:online], nil)];
  } else {
    [_stack addArrangedSubview:EmptyState(@"person.2",
                                          NSLocalizedString(@"No friends online",
                                                            @"Online panel: empty state"),
                                          NSLocalizedString(@"Search to see all your friends, or "
                                                            @"add one with a code.",
                                                            @"Online panel: empty state hint"))];
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
      row = Row(RowSymbol(@"person.2"), NSLocalizedString(@"Start a Party", @"Online panel row"),
                nil, NO, @[ Chevron() ], ^{
                  unsafe_self->_openPartyWhenReady = [[XeniaLiveSocial shared] createParty];
                  [unsafe_self showParty];
                });
    }
    [_stack addArrangedSubview:SectionLabel(NSLocalizedString(@"Play together",
                                                              @"Online panel section"))];
    [_stack addArrangedSubview:Group(@[ row ], nil)];
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
    // The same empty state as the friends list, not a bare line under the title.
    [_stack addArrangedSubview:EmptyState(@"person.2",
                                          NSLocalizedString(@"No friends online",
                                                            @"Online panel: empty state"),
                                          NSLocalizedString(@"Friends show up here to invite "
                                                            @"when they come online.",
                                                            @"Online panel: nobody to invite"))];
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

  // Your own code: a QR code to scan across the room, the code to read out, and a link to send.
  if (social.friendCode.length) {
    NSString* code = social.friendCode;
    NSString* formatted = xe_ios_format_friend_code(code);
    UIImageView* qr =
        [[[UIImageView alloc] initWithImage:xe_ios_qr_image(xe_ios_friend_link(code), 132)]
            autorelease];
    qr.contentMode = UIViewContentModeCenter;
    qr.backgroundColor = [UIColor whiteColor];
    qr.layer.cornerRadius = 18.0;
    qr.layer.cornerCurve = kCACornerCurveContinuous;
    qr.translatesAutoresizingMaskIntoConstraints = NO;
    [qr.widthAnchor constraintEqualToConstant:156.0].active = YES;
    [qr.heightAnchor constraintEqualToConstant:156.0].active = YES;
    qr.isAccessibilityElement = YES;
    qr.accessibilityLabel = NSLocalizedString(@"Your friend QR code", @"Online panel: QR code");
    UILabel* mine = NewLabel(formatted, UIFontTextStyleTitle2, 24.0, UIFontWeightBold,
                             [XeniaTheme textPrimary]);
    mine.font = [UIFont monospacedSystemFontOfSize:S(24.0) weight:UIFontWeightBold];
    mine.textAlignment = NSTextAlignmentCenter;
    mine.accessibilityLabel =
        [NSString stringWithFormat:NSLocalizedString(@"Your friend code, %@",
                                                     @"Online panel: VoiceOver; %@ is the code"),
                                   SpokenCode(code)];
    UIButton* share = Capsule(NSLocalizedString(@"Share…", @"Online panel: share your friend link"),
                              @"square.and.arrow.up", YES, NO, ^(UIButton* sender) {
                                [unsafe_self shareFriendCode:code from:sender];
                              });
    UIButton* copy = Capsule(NSLocalizedString(@"Copy", @"Online panel: copy your friend code"),
                             @"doc.on.doc", NO, NO, ^(UIButton* sender) {
                               [UIPasteboard generalPasteboard].string = formatted;
                               UIButtonConfiguration* config = sender.configuration;
                               config.title = NSLocalizedString(@"Copied", @"Online panel: after "
                                                                           @"copying");
                               config.image = [UIImage systemImageNamed:@"checkmark"];
                               sender.configuration = config;
                             });
    UIStackView* buttons =
        [[[UIStackView alloc] initWithArrangedSubviews:@[ share, copy ]] autorelease];
    buttons.axis = UILayoutConstraintAxisHorizontal;
    buttons.spacing = 8.0;
    UIStackView* card =
        [[[UIStackView alloc] initWithArrangedSubviews:@[ qr, mine, buttons ]] autorelease];
    card.axis = UILayoutConstraintAxisVertical;
    card.alignment = UIStackViewAlignmentCenter;
    card.spacing = 10.0;
    [card setCustomSpacing:14.0 afterView:qr];
    [card setCustomSpacing:12.0 afterView:mine];
    card.layoutMargins = UIEdgeInsetsMake(18, 12, 16, 12);
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
  UIButton* scan = Capsule(nil, @"qrcode.viewfinder", NO, NO,
                           ^(UIButton* __unused b) { [unsafe_self scanFriendCode]; });
  scan.accessibilityLabel =
      NSLocalizedString(@"Scan a Friend’s Code", @"Online panel: camera button");
  [scan.heightAnchor constraintEqualToConstant:44.0].active = YES;
  [scan.widthAnchor constraintEqualToConstant:44.0].active = YES;
  UIStackView* add_row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ _codeField, scan, add ]] autorelease];
  add_row.axis = UILayoutConstraintAxisHorizontal;
  add_row.alignment = UIStackViewAlignmentCenter;
  add_row.spacing = 8.0;
  [_stack addArrangedSubview:add_row];
  if (_addStatus.length) {
    UILabel* status = [self caption:_addStatus center:NO];
    if (_addStatusFailed) {
      status.textColor = [UIColor systemRedColor];
    }
    // In line with the field's text, not its edge.
    UIStackView* inset = [[[UIStackView alloc] initWithArrangedSubviews:@[ status ]] autorelease];
    inset.layoutMargins = UIEdgeInsetsMake(0, 14, 0, 14);
    inset.layoutMarginsRelativeArrangement = YES;
    [_stack addArrangedSubview:inset];
    [_stack setCustomSpacing:6.0 afterView:add_row];
  }

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
    // Blue is for invites; the cursor and Cancel take the accent, like every other field.
    _searchField.tintColor = [XeniaTheme accent];
    _searchField.font = xe_scaled_system_font(UIFontTextStyleBody, 16.0, UIFontWeightRegular);
    _searchField.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.1];
    _searchField.layer.cornerRadius = S(18.0);
    _searchField.layer.cornerCurve = kCACornerCurveContinuous;
    UIImageView* glass = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:@"magnifyingglass"]] autorelease];
    glass.tintColor = [XeniaTheme textSecondary];
    glass.contentMode = UIViewContentModeCenter;
    // In a box, so the field keeps the magnifier clear of its round end and the text where the
    // library's search field has it.
    UIView* glass_box = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 40, 36)] autorelease];
    glass.frame = CGRectMake(10, 0, 24, 36);
    [glass_box addSubview:glass];
    [glass_box.widthAnchor constraintEqualToConstant:40].active = YES;
    _searchField.leftView = glass_box;
    _searchField.leftViewMode = UITextFieldViewModeAlways;
    [_searchField.heightAnchor constraintEqualToConstant:S(36.0)].active = YES;
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
  cancel.tintColor = [XeniaTheme accent];
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
  if (!_codeField.text.length) {
    return;
  }
  NSString* code = xe_ios_normalize_friend_code(_codeField.text);
  if (!code) {
    [self setAddStatus:NSLocalizedString(@"A friend code is 8 letters and numbers.",
                                         @"Online panel: add-friend, bad code")
                failed:YES];
    return;
  }
  if ([code isEqualToString:[XeniaLiveSocial shared].friendCode]) {
    [self setAddStatus:NSLocalizedString(@"That’s your own code.",
                                         @"Online panel: add-friend, own code")
                failed:YES];
    return;
  }
  [[XeniaLiveSocial shared] addFriendWithCode:code];
  _codeField.text = @"";
  [_codeField resignFirstResponder];
  [self setAddStatus:[NSString stringWithFormat:NSLocalizedString(
                                                    @"Request sent to %@.",
                                                    @"Online panel: add-friend; %@ is the code"),
                                                xe_ios_format_friend_code(code)]
              failed:NO];
}

// The controller showing this panel, to present the share sheet and the camera from.
- (UIViewController*)owningController {
  for (UIResponder* r = self.nextResponder; r; r = r.nextResponder) {
    if ([r isKindOfClass:[UIViewController class]]) {
      return (UIViewController*)r;
    }
  }
  return nil;
}

- (void)shareFriendCode:(NSString*)code from:(UIView*)source {
  UIViewController* owner = [self owningController];
  if (!owner) {
    return;
  }
  // The code in words for anyone without XeniOS at hand, and the link that opens Add Friend.
  NSString* text = [NSString
      stringWithFormat:NSLocalizedString(@"Add me as a friend in XeniOS: %@",
                                         @"Shared text; %@ is the user's friend code"),
                       xe_ios_format_friend_code(code)];
  XeniaFriendLinkItem* link = [[[XeniaFriendLinkItem alloc] initWithCode:code] autorelease];
  XeniaShareTextItem* words = [[[XeniaShareTextItem alloc] initWithText:text
                                                                    url:link.url] autorelease];
  UIActivityViewController* share =
      [[[UIActivityViewController alloc] initWithActivityItems:@[ words, link ]
                                         applicationActivities:nil] autorelease];
  share.popoverPresentationController.sourceView = source;
  share.popoverPresentationController.sourceRect = source.bounds;
  // On iPad the owner is the library, already presenting the Friends card: present on top.
  while (owner.presentedViewController && !owner.presentedViewController.isBeingDismissed) {
    owner = owner.presentedViewController;
  }
  [owner presentViewController:share animated:YES completion:nil];
}

- (void)scanFriendCode {
  UIViewController* owner = [self owningController];
  if (!owner) {
    return;
  }
  [_codeField resignFirstResponder];
  __unsafe_unretained XeniaIOSOnlinePanel* unsafe_self = self;
  xe_ios_present_code_scanner(
      owner, NSLocalizedString(@"Scan a Friend’s Code", @"Scan page title"),
      NSLocalizedString(@"Point at the code in their Add Friend", @"Scan page: camera hint"),
      xe_ios_friend_link(@"K7Q2M9XD"), ^BOOL(NSString* text) {
        NSString* scanned = xe_ios_friend_code_from_text(text);
        if (!scanned || ![text.lowercaseString hasPrefix:@"xenios://friend"]) {
          return NO;
        }
        unsafe_self->_codeField.text = scanned;
        [unsafe_self addFriendFromField];
        return YES;
      });
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
  CGRect _sourceRect;
  BOOL _expanded;
  BOOL _closing;
}

- (void)dealloc {
  gPanelScale = 1.0;
  [_panel release];
  [_acceptInviteHandler release];
  [_joinFriendHandler release];
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

- (CGRect)sourceRect {
  return _sourceRect;
}
- (void)setSourceRect:(CGRect)rect {
  _sourceRect = rect;
}

// The card as a transform that lays it exactly over the button it grows from.
- (CGAffineTransform)collapsedTransform {
  const CGRect from = [self.view convertRect:_sourceRect fromView:nil];
  const CGRect to = _panel.frame;
  if (CGRectIsEmpty(from) || CGRectIsEmpty(to)) {
    return CGAffineTransformIdentity;
  }
  return CGAffineTransformMake(from.size.width / to.size.width, 0, 0,
                               from.size.height / to.size.height, CGRectGetMidX(from) - CGRectGetMidX(to),
                               CGRectGetMidY(from) - CGRectGetMidY(to));
}

- (void)setCardContentAlpha:(CGFloat)alpha {
  for (UIView* view in _panel.subviews) {
    if (![view isKindOfClass:[UIVisualEffectView class]]) {
      view.alpha = alpha;
    }
  }
}

- (void)setCornerRadiusAnimated:(CGFloat)radius duration:(NSTimeInterval)duration {
  CABasicAnimation* animation = [CABasicAnimation animationWithKeyPath:@"cornerRadius"];
  animation.fromValue = @(_panel.layer.cornerRadius);
  animation.toValue = @(radius);
  animation.duration = duration;
  animation.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
  [_panel.layer addAnimation:animation forKey:@"cornerRadius"];
  _panel.layer.cornerRadius = radius;
}

- (BOOL)growsFromButton {
  return !CGRectIsEmpty(_sourceRect) && !UIAccessibilityIsReduceMotionEnabled();
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  if (_expanded || !_panel || CGRectIsEmpty(_panel.frame)) {
    return;
  }
  _expanded = YES;
  if (![self growsFromButton]) {
    return;
  }
  // Start as the button: the card laid over it, round as a pill, its contents not yet visible.
  const CGRect from = [self.view convertRect:_sourceRect fromView:nil];
  _panel.transform = [self collapsedTransform];
  const CGFloat scale_y = from.size.height / MAX(_panel.bounds.size.height, 1.0);
  _panel.layer.cornerRadius = MIN(from.size.height / 2.0 / MAX(scale_y, 0.01), 400.0);
  [self setCardContentAlpha:0.0];
  const UIColor* dim = self.view.backgroundColor;
  self.view.backgroundColor = [UIColor clearColor];
  __unsafe_unretained XeniaIOSOnlineSheetViewController* unsafe_self = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    [unsafe_self setCornerRadiusAnimated:[XeniaTheme glassCardRadius] duration:0.45];
    [UIView animateWithDuration:0.55
                          delay:0.0
         usingSpringWithDamping:0.82
          initialSpringVelocity:0.0
                        options:UIViewAnimationOptionBeginFromCurrentState
                     animations:^{
                       unsafe_self->_panel.transform = CGAffineTransformIdentity;
                       unsafe_self.view.backgroundColor = (UIColor*)dim;
                     }
                     completion:nil];
    [UIView animateWithDuration:0.25
                          delay:0.14
                        options:UIViewAnimationOptionCurveEaseOut
                     animations:^{
                       [unsafe_self setCardContentAlpha:1.0];
                     }
                     completion:nil];
  });
}

// Closing sends the card back toward the button it came from: it shrinks evenly (never squashed
// out of proportion) and fades as it goes, with the dim lifting at the same pace. The spring
// front-loads the motion, so the shrink is seen before the card is gone.
- (void)closeAnimated {
  if (_closing) {
    return;
  }
  _closing = YES;
  if (![self growsFromButton]) {
    [self dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  const CGRect from = [self.view convertRect:_sourceRect fromView:nil];
  const CGRect to = _panel.frame;
  const CGFloat scale = 0.5;
  const CGFloat travel = 0.6;  // most of the way to the button, so it reads as going back in
  const CGAffineTransform target = CGAffineTransformScale(
      CGAffineTransformMakeTranslation((CGRectGetMidX(from) - CGRectGetMidX(to)) * travel,
                                       (CGRectGetMidY(from) - CGRectGetMidY(to)) * travel),
      scale, scale);
  __unsafe_unretained XeniaIOSOnlineSheetViewController* unsafe_self = self;
  [UIView animateWithDuration:0.12
                   animations:^{
                     [unsafe_self setCardContentAlpha:0.0];
                   }];
  // A blur ignores its parent's alpha, so it fades by losing its effect; otherwise a frosted
  // slab lingers after the card has gone.
  [UIView animateWithDuration:0.3
      delay:0.0
      usingSpringWithDamping:1.0
      initialSpringVelocity:0.0
      options:UIViewAnimationOptionBeginFromCurrentState
      animations:^{
        unsafe_self->_panel.transform = target;
        unsafe_self->_panel.alpha = 0.0;
        for (UIView* view in unsafe_self->_panel.subviews) {
          if ([view isKindOfClass:[UIVisualEffectView class]]) {
            ((UIVisualEffectView*)view).effect = nil;
          }
        }
        unsafe_self.view.backgroundColor = [UIColor clearColor];
      }
      completion:^(BOOL __unused finished) {
        [unsafe_self dismissViewControllerAnimated:NO completion:nil];
      }];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  // Dark glass whatever the system setting, like the panel over a game.
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  // The home screen stays visible and dimmed; the panel floats over it as the glass card it is
  // in the game menu. A tap beside it closes it.
  self.view.backgroundColor = [XeniaTheme glassDim];
  UIButton* outside = [UIButton buttonWithType:UIButtonTypeCustom];
  outside.translatesAutoresizingMaskIntoConstraints = NO;
  outside.isAccessibilityElement = NO;
  [self.view addSubview:outside];
  [NSLayoutConstraint activateConstraints:@[
    [outside.topAnchor constraintEqualToAnchor:self.view.topAnchor],
    [outside.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [outside.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [outside.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
  ]];
  __unsafe_unretained XeniaIOSOnlineSheetViewController* unsafe_close = self;
  [outside addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
             [unsafe_close closeAnimated];
           }]
    forControlEvents:UIControlEventTouchUpInside];

  __unsafe_unretained XeniaIOSOnlineSheetViewController* unsafe_self = self;
  _panel = [[XeniaIOSOnlinePanel alloc] initWithFrame:CGRectZero];
  _panel.closeHandler = ^{
    [unsafe_self closeAnimated];
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
  _panel.fitsContent = YES;
  // The same Liquid Glass card as Profiles and Public Games.
  XEInstallGlassCardSurface(_panel);
  [self.view addSubview:_panel];
  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  const CGFloat inset = [XeniaTheme glassCardInset];
  // As wide as the other cards: the screen less its insets on a phone, at most
  // kXEGlassCardMaxWidth and centred on iPad.
  NSLayoutConstraint* widest = [_panel.widthAnchor constraintEqualToConstant:kXEGlassCardMaxWidth];
  widest.priority = UILayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    [_panel.topAnchor constraintEqualToAnchor:safe.topAnchor constant:4],
    [_panel.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor constant:-4],
    [_panel.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
    [_panel.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:inset],
    [_panel.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-inset],
    widest,
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


namespace {

// "1,284": full numbers in text (badges go compact).
NSString* FullCount(NSInteger count) {
  return [NSNumberFormatter localizedStringFromNumber:@(count)
                                          numberStyle:NSNumberFormatterDecimalStyle];
}

NSString* PlayersText(NSInteger count) {
  return count == 1 ? NSLocalizedString(@"1 player", @"Public Games: count")
                    : [NSString localizedStringWithFormat:NSLocalizedString(
                                                              @"%@ players",
                                                              @"Public Games: count; %@ is a "
                                                              @"formatted number, \"1,284\""),
                                                          FullCount(count)];
}

// Two or three faces overlapping, the rest as "+2".
UIView* AvatarStack(NSArray<NSString*>* names) {
  NSMutableArray<UIView*>* faces = [NSMutableArray array];
  const NSUInteger shown = names.count > 3 ? 2 : names.count;
  for (NSUInteger i = 0; i < shown; ++i) {
    UIView* face = Avatar(names[i], 30.0, NO);
    face.layer.borderWidth = 1.5;
    face.layer.borderColor = [UIColor colorWithWhite:0.0 alpha:0.35].CGColor;
    [faces addObject:face];
  }
  if (names.count > shown) {
    UILabel* more = (UILabel*)Avatar(@"+", 30.0, NO);
    more.text = [NSString stringWithFormat:@"+%lu", (unsigned long)(names.count - shown)];
    more.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.18];
    more.font = [UIFont systemFontOfSize:11.0 weight:UIFontWeightSemibold];
    [faces addObject:more];
  }
  UIStackView* stack = [[[UIStackView alloc] initWithArrangedSubviews:faces] autorelease];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  stack.spacing = -8.0;
  stack.isAccessibilityElement = NO;
  [stack setContentHuggingPriority:UILayoutPriorityRequired
                           forAxis:UILayoutConstraintAxisHorizontal];
  return stack;
}

// "NobleSix, Arbiter and 2 more".
NSString* JoinedNames(NSArray<NSString*>* names, NSUInteger shown) {
  if (names.count <= shown) {
    return [NSListFormatter localizedStringByJoiningStrings:names];
  }
  NSArray<NSString*>* head = [names subarrayWithRange:NSMakeRange(0, shown)];
  return [NSString localizedStringWithFormat:NSLocalizedString(
                                                 @"%@ and %lu more",
                                                 @"Public Games: friends row; %@ is names, %lu "
                                                 @"how many others"),
                                             [head componentsJoinedByString:@", "],
                                             (unsigned long)(names.count - shown)];
}

}  // namespace

@implementation XeniaIOSPublicGamesViewController {
  NSDictionary<NSNumber*, NSString*>* _library;
  void (^_openGame)(uint32_t);
  void (^_openFriends)(void);
  UIScrollView* _scroll;
  UIStackView* _content;
  UIStackView* _list;
  UILabel* _subtitle;
  UIView* _dot;
  UISegmentedControl* _filter;
  NSMutableSet<NSNumber*>* _coverFetches;
  BOOL _filterChosen;
  CFAbsoluteTime _openedAt;
}

@synthesize openFriendsHandler = _openFriends;

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
  [NSObject cancelPreviousPerformRequestsWithTarget:self];
  [_library release];
  [_openGame release];
  [_openFriends release];
  [_scroll release];
  [_content release];
  [_list release];
  [_subtitle release];
  [_dot release];
  [_filter release];
  [_coverFetches release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  // A glass card from the globe: the card's own glass is the backdrop, its bar holds the title
  // and the ✕.
  self.view.backgroundColor = [UIColor clearColor];
  self.title = NSLocalizedString(@"Public Games", @"Public Games: page title");
  XESetSheetClose(self, self, @selector(closeTapped));
  _openedAt = CFAbsoluteTimeGetCurrent();

  _dot = [[UIView alloc] init];
  _dot.translatesAutoresizingMaskIntoConstraints = NO;
  _dot.backgroundColor = [XeniaTheme accent];
  _dot.layer.cornerRadius = 4.0;
  [NSLayoutConstraint activateConstraints:@[
    [_dot.widthAnchor constraintEqualToConstant:8],
    [_dot.heightAnchor constraintEqualToConstant:8],
  ]];
  _subtitle = [NewLabel(@"", UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular,
                        [XeniaTheme textSecondary]) retain];
  _subtitle.numberOfLines = 0;
  UIStackView* subtitle_row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ _dot, _subtitle ]] autorelease];
  subtitle_row.spacing = 7;
  subtitle_row.alignment = UIStackViewAlignmentCenter;
  subtitle_row.layoutMargins = UIEdgeInsetsMake(0, 4, 0, 4);
  subtitle_row.layoutMarginsRelativeArrangement = YES;

  _filter = [[UISegmentedControl alloc] initWithItems:@[
    NSLocalizedString(@"In My Library", @"Public Games: filter"),
    NSLocalizedString(@"All Games", @"Public Games: filter"),
  ]];
  _filter.translatesAutoresizingMaskIntoConstraints = NO;
  _filter.selectedSegmentIndex = 0;
  [_filter addTarget:self action:@selector(filterChanged) forControlEvents:UIControlEventValueChanged];

  _list = [[UIStackView alloc] init];
  _list.translatesAutoresizingMaskIntoConstraints = NO;
  _list.axis = UILayoutConstraintAxisVertical;
  _list.spacing = 8;

  _scroll = [[UIScrollView alloc] init];
  _scroll.translatesAutoresizingMaskIntoConstraints = NO;
  _scroll.alwaysBounceVertical = NO;
  _content = [[UIStackView alloc] initWithArrangedSubviews:@[ subtitle_row, _filter, _list ]];
  _content.translatesAutoresizingMaskIntoConstraints = NO;
  _content.axis = UILayoutConstraintAxisVertical;
  _content.spacing = 14;
  [_scroll addSubview:_content];
  [self.view addSubview:_scroll];

  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [_scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
    [_scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [_scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [_scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [_content.topAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.topAnchor constant:2],
    [_content.bottomAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.bottomAnchor
                                          constant:-16],
    [_content.leadingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.leadingAnchor
                                           constant:16],
    [_content.trailingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.trailingAnchor
                                            constant:-16],
  ]];

  for (NSNotificationName name : @[ XeniaIOSSystemLinkOnlineDidChangeNotification,
                                    XeniaLiveSocialDidChangeNotification ]) {
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(reload)
                                                 name:name
                                               object:nil];
  }
  [self reload];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  xe_ios_system_link_keep_online(YES);
  xe_ios_system_link_refresh_online();
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  // As tall as its content, up to the screen.
  XEFitGlassCardToHeight(self, _scroll.contentSize.height);
}

- (void)closeTapped {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)filterChanged {
  _filterChosen = YES;
  [self reload];
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
  view.layer.cornerRadius = 5;
  view.layer.cornerCurve = kCACornerCurveContinuous;
  view.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.1];
  view.isAccessibilityElement = NO;
  if (!cover) {
    view.image = [UIImage systemImageNamed:@"gamecontroller.fill"];
    view.contentMode = UIViewContentModeCenter;
    view.tintColor = [UIColor colorWithWhite:1.0 alpha:0.4];
    view.preferredSymbolConfiguration =
        [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleCaption2];
  }
  [NSLayoutConstraint activateConstraints:@[
    [view.widthAnchor constraintEqualToConstant:30],
    [view.heightAnchor constraintEqualToConstant:40],
  ]];
  return view;
}

- (NSString*)nameForTitle:(uint32_t)title_id {
  return _library[@(title_id)] ?: xe_ios_game_name_for_title(title_id);
}

- (void)openTitle:(uint32_t)title_id {
  void (^open)(uint32_t) = [[_openGame copy] autorelease];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             if (open) open(title_id);
                           }];
}

- (void)openFriends {
  void (^open)(void) = [[_openFriends copy] autorelease];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             if (open) open();
                           }];
}

// One row at the top for every friend in a game: faces, names, which games.
- (UIView*)friendsRowForGames:(NSDictionary<NSNumber*, NSNumber*>*)counts {
  NSMutableArray<NSString*>* names = [NSMutableArray array];
  NSMutableOrderedSet<NSString*>* games = [NSMutableOrderedSet orderedSet];
  for (XeniaLiveFriend* peer in [XeniaLiveSocial shared].friends) {
    if (!peer.online || !peer.titleId) {
      continue;
    }
    [names addObject:peer.gamertag];
    [games addObject:[self nameForTitle:peer.titleId]];
  }
  if (!names.count) {
    return nil;
  }
  NSString* where =
      games.count <= 2
          ? [NSString localizedStringWithFormat:NSLocalizedString(@"In %@",
                                                                  @"Public Games: friends row; "
                                                                  @"%@ is one or two games"),
                                                [NSListFormatter localizedStringByJoiningStrings:
                                                                     games.array]]
          : [NSString localizedStringWithFormat:NSLocalizedString(
                                                    @"In %@ and %lu other games",
                                                    @"Public Games: friends row"),
                                                games.firstObject,
                                                (unsigned long)(games.count - 1)];
  __unsafe_unretained XeniaIOSPublicGamesViewController* unsafe_self = self;
  void (^tapped)(void) = nil;
  if (_openFriends) {
    tapped = ^{
      [unsafe_self openFriends];
    };
  }
  return Row(AvatarStack(names), JoinedNames(names, 2), where, NO,
             tapped ? @[ Chevron() ] : @[], tapped);
}

- (void)reload {
  if (!self.isViewLoaded) {
    return;
  }
  [self rebuildList];
  // The list arrives after the card opens (and changes with the filter): fit the card to it now,
  // since the scroll view's own layout doesn't reach viewDidLayoutSubviews.
  [_scroll layoutIfNeeded];
  XEFitGlassCardToHeight(self, _scroll.contentSize.height);
}

- (void)rebuildList {
  for (UIView* row in [[_list.arrangedSubviews copy] autorelease]) {
    [_list removeArrangedSubview:row];
    [row removeFromSuperview];
  }
  const BOOL connected = xe_ios_system_link_online_connected();
  const NSInteger others = xe_ios_system_link_players_elsewhere();

  NSDictionary<NSNumber*, NSNumber*>* counts = xe_ios_system_link_online_playing();
  if (!counts.count) {
    counts = xe_ios_system_link_online_titles();
  }
  NSMutableArray<NSNumber*>* all = [NSMutableArray array];
  NSMutableArray<NSNumber*>* mine = [NSMutableArray array];
  for (NSNumber* title_id in counts) {
    if (counts[title_id].integerValue <= 0) {
      continue;
    }
    [all addObject:title_id];
    if (_library[title_id]) {
      [mine addObject:title_id];
    }
  }
  // Busiest first; a tie keeps a stable order.
  NSComparator busiest = ^NSComparisonResult(NSNumber* a, NSNumber* b) {
    const NSComparisonResult by_count = [counts[b] compare:counts[a]];
    return by_count != NSOrderedSame ? by_count : [a compare:b];
  };
  [all sortUsingComparator:busiest];
  [mine sortUsingComparator:busiest];

  // Opens on your games when any have players; after that it's your choice.
  if (!_filterChosen) {
    _filter.selectedSegmentIndex = mine.count || !all.count ? 0 : 1;
  }
  const BOOL library_only = _filter.selectedSegmentIndex == 0;
  NSArray<NSNumber*>* games = library_only ? mine : all;

  _dot.hidden = !connected || others <= 0;
  if (!connected) {
    _subtitle.text = NSLocalizedString(@"Not connected", @"Public Games: subtitle");
  } else {
    NSString* players =
        others <= 0 ? NSLocalizedString(@"Nobody else online", @"Public Games: subtitle")
                    : [NSString localizedStringWithFormat:NSLocalizedString(
                                                              @"%@ on XeniOS",
                                                              @"Public Games: subtitle; %@ is "
                                                              @"\"1,284 players\""),
                                                          PlayersText(others)];
    _subtitle.text =
        all.count == 0 ? players
        : all.count == 1
            ? [NSString stringWithFormat:@"%@ · %@", players,
                                         NSLocalizedString(@"1 game", @"Public Games: subtitle")]
            : [NSString stringWithFormat:@"%@ · %@", players,
                                         [NSString localizedStringWithFormat:
                                                       NSLocalizedString(@"%lu games",
                                                                         @"Public Games: subtitle"),
                                                       (unsigned long)all.count]];
  }
  _filter.hidden = !connected || !all.count;

  __unsafe_unretained XeniaIOSPublicGamesViewController* unsafe_self = self;
  if (!connected) {
    // The lobby takes a moment on open: a few seconds of "Connecting" before saying it can't.
    const CFAbsoluteTime waited = CFAbsoluteTimeGetCurrent() - _openedAt;
    if (waited < 4.0) {
      _subtitle.text = NSLocalizedString(@"Connecting…", @"Public Games: subtitle");
      [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(reload) object:nil];
      [self performSelector:@selector(reload) withObject:nil afterDelay:4.0 - waited + 0.05];
      UIActivityIndicatorView* spinner = [[[UIActivityIndicatorView alloc]
          initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
      [spinner startAnimating];
      [_list addArrangedSubview:Group(@[ [self paddedState:spinner] ], nil)];
      return;
    }
    [_list addArrangedSubview:
               EmptyStateWithButton(@"waveform.path.ecg",
                                    NSLocalizedString(@"Can't reach Public Games",
                                                      @"Public Games: offline state"),
                                    NSLocalizedString(@"Your games still work offline.",
                                                      @"Public Games: offline state"),
                                    NSLocalizedString(@"Try Again", @"Public Games: button"), ^{
                                      unsafe_self->_openedAt = CFAbsoluteTimeGetCurrent();
                                      xe_ios_system_link_refresh_online();
                                      [unsafe_self reload];
                                    })];
    return;
  }

  UIView* friends = [self friendsRowForGames:counts];
  if (friends) {
    [_list addArrangedSubview:SectionLabel(NSLocalizedString(@"Friends playing",
                                                             @"Public Games: section"))];
    [_list addArrangedSubview:Group(@[ friends ], nil)];
  }

  if (!all.count) {
    [_list addArrangedSubview:EmptyState(@"gamecontroller",
                                         NSLocalizedString(@"No public games right now",
                                                           @"Public Games: empty state"),
                                         NSLocalizedString(@"Start one from your library and it "
                                                           @"shows up here.",
                                                           @"Public Games: empty state"))];
    return;
  }
  if (!games.count) {
    // Your games are quiet, others aren't: say so, and offer the way out.
    [_list addArrangedSubview:
               EmptyStateWithButton(@"line.3.horizontal.decrease",
                                    NSLocalizedString(@"None of your games right now",
                                                      @"Public Games: filtered state"),
                                    all.count == 1
                                        ? NSLocalizedString(@"1 other game has players.",
                                                            @"Public Games: filtered state")
                                        : [NSString localizedStringWithFormat:
                                                        NSLocalizedString(
                                                            @"%lu other games have players.",
                                                            @"Public Games: filtered state"),
                                                        (unsigned long)all.count],
                                    NSLocalizedString(@"Show All Games", @"Public Games: button"),
                                    ^{
                                      unsafe_self->_filterChosen = YES;
                                      unsafe_self->_filter.selectedSegmentIndex = 1;
                                      [unsafe_self reload];
                                    })];
    return;
  }

  NSMutableArray<UIView*>* rows = [NSMutableArray array];
  for (NSNumber* number in games) {
    const uint32_t title_id = number.unsignedIntValue;
    const BOOL in_library = _library[number] != nil;
    NSString* detail = PlayersText(counts[number].integerValue);
    if (!library_only && !in_library) {
      detail = [NSString stringWithFormat:@"%@ · %@", detail,
                                          NSLocalizedString(@"Not in your library",
                                                            @"Public Games: row detail")];
    }
    void (^tapped)(void) = nil;
    if (in_library) {
      tapped = ^{
        [unsafe_self openTitle:title_id];
      };
    }
    UIView* row = Row([self coverForTitle:title_id], [self nameForTitle:title_id], detail, NO,
                      in_library ? @[ Chevron() ] : @[], tapped);
    [rows addObject:row];
  }
  [_list addArrangedSubview:SectionLabel(library_only
                                             ? NSLocalizedString(@"Most played · your games",
                                                                 @"Public Games: section")
                                             : NSLocalizedString(@"Most played",
                                                                 @"Public Games: section"))];
  [_list addArrangedSubview:Group(rows, nil)];
}

- (UIView*)paddedState:(UIView*)view {
  UIStackView* column = [[[UIStackView alloc] initWithArrangedSubviews:@[ view ]] autorelease];
  column.axis = UILayoutConstraintAxisVertical;
  column.alignment = UIStackViewAlignmentCenter;
  column.layoutMargins = UIEdgeInsetsMake(28, 0, 28, 0);
  column.layoutMarginsRelativeArrangement = YES;
  return column;
}

@end

// Sends the request; if the lobby turns it down soon after (no such code, a full list), says why
// in an alert, since no Add Friend page is open to show it.
static void xe_ios_send_friend_request_reporting_failure(NSString* code,
                                                         UIViewController* presenter) {
  __block id observer = nil;
  void (^stop)(void) = [[^{
    if (observer) {
      [[NSNotificationCenter defaultCenter] removeObserver:observer];
      [observer release];
      observer = nil;
    }
  } copy] autorelease];
  observer = [[[NSNotificationCenter defaultCenter]
      addObserverForName:XeniaLiveSocialRequestFailedNotification
                  object:nil
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification* note) {
                stop();
                UIAlertController* alert = [UIAlertController
                    alertControllerWithTitle:NSLocalizedString(@"Couldn’t Send Request",
                                                               @"Friend link alert title")
                                     message:note.userInfo[XeniaLiveSocialMessageKey]
                              preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK",
                                                                                  @"Alert button")
                                                          style:UIAlertActionStyleCancel
                                                        handler:nil]];
                UIViewController* top = presenter.view.window ? presenter : nil;
                while (top.presentedViewController && !top.presentedViewController.isBeingDismissed) {
                  top = top.presentedViewController;
                }
                [top presentViewController:alert animated:YES completion:nil];
              }] retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(8 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), stop);
  [[XeniaLiveSocial shared] addFriendWithCode:code];
}

BOOL xe_ios_handle_friend_url(NSURL* url, UIViewController* presenter) {
  if (![url.scheme.lowercaseString isEqualToString:@"xenios"] ||
      ![url.host.lowercaseString isEqualToString:@"friend"]) {
    return NO;
  }
  UIViewController* top = presenter;
  while (top.presentedViewController && !top.presentedViewController.isBeingDismissed) {
    top = top.presentedViewController;
  }
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  NSString* code = xe_ios_friend_code_from_text(url.absoluteString);
  NSString* title = nil;
  NSString* message = nil;
  if (!code) {
    title = NSLocalizedString(@"This Link Doesn’t Work", @"Friend link alert title");
    message = NSLocalizedString(@"Ask your friend to share their code again.",
                                @"Friend link alert: malformed code");
  } else if (social.state != XeniaLiveSocialStateOnline || !social.friendCode.length) {
    title = NSLocalizedString(@"You’re Not Online", @"Friend link alert title");
    message = NSLocalizedString(@"Get a Gamertag in Profiles, then open the link again.",
                                @"Friend link alert: not signed in");
  } else if ([code isEqualToString:social.friendCode]) {
    title = NSLocalizedString(@"That’s Your Own Code", @"Friend link alert title");
    message = NSLocalizedString(@"Send it to a friend so they can add you.",
                                @"Friend link alert: own code");
  }
  UIAlertController* alert = nil;
  if (title) {
    alert = [UIAlertController alertControllerWithTitle:title
                                                message:message
                                         preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
  } else {
    // Any page can open a xenios:// link, so the player says yes before a request goes out.
    alert = [UIAlertController
        alertControllerWithTitle:NSLocalizedString(@"Add Friend?", @"Friend link alert title")
                         message:[NSString stringWithFormat:
                                               NSLocalizedString(
                                                   @"Send a friend request to the player with "
                                                   @"code %@.",
                                                   @"Friend link alert; %@ is their friend code"),
                                               xe_ios_format_friend_code(code)]
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    UIAlertAction* send = [UIAlertAction
        actionWithTitle:NSLocalizedString(@"Send Request", @"Friend link alert button")
                  style:UIAlertActionStyleDefault
                handler:^(UIAlertAction* __unused a) {
                  xe_ios_send_friend_request_reporting_failure(code, top);
                }];
    [alert addAction:send];
    alert.preferredAction = send;
  }
  [top presentViewController:alert animated:YES completion:nil];
  return YES;
}
