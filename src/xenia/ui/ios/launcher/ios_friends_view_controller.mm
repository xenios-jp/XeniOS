/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_friends_view_controller.h"

#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/shared/ios_status_toast.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

typedef NS_ENUM(NSInteger, FriendsSection) {
  kSectionStatus,
  kSectionInvites,
  kSectionRequests,
  kSectionParty,
  kSectionInGame,
  kSectionOnline,
  kSectionOffline,
  kSectionSent,
  kSectionNoFriends,
  kSectionGames,
};

constexpr CGFloat kAvatarSize = 40.0;
NSString* const kCellId = @"XeniaFriendCell";

// A row's text, secondary text and image, as the design's list rows (R4).
UIListContentConfiguration* RowContent(NSString* text, NSString* secondary, UIImage* image) {
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = text;
  content.secondaryText = secondary;
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.font = xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightSemibold);
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.secondaryTextProperties.font =
      [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  content.image = image;
  content.imageProperties.maximumSize = CGSizeMake(kAvatarSize, kAvatarSize);
  content.imageProperties.reservedLayoutSize = CGSizeMake(kAvatarSize, kAvatarSize);
  return content;
}

// "● Playing Halo 3": the library tiles' coloured status dot, then the text.
NSAttributedString* DotText(NSString* text, UIColor* dot) {
  UIFont* font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  NSMutableAttributedString* result = [[[NSMutableAttributedString alloc]
      initWithString:@"\u25CF "
          attributes:@{NSForegroundColorAttributeName : dot, NSFontAttributeName : font}]
      autorelease];
  [result appendAttributedString:[[[NSAttributedString alloc]
                                     initWithString:text
                                         attributes:@{
                                           NSForegroundColorAttributeName : [XeniaTheme textSecondary],
                                           NSFontAttributeName : font
                                         }] autorelease]];
  return result;
}

// Green online, blue in a game (as a tile's In-Game), grey offline.
UIColor* PresenceDot(XeniaLiveFriend* peer) {
  if (!peer.online) {
    return [XeniaTheme textMuted];
  }
  return peer.titleId ? [UIColor systemBlueColor] : [XeniaTheme accent];
}

UIImage* Avatar(NSString* gamertag, BOOL online) {
  return xe_profile_avatar_image(gamertag, online, kAvatarSize);
}

// A small capsule button for a row's accessory: filled with the accent for the row's one action,
// tinted otherwise.
UIButton* RowButton(NSString* title, NSString* symbol, BOOL primary, NSString* label,
                    UIAction* action) {
  UIButtonConfiguration* config = primary ? [UIButtonConfiguration filledButtonConfiguration]
                                          : [UIButtonConfiguration tintedButtonConfiguration];
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.buttonSize = UIButtonConfigurationSizeSmall;
  if (primary) {
    config.baseBackgroundColor = [XeniaTheme accent];
    config.baseForegroundColor = [XeniaTheme accentFg];
  } else {
    config.baseBackgroundColor = [XeniaTheme textSecondary];
    config.baseForegroundColor = [XeniaTheme textPrimary];
  }
  config.title = title;
  if (symbol) {
    config.image = [UIImage systemImageNamed:symbol];
    config.preferredSymbolConfigurationForImage =
        [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote
                                                         scale:UIImageSymbolScaleMedium];
  }
  config.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
      NSDictionary<NSAttributedStringKey, id>* attributes) {
    NSMutableDictionary* updated = [[attributes mutableCopy] autorelease];
    updated[NSFontAttributeName] =
        xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
    return updated;
  };
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:action];
  button.accessibilityLabel = label ?: title;
  [button sizeToFit];
  return button;
}

UIView* Accessory(NSArray<UIView*>* views) {
  UIStackView* stack = [[[UIStackView alloc] initWithArrangedSubviews:views] autorelease];
  stack.axis = UILayoutConstraintAxisHorizontal;
  stack.spacing = 8.0;
  stack.alignment = UIStackViewAlignmentCenter;
  stack.frame = CGRectMake(0, 0, [stack systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].width,
                           36.0);
  return stack;
}

// The cover of the game a friend is in, as a small portrait box beside the row's action.
UIImageView* CoverView(uint32_t title_id) {
  UIImage* art = xe_cached_game_art_thumbnail(title_id);
  UIImageView* view = [[[UIImageView alloc] initWithFrame:CGRectMake(0, 0, 27, 36)] autorelease];
  view.contentMode = UIViewContentModeScaleAspectFill;
  view.clipsToBounds = YES;
  view.layer.cornerRadius = XeniaRadiusXs;
  view.layer.cornerCurve = kCACornerCurveContinuous;
  view.backgroundColor = [[XeniaTheme textPrimary] colorWithAlphaComponent:0.08];
  view.image = art;
  view.isAccessibilityElement = NO;
  [view.widthAnchor constraintEqualToConstant:27.0].active = YES;
  [view.heightAnchor constraintEqualToConstant:36.0].active = YES;
  return view;
}

NSString* AgoText(NSDate* date) {
  NSRelativeDateTimeFormatter* formatter =
      [[[NSRelativeDateTimeFormatter alloc] init] autorelease];
  formatter.unitsStyle = NSRelativeDateTimeFormatterUnitsStyleShort;
  const NSTimeInterval age = -date.timeIntervalSinceNow;
  if (age < 60.0) {
    return NSLocalizedString(@"just now", @"Friends: an invite that came less than a minute ago");
  }
  return [formatter localizedStringForDate:date relativeToDate:[NSDate date]];
}

}  // namespace

#pragma mark - Friends

@interface XeniaIOSFriendsViewController () <UISearchResultsUpdating>
@end

@implementation XeniaIOSFriendsViewController {
  UISearchController* search_controller_;
  NSString* query_;
  // A big party folds into one row, so the friends below it stay in reach.
  BOOL party_expanded_;
  XeniaIOSFriendsMode mode_;
  NSString* game_title_;
  NSArray<NSNumber*>* sections_;
  NSArray<XeniaLiveInvite*>* invites_;
  NSArray<XeniaLiveFriend*>* requests_;
  NSArray<XeniaLiveFriend*>* in_game_;
  NSArray<XeniaLiveFriend*>* online_;
  NSArray<XeniaLiveFriend*>* offline_;
  UIBarButtonItem* add_item_;  // Owned by the header.
  NSArray<XeniaLiveFriend*>* sent_;
  // Friends invited from this screen (Invite mode): their button says so.
  NSMutableSet<NSNumber*>* invited_;
  NSMutableSet<NSNumber*>* covers_loading_;
  XeniaIOSStatusToastPresenter* toast_;
  void (^accept_invite_handler_)(XeniaLiveInvite*);
  void (^join_friend_handler_)(XeniaLiveFriend*);
  UIViewController* (^games_online_provider_)(void);
  NSArray<XeniaLivePartyMember*>* party_;
}
@synthesize acceptInviteHandler = accept_invite_handler_;
@synthesize joinFriendHandler = join_friend_handler_;
@synthesize gamesOnlineProvider = games_online_provider_;

- (instancetype)initWithMode:(XeniaIOSFriendsMode)mode gameTitle:(NSString*)game_title {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    mode_ = mode;
    game_title_ = [game_title copy];
    invited_ = [[NSMutableSet alloc] init];
    covers_loading_ = [[NSMutableSet alloc] init];
    toast_ = [[XeniaIOSStatusToastPresenter alloc] init];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [search_controller_ release];
  [query_ release];
  [game_title_ release];
  [sections_ release];
  [invites_ release];
  [requests_ release];
  [in_game_ release];
  [online_ release];
  [offline_ release];
  [sent_ release];
  [invited_ release];
  [covers_loading_ release];
  [toast_ release];
  [accept_invite_handler_ release];
  [join_friend_handler_ release];
  [games_online_provider_ release];
  [party_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = mode_ == XeniaIOSFriendsModeInvite
                   ? NSLocalizedString(@"Invite Friends", @"Friends screen title from the game menu")
                   : NSLocalizedString(@"Friends", @"Online screen title: friends, invites, party "
                                                  @"and games online");
  if (game_title_.length) {
    self.navigationItem.subtitle = game_title_;
  }
  XESetSheetClose(self, self, @selector(closeTapped:));
  UIBarButtonItem* add = [[[UIBarButtonItem alloc]
      initWithImage:[UIImage systemImageNamed:@"person.badge.plus"]
              style:UIBarButtonItemStylePlain
             target:self
             action:@selector(addTapped:)] autorelease];
  add.accessibilityLabel = NSLocalizedString(@"Add Friend", @"Friends: add friend button");
  add_item_ = add;
  XESetSheetActions(self, @[ add ]);
  [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:kCellId];
  // With dozens of friends the list needs a way in: search covers all of them, offline too.
  search_controller_ = [[UISearchController alloc] initWithSearchResultsController:nil];
  search_controller_.searchResultsUpdater = self;
  search_controller_.obscuresBackgroundDuringPresentation = NO;
  search_controller_.searchBar.placeholder =
      NSLocalizedString(@"Search friends", @"Friends: search field placeholder");
  self.navigationItem.searchController = search_controller_;
  self.navigationItem.hidesSearchBarWhenScrolling = YES;
  self.definesPresentationContext = YES;

  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [center addObserver:self
             selector:@selector(socialChanged:)
                 name:XeniaLiveSocialDidChangeNotification
               object:nil];
  [center addObserver:self
             selector:@selector(requestFailed:)
                 name:XeniaLiveSocialRequestFailedNotification
               object:nil];
  [center addObserver:self
             selector:@selector(socialChanged:)
                 name:XeniaIOSSystemLinkOnlineDidChangeNotification
               object:nil];
  [[XeniaLiveSocial shared] refresh];
  [self reload];
}

- (void)closeTapped:(id)__unused sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)addTapped:(id)__unused sender {
  XeniaIOSAddFriendViewController* add =
      [[[XeniaIOSAddFriendViewController alloc] initWithStyle:UITableViewStyleInsetGrouped]
          autorelease];
  [self.navigationController pushViewController:add animated:YES];
}

- (void)socialChanged:(NSNotification*)__unused note {
  [self reload];
}

- (void)requestFailed:(NSNotification*)note {
  if (!self.view.window) {
    return;
  }
  [toast_ presentMessage:note.userInfo[XeniaLiveSocialMessageKey]
                   style:XeniaIOSStatusToastStyleWarning
                  inView:self.navigationController.view ?: self.view];
}

- (void)showToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style {
  [toast_ presentMessage:message style:style inView:self.navigationController.view ?: self.view];
}

#pragma mark Data

- (void)reload {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  const BOOL online = social.state == XeniaLiveSocialStateOnline;
  NSMutableArray* in_game = [NSMutableArray array];
  NSMutableArray* online_friends = [NSMutableArray array];
  NSMutableArray* offline_friends = [NSMutableArray array];
  // While searching, only matching friends show; invites, party and games step aside.
  const BOOL searching = online && query_.length > 0;
  if (online) {
    for (XeniaLiveFriend* peer in social.friends) {
      if (searching && ![peer.gamertag localizedStandardContainsString:query_]) {
        continue;
      }
      if (peer.online && peer.titleId) {
        [in_game addObject:peer];
      } else if (peer.online) {
        [online_friends addObject:peer];
      } else {
        [offline_friends addObject:peer];
      }
    }
  }
#define XE_SET(ivar, value) \
  [ivar release];           \
  ivar = [(value) retain];
  XE_SET(invites_, online && !searching ? social.invites : @[]);
  XE_SET(requests_, online && !searching ? social.incomingRequests : @[]);
  XE_SET(in_game_, in_game);
  XE_SET(online_, online_friends);
  XE_SET(offline_, offline_friends);
  XE_SET(sent_, online && !searching ? social.outgoingRequests : @[]);
  XE_SET(party_, online && !searching ? social.partyMembers : @[]);
#undef XE_SET

  NSMutableArray* sections = [NSMutableArray array];
  if (!online) {
    [sections addObject:@(kSectionStatus)];
  }
  if (invites_.count) [sections addObject:@(kSectionInvites)];
  if (requests_.count) [sections addObject:@(kSectionRequests)];
  // The party panel: its members, or Create Party.
  if (online && !searching && (party_.count || mode_ == XeniaIOSFriendsModeBrowse)) {
    [sections addObject:@(kSectionParty)];
  }
  if (in_game_.count) [sections addObject:@(kSectionInGame)];
  if (online_.count) [sections addObject:@(kSectionOnline)];
  if (offline_.count) [sections addObject:@(kSectionOffline)];
  if (sent_.count) [sections addObject:@(kSectionSent)];
  if (online && !searching && [self hasNoFriends]) [sections addObject:@(kSectionNoFriends)];
  if (!searching && mode_ == XeniaIOSFriendsModeBrowse && games_online_provider_) {
    [sections addObject:@(kSectionGames)];
  }
  [sections_ release];
  sections_ = [sections copy];

  add_item_.enabled = online;
  self.navigationItem.searchController.searchBar.userInteractionEnabled = online;
  self.contentUnavailableConfiguration =
      searching && !sections.count ? [UIContentUnavailableConfiguration searchConfiguration] : nil;
  [self.tableView reloadData];
}

#pragma mark - UISearchResultsUpdating

- (void)updateSearchResultsForSearchController:(UISearchController*)search_controller {
  NSString* query = [search_controller.searchBar.text
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if ([query isEqualToString:query_ ?: @""]) {
    return;
  }
  [query_ release];
  query_ = query.length ? [query copy] : nil;
  [self reload];
}

// No friends yet: a row in the list (beside the party and System Link) rather than a
// full-screen empty state, which would sit over those rows.
- (BOOL)hasNoFriends {
  return !invites_.count && !requests_.count && !in_game_.count && !online_.count &&
         !offline_.count && !sent_.count;
}

- (FriendsSection)sectionAt:(NSInteger)index {
  return (FriendsSection)sections_[(NSUInteger)index].integerValue;
}

- (NSArray*)itemsIn:(FriendsSection)section {
  switch (section) {
    case kSectionInvites:
      return invites_;
    case kSectionRequests:
      return requests_;
    case kSectionInGame:
      return in_game_;
    case kSectionOnline:
      return online_;
    case kSectionOffline:
      return offline_;
    case kSectionSent:
      return sent_;
    case kSectionStatus:
    case kSectionParty:
    case kSectionNoFriends:
    case kSectionGames:
      return nil;
  }
  return nil;
}

#pragma mark Table

- (NSInteger)numberOfSectionsInTableView:(UITableView*)__unused tableView {
  return (NSInteger)sections_.count;
}

- (NSInteger)tableView:(UITableView*)__unused tableView numberOfRowsInSection:(NSInteger)section {
  const FriendsSection kind = [self sectionAt:section];
  if (kind == kSectionStatus) {
    return [XeniaLiveSocial shared].state == XeniaLiveSocialStateProfileOffline ? 2 : 1;
  }
  if (kind == kSectionParty) {
    // A summary row for a big party, the members (unless folded), then Create Party (none) or
    // Invite and Leave.
    return [self partySummaryRows] + [self partyMemberRows] + (party_.count ? 2 : 1);
  }
  if (kind == kSectionGames || kind == kSectionNoFriends) {
    return 1;
  }
  return (NSInteger)[self itemsIn:kind].count;
}

- (NSString*)tableView:(UITableView*)__unused tableView titleForHeaderInSection:(NSInteger)section {
  switch ([self sectionAt:section]) {
    case kSectionStatus:
      return nil;
    case kSectionInvites:
      return NSLocalizedString(@"Invites", @"Friends section header: game and party invites");
    case kSectionRequests:
      return NSLocalizedString(@"Friend Requests", @"Friends section header");
    case kSectionInGame:
      return NSLocalizedString(@"Playing", @"Friends section header: friends in a game");
    case kSectionOnline:
      return NSLocalizedString(@"Online", @"Friends section header");
    case kSectionOffline:
      return NSLocalizedString(@"Offline", @"Friends section header");
    case kSectionSent:
      return NSLocalizedString(@"Sent Requests", @"Friends section header");
    case kSectionParty:
      return NSLocalizedString(@"Party", @"Friends section header");
    case kSectionNoFriends:
      return NSLocalizedString(@"Friends", @"Friends section header");
    case kSectionGames:
      return NSLocalizedString(@"Games", @"Friends section header: games people are playing");
  }
  return nil;
}

- (NSString*)tableView:(UITableView*)__unused tableView titleForFooterInSection:(NSInteger)section {
  switch ([self sectionAt:section]) {
    case kSectionInvites:
      return NSLocalizedString(@"Accepting a game invite starts the game and joins your friend. "
                               @"Invites expire after 10 minutes.",
                               @"Friends footer under game and party invites");
    case kSectionInGame:
      return mode_ == XeniaIOSFriendsModeInvite
                 ? nil
                 : NSLocalizedString(@"Join starts the game your friend is in and joins them.",
                                     @"Friends footer under friends in a game");
    case kSectionSent:
      return NSLocalizedString(@"They become your friend once they accept.",
                               @"Friends footer under sent requests");
    case kSectionNoFriends:
      return NSLocalizedString(
          @"Add a friend with their friend code, and share yours so they can add you.",
          @"Friends empty state text");
    case kSectionParty:
      return party_.count ? nil
                          : NSLocalizedString(@"A party keeps up to 8 friends together from "
                                              @"game to game.",
                                              @"Friends footer under Create Party");
    default:
      return nil;
  }
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellId
                                                          forIndexPath:indexPath];
  cell.accessoryView = nil;
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.accessibilityTraits = UIAccessibilityTraitNone;
  cell.accessibilityLabel = nil;
  const FriendsSection kind = [self sectionAt:indexPath.section];
  switch (kind) {
    case kSectionStatus:
      [self configureStatusCell:cell row:indexPath.row];
      break;
    case kSectionInvites:
      [self configureInviteCell:cell invite:invites_[(NSUInteger)indexPath.row]];
      break;
    case kSectionRequests:
      [self configureRequestCell:cell peer:requests_[(NSUInteger)indexPath.row]];
      break;
    case kSectionSent:
      [self configureSentCell:cell peer:sent_[(NSUInteger)indexPath.row]];
      break;
    case kSectionInGame:
    case kSectionOnline:
    case kSectionOffline:
      [self configureFriendCell:cell peer:[self itemsIn:kind][(NSUInteger)indexPath.row]];
      break;
    case kSectionParty:
      [self configurePartyCell:cell row:indexPath.row];
      break;
    case kSectionGames:
      [self configureGamesCell:cell];
      break;
    case kSectionNoFriends:
      [self configureActionCell:cell
                          title:NSLocalizedString(@"Add a Friend", @"Friends: the row when "
                                                                    @"there are none yet")
                         symbol:@"person.badge.plus"
                        enabled:YES
                    destructive:NO];
      break;
  }
  return cell;
}

- (void)configureStatusCell:(UITableViewCell*)cell row:(NSInteger)row {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  if (row == 1) {
    // Profile offline: the screen's one action.
    UIListContentConfiguration* content =
        RowContent(NSLocalizedString(@"Turn On Online Play", @"Friends action row"), nil,
                   [UIImage systemImageNamed:@"globe"]);
    content.textProperties.color = [XeniaTheme accent];
    content.imageProperties.tintColor = [XeniaTheme accent];
    content.imageProperties.reservedLayoutSize = CGSizeZero;
    cell.contentConfiguration = content;
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    cell.accessibilityTraits = UIAccessibilityTraitButton;
    return;
  }
  NSString* title = nil;
  NSString* symbol = @"person.2";
  switch (social.state) {
    case XeniaLiveSocialStateConnecting:
      title = NSLocalizedString(@"Connecting…", @"Friends status title");
      break;
    case XeniaLiveSocialStateProblem:
      title = NSLocalizedString(@"Can't Connect", @"Friends status title");
      symbol = @"exclamationmark.triangle";
      break;
    case XeniaLiveSocialStateProfileOffline:
      title = NSLocalizedString(@"Online Play Is Off", @"Friends status title");
      break;
    case XeniaLiveSocialStateNoProfile:
      title = NSLocalizedString(@"No Profile", @"Friends status title");
      symbol = @"person.crop.circle.badge.questionmark";
      break;
    case XeniaLiveSocialStateOff:
    case XeniaLiveSocialStateOnline:
      title = NSLocalizedString(@"Friends Are Off", @"Friends status title");
      symbol = @"wifi.slash";
      break;
  }
  UIListContentConfiguration* content =
      RowContent(title, social.statusText, [UIImage systemImageNamed:symbol]);
  content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
  content.imageProperties.tintColor = social.state == XeniaLiveSocialStateProblem
                                          ? [XeniaTheme statusWarning]
                                          : [XeniaTheme accent];
  content.imageProperties.reservedLayoutSize = CGSizeZero;
  cell.contentConfiguration = content;
  if (social.state == XeniaLiveSocialStateConnecting) {
    UIActivityIndicatorView* spinner = [[[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
    [spinner startAnimating];
    cell.accessoryView = spinner;
  }
}

- (void)configureInviteCell:(UITableViewCell*)cell invite:(XeniaLiveInvite*)invite {
  if (invite.party) {
    [self configurePartyInviteCell:cell invite:invite];
    return;
  }
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  NSString* game = [social nameForTitle:invite.titleId];
  NSString* secondary = [NSString
      stringWithFormat:NSLocalizedString(@"Invited you to %1$@ · %2$@",
                                         @"Friends invite row; %1$@ is the game, %2$@ how long "
                                         @"ago (\"2 min ago\")"),
                       game, AgoText(invite.received)];
  cell.contentConfiguration = RowContent(invite.gamertag, secondary, Avatar(invite.gamertag, YES));
  __unsafe_unretained XeniaIOSFriendsViewController* unsafe_self = self;
  XeniaLiveInvite* held = [[invite retain] autorelease];
  UIButton* decline = RowButton(
      nil, @"xmark", NO,
      [NSString stringWithFormat:NSLocalizedString(@"Decline invite from %@",
                                                   @"VoiceOver: decline a game invite; %@ is "
                                                   @"the gamertag"),
                                 invite.gamertag],
      [UIAction actionWithHandler:^(UIAction* __unused a) {
        [[XeniaLiveSocial shared] declineInvite:held];
      }]);
  UIButton* accept = RowButton(
      NSLocalizedString(@"Accept", @"Friends: accept a game invite"), nil, YES,
      [NSString stringWithFormat:NSLocalizedString(@"Accept invite from %@",
                                                   @"VoiceOver: accept a game invite; %@ is the "
                                                   @"gamertag"),
                                 invite.gamertag],
      [UIAction actionWithHandler:^(UIAction* __unused a) {
        [unsafe_self acceptInvite:held];
      }]);
  cell.accessoryView = Accessory(@[ CoverView(invite.titleId), decline, accept ]);
  [self loadCoverFor:invite.titleId];
  cell.accessibilityLabel = [NSString stringWithFormat:@"%@, %@", invite.gamertag, secondary];
}

- (void)configurePartyInviteCell:(UITableViewCell*)cell invite:(XeniaLiveInvite*)invite {
  NSString* secondary = [NSString
      stringWithFormat:NSLocalizedString(@"Invited you to their party · %@",
                                         @"Friends party invite row; %@ is how long ago "
                                         @"(\"2 min ago\")"),
                       AgoText(invite.received)];
  cell.contentConfiguration = RowContent(invite.gamertag, secondary, Avatar(invite.gamertag, YES));
  __unsafe_unretained XeniaIOSFriendsViewController* unsafe_self = self;
  XeniaLiveInvite* held = [[invite retain] autorelease];
  UIButton* decline = RowButton(
      nil, @"xmark", NO,
      [NSString stringWithFormat:NSLocalizedString(@"Decline party invite from %@",
                                                   @"VoiceOver: decline a party invite; %@ is "
                                                   @"the gamertag"),
                                 invite.gamertag],
      [UIAction actionWithHandler:^(UIAction* __unused a) {
        [[XeniaLiveSocial shared] declineInvite:held];
      }]);
  UIButton* join = RowButton(
      NSLocalizedString(@"Join Party", @"Friends: accept a party invite"), nil, YES,
      [NSString stringWithFormat:NSLocalizedString(@"Join %@'s party",
                                                   @"VoiceOver: accept a party invite; %@ is "
                                                   @"the gamertag"),
                                 invite.gamertag],
      [UIAction actionWithHandler:^(UIAction* __unused a) {
        if (![[XeniaLiveSocial shared] acceptPartyInvite:held]) {
          [unsafe_self showToast:NSLocalizedString(@"Couldn't join the party.", @"Party toast")
                           style:XeniaIOSStatusToastStyleError];
        }
      }]);
  cell.accessoryView = Accessory(@[ decline, join ]);
  cell.accessibilityLabel = [NSString stringWithFormat:@"%@, %@", invite.gamertag, secondary];
}

- (void)configureRequestCell:(UITableViewCell*)cell peer:(XeniaLiveFriend*)peer {
  cell.contentConfiguration =
      RowContent(peer.gamertag,
                 NSLocalizedString(@"Wants to be friends", @"Friends: incoming request row"),
                 Avatar(peer.gamertag, YES));
  const uint64_t xuid = peer.xuid;
  UIButton* decline = RowButton(
      nil, @"xmark", NO,
      [NSString stringWithFormat:NSLocalizedString(@"Decline friend request from %@",
                                                   @"VoiceOver; %@ is the gamertag"),
                                 peer.gamertag],
      [UIAction actionWithHandler:^(UIAction* __unused a) {
        [[XeniaLiveSocial shared] declineFriendRequest:xuid];
      }]);
  UIButton* accept = RowButton(
      NSLocalizedString(@"Accept", @"Friends: accept a friend request"), nil, YES,
      [NSString stringWithFormat:NSLocalizedString(@"Accept friend request from %@",
                                                   @"VoiceOver; %@ is the gamertag"),
                                 peer.gamertag],
      [UIAction actionWithHandler:^(UIAction* __unused a) {
        [[XeniaLiveSocial shared] acceptFriendRequest:xuid];
      }]);
  cell.accessoryView = Accessory(@[ decline, accept ]);
}

- (void)configureSentCell:(UITableViewCell*)cell peer:(XeniaLiveFriend*)peer {
  cell.contentConfiguration = RowContent(
      peer.gamertag, NSLocalizedString(@"Waiting for them to accept", @"Friends: sent request row"),
      Avatar(peer.gamertag, NO));
  const uint64_t xuid = peer.xuid;
  cell.accessoryView = Accessory(@[ RowButton(
      NSLocalizedString(@"Cancel", @"Friends: cancel a sent request"), nil, NO,
      [NSString stringWithFormat:NSLocalizedString(@"Cancel friend request to %@",
                                                   @"VoiceOver; %@ is the gamertag"),
                                 peer.gamertag],
      [UIAction actionWithHandler:^(UIAction* __unused a) {
        [[XeniaLiveSocial shared] removeFriend:xuid];
      }]) ]);
}

- (void)configureFriendCell:(UITableViewCell*)cell peer:(XeniaLiveFriend*)peer {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  NSString* presence = [social presenceTextForFriend:peer];
  UIListContentConfiguration* content =
      RowContent(peer.gamertag, nil, Avatar(peer.gamertag, peer.online));
  content.secondaryAttributedText = DotText(presence, PresenceDot(peer));
  cell.contentConfiguration = content;
  cell.accessibilityLabel = [NSString stringWithFormat:@"%@, %@", peer.gamertag, presence];
  NSMutableArray<UIView*>* accessory = [NSMutableArray array];
  if (peer.titleId) {
    [accessory addObject:CoverView(peer.titleId)];
    [self loadCoverFor:peer.titleId];
  }
  __unsafe_unretained XeniaIOSFriendsViewController* unsafe_self = self;
  XeniaLiveFriend* held = [[peer retain] autorelease];
  if (mode_ == XeniaIOSFriendsModeInvite) {
    const BOOL sent = [invited_ containsObject:@(peer.xuid)];
    UIButton* invite = RowButton(
        sent ? NSLocalizedString(@"Invited", @"Friends: a friend was invited")
             : NSLocalizedString(@"Invite", @"Friends: invite a friend to this game"),
        sent ? @"checkmark" : nil, !sent,
        [NSString stringWithFormat:NSLocalizedString(@"Invite %@ to this game",
                                                     @"VoiceOver; %@ is the gamertag"),
                                   peer.gamertag],
        [UIAction actionWithHandler:^(UIAction* __unused a) {
          [unsafe_self inviteFriend:held];
        }]);
    invite.enabled = !sent;
    [accessory addObject:invite];
  } else if (peer.joinable) {
    [accessory addObject:RowButton(
                             NSLocalizedString(@"Join", @"Friends: join a friend's game"), nil, YES,
                             [NSString stringWithFormat:NSLocalizedString(
                                                            @"Join %@'s game",
                                                            @"VoiceOver; %@ is the gamertag"),
                                                        peer.gamertag],
                             [UIAction actionWithHandler:^(UIAction* __unused a) {
                               [unsafe_self joinFriend:held];
                             }])];
  }
  if (accessory.count) {
    cell.accessoryView = Accessory(accessory);
  }
}

// An action row in the accent: Create Party, Invite to Party, Leave Party.
- (void)configureActionCell:(UITableViewCell*)cell
                      title:(NSString*)title
                     symbol:(NSString*)symbol
                    enabled:(BOOL)enabled
                destructive:(BOOL)destructive {
  UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
  content.text = title;
  content.image = [UIImage systemImageNamed:symbol];
  UIColor* color = !enabled      ? [XeniaTheme textMuted]
                   : destructive ? [XeniaTheme statusError]
                                 : [XeniaTheme accent];
  content.textProperties.color = color;
  content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  content.imageProperties.tintColor = color;
  cell.contentConfiguration = content;
  cell.selectionStyle =
      enabled ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
  cell.accessibilityTraits =
      UIAccessibilityTraitButton | (enabled ? UIAccessibilityTraitNone : UIAccessibilityTraitNotEnabled);
}

- (void)configurePartyCell:(UITableViewCell*)cell row:(NSInteger)table_row {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  const BOOL actions = social.partyActionsAvailable;
  const NSInteger row = [self partyLayoutRow:table_row];
  if (row < 0) {
    NSString* leader = nil;
    for (XeniaLivePartyMember* member in party_) {
      if (member.leader) {
        leader = member.gamertag;
      }
    }
    NSString* detail = [NSString
        localizedStringWithFormat:NSLocalizedString(@"%ld players", @"Party: how many are in it"),
                                  (long)party_.count];
    if (leader.length) {
      detail = [NSString stringWithFormat:NSLocalizedString(@"%@ · %@ leads",
                                                            @"Party summary; first %@ is the "
                                                            @"player count, second the leader"),
                                          detail, leader];
    }
    UIListContentConfiguration* content = RowContent(
        NSLocalizedString(@"Party", @"Party: summary row"), detail,
        [UIImage systemImageNamed:@"person.3.fill"]);
    content.imageProperties.tintColor = [XeniaTheme accent];
    cell.contentConfiguration = content;
    UIImageView* chevron = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:party_expanded_ ? @"chevron.up" : @"chevron.down"]]
        autorelease];
    chevron.tintColor = [XeniaTheme textMuted];
    chevron.isAccessibilityElement = NO;
    [chevron sizeToFit];
    cell.accessoryView = chevron;
    cell.accessibilityLabel = [NSString stringWithFormat:@"%@, %@",
                                                         NSLocalizedString(@"Party", @"Party: summary row"),
                                                         detail];
    cell.accessibilityValue = party_expanded_ ? NSLocalizedString(@"Expanded", @"VoiceOver")
                                              : NSLocalizedString(@"Collapsed", @"VoiceOver");
    cell.accessibilityTraits = UIAccessibilityTraitButton;
    return;
  }
  if ((NSUInteger)row < party_.count) {
    XeniaLivePartyMember* member = party_[(NSUInteger)row];
    NSString* role = member.leader ? NSLocalizedString(@"Leader", @"Party: the party's leader")
                                   : NSLocalizedString(@"In the party", @"Party: a member");
    if (member.me) {
      role = [NSString stringWithFormat:NSLocalizedString(@"%@ · You",
                                                          @"Party: this profile's row; %@ is "
                                                          @"Leader or In the party"),
                                        role];
    }
    UIListContentConfiguration* content =
        RowContent(member.gamertag, nil, Avatar(member.gamertag, YES));
    content.secondaryAttributedText = DotText(role, [XeniaTheme accent]);
    cell.contentConfiguration = content;
    // Room on the right for the voice chat mic indicator (later); the leader's crown for now.
    if (member.leader) {
      UIImageView* crown = [[[UIImageView alloc]
          initWithImage:[UIImage systemImageNamed:@"crown.fill"]] autorelease];
      crown.tintColor = [XeniaTheme statusWarning];
      crown.isAccessibilityElement = NO;
      cell.accessoryView = crown;
    }
    cell.accessibilityLabel = [NSString stringWithFormat:@"%@, %@", member.gamertag, role];
    return;
  }
  if (!party_.count) {
    [self configureActionCell:cell
                        title:NSLocalizedString(@"Create Party", @"Party action")
                       symbol:@"person.3"
                      enabled:actions
                  destructive:NO];
  } else if ((NSUInteger)row == party_.count) {
    [self configureActionCell:cell
                        title:NSLocalizedString(@"Invite to Party", @"Party action")
                       symbol:@"person.badge.plus"
                      enabled:actions && online_.count + in_game_.count > 0
                  destructive:NO];
  } else {
    [self configureActionCell:cell
                        title:NSLocalizedString(@"Leave Party", @"Party action")
                       symbol:@"rectangle.portrait.and.arrow.right"
                      enabled:actions
                  destructive:YES];
  }
}

- (void)configureGamesCell:(UITableViewCell*)cell {
  const NSInteger players = xe_ios_system_link_online_total();
  NSString* secondary =
      !xe_ios_system_link_online_connected()
          ? NSLocalizedString(@"Not connected", @"Online: System Link row while the lobby "
                                                @"doesn't answer")
      : players > 0 ? [NSString localizedStringWithFormat:
                                   NSLocalizedString(@"%ld in public lobbies",
                                                     @"Online: System Link row; %ld is how many "
                                                     @"players are in games' public lobbies"),
                                   long(players)]
                    : NSLocalizedString(@"Games people are playing", @"Online: System Link row");
  UIListContentConfiguration* content =
      RowContent(NSLocalizedString(@"Games Online", @"Online: System Link row title"), nil,
                 [UIImage systemImageNamed:@"gamecontroller"]);
  content.secondaryAttributedText =
      DotText(secondary, xe_ios_system_link_online_connected() ? [XeniaTheme accent]
                                                               : [XeniaTheme textMuted]);
  content.imageProperties.tintColor = [XeniaTheme accent];
  content.imageProperties.reservedLayoutSize = CGSizeMake(kAvatarSize, kAvatarSize);
  cell.contentConfiguration = content;
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  cell.accessibilityLabel = [NSString
      stringWithFormat:@"%@, %@", NSLocalizedString(@"Games Online", @"Online: System Link row title"),
                       secondary];
}

- (void)loadCoverFor:(uint32_t)title_id {
  if (!title_id || xe_cached_game_art_thumbnail(title_id) ||
      [covers_loading_ containsObject:@(title_id)]) {
    return;
  }
  [covers_loading_ addObject:@(title_id)];
  __unsafe_unretained XeniaIOSFriendsViewController* unsafe_self = self;
  [self retain];
  xe_load_game_art_thumbnail(title_id, 120.0, ^(UIImage* image) {
    if (image) {
      [unsafe_self.tableView reloadData];
      [unsafe_self release];
      return;
    }
    // Not on disk yet: fetch it (only with Online Features on), then look again.
    xe_fetch_game_art(title_id, ^(UIImage* fetched) {
      if (fetched) {
        xe_load_game_art_thumbnail(title_id, 120.0, ^(UIImage* __unused thumbnail) {
          [unsafe_self.tableView reloadData];
          [unsafe_self release];
        });
      } else {
        [unsafe_self release];
      }
    });
  });
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  const FriendsSection kind = [self sectionAt:indexPath.section];
  if (kind == kSectionGames) {
    UIViewController* games = games_online_provider_ ? games_online_provider_() : nil;
    if (games) {
      [self.navigationController pushViewController:games animated:YES];
    }
    return;
  }
  if (kind == kSectionNoFriends) {
    [self addTapped:nil];
    return;
  }
  if (kind == kSectionParty) {
    const NSInteger row = [self partyLayoutRow:indexPath.row];
    if (row < 0) {
      party_expanded_ = !party_expanded_;
      [tableView reloadSections:[NSIndexSet indexSetWithIndex:(NSUInteger)indexPath.section]
               withRowAnimation:UITableViewRowAnimationAutomatic];
    } else if ((NSUInteger)row >= party_.count) {
      [self partyActionAtRow:row];
    }
    return;
  }
  if (kind == kSectionStatus && indexPath.row == 1) {
    if (![[XeniaLiveSocial shared] turnOnOnlineForProfile]) {
      [self showToast:NSLocalizedString(@"Couldn't turn on online play for this profile.",
                                        @"Friends toast")
                style:XeniaIOSStatusToastStyleError];
    }
    [self reload];
  }
}

- (UISwipeActionsConfiguration*)tableView:(UITableView*)__unused tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  const FriendsSection kind = [self sectionAt:indexPath.section];
  if (kind != kSectionInGame && kind != kSectionOnline && kind != kSectionOffline) {
    return nil;
  }
  XeniaLiveFriend* peer = [self itemsIn:kind][(NSUInteger)indexPath.row];
  __unsafe_unretained XeniaIOSFriendsViewController* unsafe_self = self;
  UIContextualAction* remove = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:NSLocalizedString(@"Remove", @"Friends: remove a friend")
                        handler:^(UIContextualAction* __unused action, UIView* __unused view,
                                  void (^done)(BOOL)) {
                          [unsafe_self confirmRemove:peer];
                          done(YES);
                        }];
  return [UISwipeActionsConfiguration configurationWithActions:@[ remove ]];
}

- (UIContextMenuConfiguration*)tableView:(UITableView*)__unused tableView
    contextMenuConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath
                                        point:(CGPoint)__unused point {
  const FriendsSection kind = [self sectionAt:indexPath.section];
  if (kind != kSectionInGame && kind != kSectionOnline && kind != kSectionOffline) {
    return nil;
  }
  XeniaLiveFriend* peer = [[[self itemsIn:kind][(NSUInteger)indexPath.row] retain] autorelease];
  __unsafe_unretained XeniaIOSFriendsViewController* unsafe_self = self;
  return [UIContextMenuConfiguration
      configurationWithIdentifier:nil
                  previewProvider:nil
                   actionProvider:^UIMenu*(NSArray<UIMenuElement*>* __unused suggested) {
                     UIAction* remove = [UIAction
                         actionWithTitle:NSLocalizedString(@"Remove Friend",
                                                           @"Friends menu item")
                                   image:[UIImage systemImageNamed:@"person.badge.minus"]
                              identifier:nil
                                 handler:^(UIAction* __unused a) {
                                   [unsafe_self confirmRemove:peer];
                                 }];
                     remove.attributes = UIMenuElementAttributesDestructive;
                     return [UIMenu menuWithChildren:@[ remove ]];
                   }];
}

- (void)confirmRemove:(XeniaLiveFriend*)peer {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:[NSString stringWithFormat:NSLocalizedString(
                                                              @"Remove %@?",
                                                              @"Alert title; %@ is the gamertag"),
                                                          peer.gamertag]
                       message:NSLocalizedString(@"You'll stop seeing what they play, and you "
                                                 @"can't invite each other until one of you "
                                                 @"adds the other again.",
                                                 @"Alert message: removing a friend")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  const uint64_t xuid = peer.xuid;
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Remove",
                                                                    @"Alert button: remove a friend")
                                            style:UIAlertActionStyleDestructive
                                          handler:^(UIAlertAction* __unused a) {
                                            [[XeniaLiveSocial shared] removeFriend:xuid];
                                          }]];
  [self presentViewController:alert animated:YES completion:nil];
}

#pragma mark Actions

static const NSUInteger kPartyFoldsAbove = 3;

- (NSInteger)partySummaryRows {
  return party_.count > kPartyFoldsAbove ? 1 : 0;
}

- (NSInteger)partyMemberRows {
  return [self partySummaryRows] && !party_expanded_ ? 0 : (NSInteger)party_.count;
}

// The table row in the party section as the original layout's row: members first (index below
// the member count), then the actions. -1 for the summary row.
- (NSInteger)partyLayoutRow:(NSInteger)row {
  const NSInteger summary = [self partySummaryRows];
  if (summary && row == 0) {
    return -1;
  }
  const NSInteger r = row - summary;
  const NSInteger members = [self partyMemberRows];
  return r < members ? r : (NSInteger)party_.count + (r - members);
}

- (void)partyActionAtRow:(NSInteger)row {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  if (!social.partyActionsAvailable) {
    return;
  }
  if (!party_.count) {
    if (![social createParty]) {
      [self showToast:NSLocalizedString(@"Couldn't create a party.", @"Party toast")
                style:XeniaIOSStatusToastStyleError];
    }
    return;
  }
  if ((NSUInteger)row > party_.count) {
    [social leaveParty];
    return;
  }
  // Invite to Party: pick an online friend.
  UIAlertController* picker = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Invite to Party", @"Party action")
                       message:nil
                preferredStyle:UIAlertControllerStyleActionSheet];
  __unsafe_unretained XeniaIOSFriendsViewController* unsafe_self = self;
  for (XeniaLiveFriend* peer in [in_game_ arrayByAddingObjectsFromArray:online_]) {
    XeniaLiveFriend* held = [[peer retain] autorelease];
    [picker addAction:[UIAlertAction actionWithTitle:peer.gamertag
                                               style:UIAlertActionStyleDefault
                                             handler:^(UIAlertAction* __unused a) {
                                               if ([[XeniaLiveSocial shared]
                                                       inviteFriendToParty:held]) {
                                                 [unsafe_self
                                                     showToast:[NSString
                                                                   stringWithFormat:
                                                                       NSLocalizedString(
                                                                           @"Invited %@.",
                                                                           @"Friends toast; %@ "
                                                                           @"is the gamertag"),
                                                                       held.gamertag]
                                                         style:XeniaIOSStatusToastStyleSuccess];
                                               }
                                             }]];
  }
  [picker addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button")
                                             style:UIAlertActionStyleCancel
                                           handler:nil]];
  NSIndexPath* path =
      [NSIndexPath indexPathForRow:row
                         inSection:(NSInteger)[sections_ indexOfObject:@(kSectionParty)]];
  picker.popoverPresentationController.sourceView = [self.tableView cellForRowAtIndexPath:path] ?: self.view;
  [self presentViewController:picker animated:YES completion:nil];
}

- (void)acceptInvite:(XeniaLiveInvite*)invite {
  void (^handler)(XeniaLiveInvite*) = [[accept_invite_handler_ copy] autorelease];
  XeniaLiveInvite* held = [[invite retain] autorelease];
  [self.presentingViewController dismissViewControllerAnimated:YES
                                                    completion:^{
                                                      if (handler) handler(held);
                                                    }];
}

- (void)joinFriend:(XeniaLiveFriend*)peer {
  void (^handler)(XeniaLiveFriend*) = [[join_friend_handler_ copy] autorelease];
  XeniaLiveFriend* held = [[peer retain] autorelease];
  [self.presentingViewController dismissViewControllerAnimated:YES
                                                    completion:^{
                                                      if (handler) handler(held);
                                                    }];
}

- (void)inviteFriend:(XeniaLiveFriend*)peer {
  if (![[XeniaLiveSocial shared] inviteFriendToRunningGame:peer]) {
    [self showToast:NSLocalizedString(@"Start a game to invite friends to it.", @"Friends toast")
              style:XeniaIOSStatusToastStyleWarning];
    return;
  }
  [invited_ addObject:@(peer.xuid)];
  [self showToast:[NSString stringWithFormat:NSLocalizedString(@"Invited %@.",
                                                               @"Friends toast; %@ is the gamertag"),
                                             peer.gamertag]
            style:XeniaIOSStatusToastStyleSuccess];
  [self reload];
}

@end

#pragma mark - Add Friend

@interface XeniaIOSAddFriendViewController () <UITextFieldDelegate>
@end

@implementation XeniaIOSAddFriendViewController {
  UITextField* field_;
  UIButton* send_button_;  // Owned by the commit helper.
  XeniaIOSStatusToastPresenter* toast_;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  field_.delegate = nil;
  [field_ release];
  [toast_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Add Friend", @"Add Friend screen title");
  toast_ = [[XeniaIOSStatusToastPresenter alloc] init];
  send_button_ = XESetFormCommit(
      self, NSLocalizedString(@"Send Request", @"Add Friend: send the request"), self,
      @selector(sendTapped:));

  field_ = [[UITextField alloc] init];
  field_.placeholder = NSLocalizedString(@"Friend Code", @"Add Friend: code field placeholder");
  field_.font = xe_scaled_monospaced_font(UIFontTextStyleTitle3, 20.0, UIFontWeightSemibold);
  field_.adjustsFontForContentSizeCategory = YES;
  field_.textColor = [XeniaTheme textPrimary];
  field_.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters;
  field_.autocorrectionType = UITextAutocorrectionTypeNo;
  field_.spellCheckingType = UITextSpellCheckingTypeNo;
  field_.smartInsertDeleteType = UITextSmartInsertDeleteTypeNo;
  field_.returnKeyType = UIReturnKeySend;
  field_.clearButtonMode = UITextFieldViewModeWhileEditing;
  field_.delegate = self;
  field_.accessibilityLabel = NSLocalizedString(@"Friend code", @"VoiceOver: the code field");
  field_.accessibilityHint =
      NSLocalizedString(@"Eight letters and digits.", @"VoiceOver hint: the friend code field");
  [field_ addTarget:self
                action:@selector(codeChanged:)
      forControlEvents:UIControlEventEditingChanged];

  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(socialChanged:)
                                               name:XeniaLiveSocialDidChangeNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(requestFailed:)
                                               name:XeniaLiveSocialRequestFailedNotification
                                             object:nil];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [field_ becomeFirstResponder];
}

- (void)socialChanged:(NSNotification*)__unused note {
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:1]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (void)requestFailed:(NSNotification*)note {
  [toast_ presentMessage:note.userInfo[XeniaLiveSocialMessageKey]
                   style:XeniaIOSStatusToastStyleWarning
                  inView:self.navigationController.view ?: self.view];
}

- (void)codeChanged:(UITextField*)__unused field {
  send_button_.enabled = xe_ios_normalize_friend_code(field_.text) != nil;
}

- (BOOL)textFieldShouldReturn:(UITextField*)__unused textField {
  [self sendTapped:nil];
  return NO;
}

- (void)sendTapped:(id)__unused sender {
  NSString* code = xe_ios_normalize_friend_code(field_.text);
  if (!code) {
    [toast_ presentMessage:NSLocalizedString(@"A friend code has 8 letters and digits.",
                                             @"Add Friend toast: the code is malformed")
                     style:XeniaIOSStatusToastStyleWarning
                    inView:self.navigationController.view ?: self.view];
    return;
  }
  if ([code isEqualToString:[XeniaLiveSocial shared].friendCode]) {
    [toast_ presentMessage:NSLocalizedString(@"That's your own friend code.",
                                             @"Add Friend toast")
                     style:XeniaIOSStatusToastStyleWarning
                    inView:self.navigationController.view ?: self.view];
    return;
  }
  [[XeniaLiveSocial shared] addFriendWithCode:code];
  field_.text = nil;
  send_button_.enabled = NO;
  [toast_ presentMessage:NSLocalizedString(@"Friend request sent.", @"Add Friend toast")
                   style:XeniaIOSStatusToastStyleSuccess
                  inView:self.navigationController.view ?: self.view];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)__unused tableView {
  return 2;
}

- (NSInteger)tableView:(UITableView*)__unused tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? 1 : 3;
}

- (NSString*)tableView:(UITableView*)__unused tableView titleForHeaderInSection:(NSInteger)section {
  return section == 0 ? NSLocalizedString(@"Their Code", @"Add Friend section header")
                      : NSLocalizedString(@"Your Friend Code", @"Add Friend section header");
}

- (NSString*)tableView:(UITableView*)__unused tableView titleForFooterInSection:(NSInteger)section {
  return section == 0
             ? NSLocalizedString(@"Your friend finds their code in XeniOS: Online in the "
                                 @"library, then Add Friend.",
                                 @"Add Friend footer")
             : NSLocalizedString(@"Share it so friends can add you. Anyone with it can send you "
                                 @"a friend request.",
                                 @"Add Friend footer under the user's own code");
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  if (indexPath.section == 0) {
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    field_.translatesAutoresizingMaskIntoConstraints = NO;
    [cell.contentView addSubview:field_];
    UILayoutGuide* margins = cell.contentView.layoutMarginsGuide;
    [NSLayoutConstraint activateConstraints:@[
      [field_.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
      [field_.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
      [field_.topAnchor constraintEqualToAnchor:margins.topAnchor constant:4.0],
      [field_.bottomAnchor constraintEqualToAnchor:margins.bottomAnchor constant:-4.0],
      [field_.heightAnchor constraintGreaterThanOrEqualToConstant:36.0],
    ]];
    return cell;
  }
  NSString* code = [XeniaLiveSocial shared].friendCode;
  if (indexPath.row == 0) {
    UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
    content.text = code ? xe_ios_format_friend_code(code)
                        : NSLocalizedString(@"Not online yet", @"Add Friend: no code yet");
    content.textProperties.font =
        code ? xe_scaled_monospaced_font(UIFontTextStyleTitle2, 22.0, UIFontWeightSemibold)
             : [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    content.textProperties.color = code ? [XeniaTheme textPrimary] : [XeniaTheme textSecondary];
    content.textProperties.alignment = UIListContentTextAlignmentCenter;
    cell.contentConfiguration = content;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.accessibilityLabel = code ? [NSString stringWithFormat:NSLocalizedString(
                                                                   @"Your friend code: %@",
                                                                   @"VoiceOver; %@ is the code"),
                                                               [self spokenCode:code]]
                                   : content.text;
    return cell;
  }
  UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
  content.text = indexPath.row == 1 ? NSLocalizedString(@"Copy Code", @"Add Friend row")
                                    : NSLocalizedString(@"Share Code…", @"Add Friend row");
  content.image =
      [UIImage systemImageNamed:indexPath.row == 1 ? @"doc.on.doc" : @"square.and.arrow.up"];
  content.imageProperties.tintColor = [XeniaTheme accent];
  content.textProperties.color = code ? [XeniaTheme textPrimary] : [XeniaTheme textMuted];
  cell.contentConfiguration = content;
  cell.userInteractionEnabled = code != nil;
  cell.accessibilityTraits = UIAccessibilityTraitButton |
                             (code ? UIAccessibilityTraitNone : UIAccessibilityTraitNotEnabled);
  return cell;
}

// Letters one by one, so VoiceOver doesn't read the code as a word.
- (NSString*)spokenCode:(NSString*)code {
  NSMutableArray* letters = [NSMutableArray array];
  for (NSUInteger i = 0; i < code.length; ++i) {
    [letters addObject:[code substringWithRange:NSMakeRange(i, 1)]];
  }
  return [letters componentsJoinedByString:@" "];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  NSString* code = [XeniaLiveSocial shared].friendCode;
  if (indexPath.section != 1 || !code) {
    return;
  }
  if (indexPath.row == 1) {
    [UIPasteboard generalPasteboard].string = code;
    [toast_ presentMessage:NSLocalizedString(@"Friend code copied.", @"Add Friend toast")
                     style:XeniaIOSStatusToastStyleSuccess
                    inView:self.navigationController.view ?: self.view];
  } else if (indexPath.row == 2) {
    NSString* text = [NSString
        stringWithFormat:NSLocalizedString(@"Add me as a friend in XeniOS: %@",
                                           @"Shared text; %@ is the user's friend code"),
                         xe_ios_format_friend_code(code)];
    UIActivityViewController* share =
        [[[UIActivityViewController alloc]
            initWithActivityItems:@[ text, [NSURL URLWithString:xe_ios_friend_link(code)] ]
            applicationActivities:nil] autorelease];
    share.popoverPresentationController.sourceView = tableView;
    share.popoverPresentationController.sourceRect = [tableView rectForRowAtIndexPath:indexPath];
    [self presentViewController:share animated:YES completion:nil];
  }
}

@end
