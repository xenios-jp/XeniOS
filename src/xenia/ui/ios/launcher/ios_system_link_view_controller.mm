/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_system_link_view_controller.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

DECLARE_string(systemlink_transport);

namespace {

enum class Row { kTurnOnOnline, kLobby, kPrivate, kCreate, kJoin, kShare, kOnline, kNobodyOnline };

// Games listed under Playing Online Now, at most.
constexpr size_t kMaxOnlineGames = 20;

NSString* PlayersText(NSInteger players) {
  return players == 1
             ? NSLocalizedString(@"1 player",
                                 @"system link view controller: players in a game's public lobby")
             : [NSString localizedStringWithFormat:NSLocalizedString(
                                                       @"%ld players",
                                                       @"system link view controller: players "
                                                       @"in a game's public lobby"),
                                                   long(players)];
}

// Online Features is one switch for everything XeniOS sends, so turning it on from here asks
// first and says what else it starts.
void ConfirmTurnOnOnlineFeatures(UIViewController* presenter, void (^turn_on)(void)) {
  NSString* message =
      NSLocalizedString(@"This turns on all Online Features, not only System Link: update checks, "
                        @"compatibility data, cover art, compatibility discussions and reports, "
                        @"and System Link over the internet, where players can see your IP "
                        @"address and a profile ID.",
                        @"system link view controller: alert message");
  if (xe_ios_diagnostics_available() && xe_ios_share_diagnostics_enabled()) {
    message = [message
        stringByAppendingFormat:@" %@",
                                NSLocalizedString(@"Share Diagnostics is on, so anonymous crash "
                                                  @"reports and performance statistics are sent "
                                                  @"too.",
                                                  @"system link view controller: alert message")];
  }
  message = [message
      stringByAppendingFormat:@"\n\n%@", NSLocalizedString(@"You can change this in Settings > "
                                                            @"About > Privacy.",
                                                            @"system link view controller: alert "
                                                            @"message")];
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Turn On Online Features?",
                                                 @"system link view controller: alert title")
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel",
                                                         @"system link view controller: button")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Turn On",
                                                         @"system link view controller: button")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 turn_on();
                               }]];
  [presenter presentViewController:alert animated:YES completion:nil];
}

// Game names by title ID in hex, from the compatibility data.
NSDictionary<NSString*, NSString*>* LoadGameNames() {
  NSMutableDictionary<NSString*, NSString*>* names = [NSMutableDictionary dictionary];
  NSDictionary* compat = xe_load_cached_compat_data();
  for (id key in compat) {
    NSString* name = xe_string_from_object(xe_dictionary_from_object(compat[key])[@"title"]);
    if ([key isKindOfClass:[NSString class]] && name.length) {
      names[[key uppercaseString]] = name;
    }
  }
  return names;
}

NSString* UnnamedGame(uint32_t title_id) {
  return [NSString
      stringWithFormat:NSLocalizedString(@"Game %@", @"system link view controller: a game with "
                                                     @"no name; %@ is its title ID"),
                       XEFormatTitleIDHexUpper(title_id)];
}

// Busiest first, then by title ID.
std::vector<std::pair<uint32_t, NSInteger>> SortedOnline() {
  std::vector<std::pair<uint32_t, NSInteger>> online;
  NSDictionary<NSNumber*, NSNumber*>* titles = xe_ios_system_link_online_titles();
  for (NSNumber* title_id in titles) {
    online.emplace_back(title_id.unsignedIntValue, titles[title_id].integerValue);
  }
  std::sort(online.begin(), online.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  return online;
}

}  // namespace

@implementation XeniaIOSSystemLinkViewController {
  NSString* game_title_;
  uint32_t title_id_;
  void (^play_handler_)(void);
  std::vector<std::vector<Row>> sections_;
  // Title ID and players for each kOnline row, busiest first.
  std::vector<std::pair<uint32_t, NSInteger>> online_;
  // Game names by title ID in hex, from the compatibility data.
  NSDictionary<NSString*, NSString*>* names_;
}

@synthesize playHandler = play_handler_;

- (instancetype)initWithGameTitle:(NSString*)gameTitle titleID:(uint32_t)titleID {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    game_title_ = [gameTitle copy];
    title_id_ = titleID;
    self.title = NSLocalizedString(@"System Link", @"system link view controller: title");
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [names_ release];
  [game_title_ release];
  [play_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  if (game_title_.length) {
    self.navigationItem.prompt = game_title_;
  }
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                               target:self
             action:@selector(doneTapped:)] autorelease];
  if (play_handler_) {
    self.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc]
        initWithTitle:NSLocalizedString(@"Play", @"system link view controller: button")
                style:UIBarButtonItemStyleProminent
               target:self
               action:@selector(playTapped:)] autorelease];
  }
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onlineChanged:)
                                               name:XeniaIOSSystemLinkOnlineDidChangeNotification
                                             object:nil];
  [self reloadRows];
  xe_ios_system_link_refresh_online();
}

- (void)onlineChanged:(NSNotification* __unused)note {
  [self reloadRows];
}

// The name of a game someone else is playing.
- (NSString*)nameForTitleID:(uint32_t)title_id {
  if (title_id == title_id_ && game_title_.length) {
    return game_title_;
  }
  if (!names_) {
    names_ = [LoadGameNames() copy];
  }
  return names_[XEFormatTitleIDHexUpper(title_id)] ?: UnnamedGame(title_id);
}

// How many players are in this game's public lobby, or nil while that isn't known.
- (NSString*)lobbyPlayersText {
  if (!xe_ios_system_link_online_titles()) {
    return nil;
  }
  const NSInteger players = xe_ios_system_link_online_players(title_id_);
  if (players == 0) {
    return NSLocalizedString(@"Nobody is there now.",
                             @"system link view controller: room row detail, after "
                             @"\"Everyone playing this game.\"");
  }
  if (players == 1) {
    return NSLocalizedString(@"One player is there now.",
                             @"system link view controller: room row detail, after "
                             @"\"Everyone playing this game.\"");
  }
  return [NSString
      localizedStringWithFormat:NSLocalizedString(@"%ld players are there now.",
                                                  @"system link view controller: room row "
                                                  @"detail, after \"Everyone playing this "
                                                  @"game.\"; %ld is how many"),
                                long(players)];
}

- (void)reloadRows {
  sections_.clear();
  if (!xe_ios_online_features_enabled()) {
    sections_.push_back({Row::kTurnOnOnline});
  }
  const BOOL has_room = xe_ios_system_link_room(title_id_) != nil;
  sections_.push_back(has_room ? std::vector<Row>{Row::kLobby, Row::kPrivate}
                               : std::vector<Row>{Row::kLobby});
  sections_.push_back(has_room ? std::vector<Row>{Row::kCreate, Row::kJoin, Row::kShare}
                               : std::vector<Row>{Row::kCreate, Row::kJoin});
  online_.clear();
  if (xe_ios_system_link_online_titles()) {
    online_ = SortedOnline();
    if (online_.size() > kMaxOnlineGames) {
      online_.resize(kMaxOnlineGames);
    }
    sections_.push_back(online_.empty() ? std::vector<Row>{Row::kNobodyOnline}
                                        : std::vector<Row>(online_.size(), Row::kOnline));
  }
  [self.tableView reloadData];
}

- (Row)rowAtIndexPath:(NSIndexPath*)indexPath {
  return sections_[size_t(indexPath.section)][size_t(indexPath.row)];
}

- (void)doneTapped:(id)__unused sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)playTapped:(id)__unused sender {
  void (^handler)(void) = [play_handler_ copy];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             handler();
                             [handler release];
                           }];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return NSInteger(sections_.size());
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return NSInteger(sections_[size_t(section)].size());
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  switch (sections_[size_t(section)].front()) {
    case Row::kLobby:
      return NSLocalizedString(@"Room", @"system link view controller: section header");
    case Row::kCreate:
      return NSLocalizedString(@"Private Room", @"system link view controller: section header");
    case Row::kOnline:
    case Row::kNobodyOnline:
      return NSLocalizedString(@"Playing Online Now",
                               @"system link view controller: section header");
    default:
      return nil;
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  switch (sections_[size_t(section)].front()) {
    case Row::kTurnOnOnline:
      return NSLocalizedString(@"System Link plays over the internet, so it needs Online "
                               @"features.",
                               @"system link view controller: footer");
    case Row::kLobby:
      if (!xe_ios_system_link_uses_relay()) {
        return [NSString
            stringWithFormat:NSLocalizedString(@"System Link is set to “%@” in "
                                               @"Settings, so these rooms aren't used.",
                                               @"system link view controller: footer; %@ is "
                                               @"the systemlink_transport setting"),
                             [NSString stringWithUTF8String:cvars::systemlink_transport.c_str()]];
      }
      return NSLocalizedString(
          @"Play with others running XeniOS, or Xenia-edge on a Mac. Players in a room find "
          @"each other automatically and connect directly where their networks allow, "
          @"otherwise through a relay. The in-game menu shows how the room is connected. "
          @"Players in your room can see your IP address and a profile ID.",
          @"system link view controller: footer");
    case Row::kCreate:
      return NSLocalizedString(@"Share the code with the players you want. Everyone needs this "
                               @"game, and players on another version of it may not see you.",
                               @"system link view controller: footer");
    case Row::kOnline:
    case Row::kNobodyOnline:
      return NSLocalizedString(@"Players in each game's public lobby, updated every minute. "
                               @"Private rooms aren't counted.",
                               @"system link view controller: footer");
    default:
      return nil;
  }
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaIOSSystemLinkCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor secondarySystemGroupedBackgroundColor];
    cell.detailTextLabel.numberOfLines = 2;
  }
  NSString* code = xe_ios_system_link_room(title_id_);
  NSString* title = nil;
  NSString* detail = nil;
  NSString* symbol = nil;
  BOOL checked = NO;
  BOOL button = YES;
  const Row row = [self rowAtIndexPath:indexPath];
  switch (row) {
    case Row::kTurnOnOnline:
      title =
          NSLocalizedString(@"Turn On Online Features", @"system link view controller: button row");
      symbol = @"network";
      break;
    case Row::kLobby:
      title = NSLocalizedString(@"Public Lobby", @"system link view controller: room row");
      detail = NSLocalizedString(@"Everyone playing this game.",
                                 @"system link view controller: room row detail");
      if (NSString* players = [self lobbyPlayersText]) {
        detail = [detail stringByAppendingFormat:@" %@", players];
      }
      symbol = @"globe";
      checked = code == nil;
      break;
    case Row::kPrivate:
      title = [NSString
          stringWithFormat:NSLocalizedString(@"Private Room %@",
                                             @"system link view controller: room row; %@ is "
                                             @"the room code"),
                           code];
      detail = NSLocalizedString(@"Only players with the code.",
                                 @"system link view controller: room row detail");
      symbol = @"lock";
      checked = YES;
      break;
    case Row::kCreate:
      title = NSLocalizedString(@"Create Private Room", @"system link view controller: button row");
      symbol = @"plus.circle";
      break;
    case Row::kJoin:
      title = NSLocalizedString(@"Join with Code…", @"system link view controller: button row");
      symbol = @"keyboard";
      break;
    case Row::kShare:
      title = NSLocalizedString(@"Share Invite…", @"system link view controller: button row");
      symbol = @"square.and.arrow.up";
      break;
    case Row::kOnline: {
      const auto& [title_id, players] = online_[size_t(indexPath.row)];
      title = [self nameForTitleID:title_id];
      detail = PlayersText(players);
      symbol = title_id == title_id_ ? @"person.2.fill" : @"person.2";
      button = NO;
      break;
    }
    case Row::kNobodyOnline:
      title = NSLocalizedString(@"Nobody is in a public lobby right now.",
                                @"system link view controller: row");
      symbol = @"person.2.slash";
      button = NO;
      break;
  }
  cell.textLabel.text = title;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text = detail;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.imageView.image = XESettingsBadgeImage(
      symbol, row == Row::kNobodyOnline ? [UIColor systemGrayColor] : [XeniaTheme accent]);
  cell.accessoryType = checked ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
  cell.selectionStyle =
      button ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
  cell.accessibilityTraits =
      (button ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText) |
      (checked ? UIAccessibilityTraitSelected : 0);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  switch ([self rowAtIndexPath:indexPath]) {
    case Row::kTurnOnOnline:
      ConfirmTurnOnOnlineFeatures(self, ^{
        xe_ios_set_online_features_enabled(YES);
        [self reloadRows];
      });
      break;
    case Row::kLobby:
      xe_ios_set_system_link_room(title_id_, nil);
      [self reloadRows];
      break;
    case Row::kPrivate:
      break;
    case Row::kCreate:
      xe_ios_set_system_link_room(title_id_, xe_ios_new_system_link_room_code());
      [self reloadRows];
      [self shareInviteFromView:[tableView cellForRowAtIndexPath:indexPath]];
      break;
    case Row::kJoin:
      [self presentJoinAlert];
      break;
    case Row::kShare:
      [self shareInviteFromView:[tableView cellForRowAtIndexPath:indexPath]];
      break;
    case Row::kOnline:
    case Row::kNobodyOnline:
      break;
  }
}

- (void)presentJoinAlert {
  __block UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Join a Private Room",
                                                 @"system link view controller: alert title")
                       message:NSLocalizedString(@"Enter the 6-character code you were sent.",
                                                 @"system link view controller: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
    field.placeholder = @"ABC123";
    field.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    field.spellCheckingType = UITextSpellCheckingTypeNo;
    field.font = [UIFont monospacedSystemFontOfSize:17.0 weight:UIFontWeightRegular];
  }];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel",
                                                         @"system link view controller: button")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  UIAlertAction* join = [UIAlertAction
      actionWithTitle:NSLocalizedString(@"Join", @"system link view controller: button")
                style:UIAlertActionStyleDefault
              handler:^(__unused UIAlertAction* action) {
                NSString* code =
                    xe_ios_normalize_system_link_room_code(alert.textFields.firstObject.text);
                if (!code) {
                  XEPresentOKAlert(
                      self,
                      NSLocalizedString(@"Not a Room Code",
                                        @"system link view controller: alert title"),
                      NSLocalizedString(@"Room codes have 6 letters and digits.",
                                        @"system link view controller: alert message"));
                  return;
                }
                xe_ios_set_system_link_room(title_id_, code);
                [self reloadRows];
              }];
  [alert addAction:join];
  alert.preferredAction = join;
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)shareInviteFromView:(UIView*)source {
  NSString* code = xe_ios_system_link_room(title_id_);
  if (!code) {
    return;
  }
  NSString* text = [NSString
      stringWithFormat:NSLocalizedString(@"Play %1$@ with me over System Link in XeniOS. Room "
                                         @"code: %2$@",
                                         @"System Link invite text; %1$@ is the game, %2$@ the "
                                         @"room code"),
                       game_title_.length ? game_title_ : @"", code];
  UIActivityViewController* share = [[[UIActivityViewController alloc]
      initWithActivityItems:@[ text, xe_ios_system_link_invite_url(title_id_, code) ]
      applicationActivities:nil] autorelease];
  share.popoverPresentationController.sourceView = source ?: self.view;
  share.popoverPresentationController.sourceRect = (source ?: self.view).bounds;
  [self presentViewController:share animated:YES completion:nil];
}

@end

void xe_ios_present_system_link_sheet(UIViewController* presenter, NSString* game_title,
                                      uint32_t title_id, void (^play)(void)) {
  XeniaIOSSystemLinkViewController* controller =
      [[XeniaIOSSystemLinkViewController alloc] initWithGameTitle:game_title titleID:title_id];
  controller.playHandler = play;
  XeniaLandscapeNavigationController* navigation =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:controller];
  XEConfigureDestinationPresentation(navigation, presenter.view, CGSizeMake(520.0, 620.0), NO);
  [presenter presentViewController:navigation animated:YES completion:nil];
  [navigation release];
  [controller release];
}

namespace {

enum class OnlineRow { kMine, kNoneMine, kOther };

// Library icons in the Online sheet, in points.
constexpr CGFloat kOnlineIconSize = 36.0;
// Box covers in the Online sheet's game rows, in points.
constexpr CGFloat kOnlineCoverWidth = 32.0;
constexpr CGFloat kOnlineCoverHeight = kOnlineCoverWidth * 300.0 / 219.0;

// "● 3 playing", the dot in the online green.
NSAttributedString* PlayingText(NSInteger players) {
  NSString* text =
      players == 1
          ? NSLocalizedString(@"1 playing", @"online view controller: players in a game's lobby")
          : [NSString localizedStringWithFormat:NSLocalizedString(
                                                    @"%ld playing",
                                                    @"online view controller: players in a "
                                                    @"game's lobby; %ld is how many"),
                                                long(players)];
  NSMutableAttributedString* out = [[[NSMutableAttributedString alloc]
      initWithString:@"● "
          attributes:@{NSForegroundColorAttributeName : [XeniaTheme accent]}] autorelease];
  [out appendAttributedString:[[[NSAttributedString alloc]
                                  initWithString:text
                                      attributes:@{
                                        NSForegroundColorAttributeName : [XeniaTheme textSecondary]
                                      }] autorelease]];
  return out;
}

// A section title as large as the sheet's other headings, with a count at the trailing edge.
UIView* NewOnlineSectionHeader(NSString* title, NSString* count) {
  UILabel* title_label = [[[UILabel alloc] init] autorelease];
  title_label.text = title;
  title_label.textColor = [XeniaTheme textPrimary];
  title_label.font = xe_scaled_system_font(UIFontTextStyleTitle2, 22.0, UIFontWeightBold);
  title_label.adjustsFontForContentSizeCategory = YES;
  title_label.numberOfLines = 0;
  title_label.accessibilityTraits = UIAccessibilityTraitHeader;
  UILabel* count_label = [[[UILabel alloc] init] autorelease];
  count_label.text = count;
  count_label.textColor = [XeniaTheme textSecondary];
  count_label.font = xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  count_label.adjustsFontForContentSizeCategory = YES;
  [count_label setContentHuggingPriority:UILayoutPriorityRequired
                                 forAxis:UILayoutConstraintAxisHorizontal];
  UIStackView* row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ title_label, count_label ]] autorelease];
  row.alignment = UIStackViewAlignmentFirstBaseline;
  row.spacing = 8.0;
  row.translatesAutoresizingMaskIntoConstraints = NO;
  UIView* header = [[[UIView alloc] init] autorelease];
  [header addSubview:row];
  [NSLayoutConstraint activateConstraints:@[
    // A little in from the group's edge, as drawn.
    [row.leadingAnchor constraintEqualToAnchor:header.layoutMarginsGuide.leadingAnchor
                                      constant:-12.0],
    [row.trailingAnchor constraintEqualToAnchor:header.layoutMarginsGuide.trailingAnchor],
    [row.topAnchor constraintEqualToAnchor:header.topAnchor constant:18.0],
    [row.bottomAnchor constraintEqualToAnchor:header.bottomAnchor constant:-8.0],
  ]];
  return header;
}

}  // namespace

// Picks one of the player's games, for a private room.
@interface XeniaIOSOnlineGamePickerViewController : XESheetTableViewController
- (instancetype)initWithGames:(NSArray<NSNumber*>*)games
                        names:(NSDictionary<NSNumber*, NSString*>*)names
                        icons:(NSDictionary<NSNumber*, UIImage*>*)icons
                       prompt:(NSString*)prompt
                         pick:(void (^)(uint32_t title_id, UIView* source))pick;
@end

@implementation XeniaIOSOnlineGamePickerViewController {
  NSArray<NSNumber*>* games_;
  NSDictionary<NSNumber*, NSString*>* names_;
  NSDictionary<NSNumber*, UIImage*>* icons_;
  NSString* prompt_;
  void (^pick_)(uint32_t title_id, UIView* source);
}

- (instancetype)initWithGames:(NSArray<NSNumber*>*)games
                        names:(NSDictionary<NSNumber*, NSString*>*)names
                        icons:(NSDictionary<NSNumber*, UIImage*>*)icons
                       prompt:(NSString*)prompt
                         pick:(void (^)(uint32_t title_id, UIView* source))pick {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    games_ = [games copy];
    names_ = [names copy];
    icons_ = [icons copy];
    prompt_ = [prompt copy];
    pick_ = [pick copy];
    self.title = NSLocalizedString(@"Choose a Game", @"online game picker: title");
  }
  return self;
}

- (void)dealloc {
  [games_ release];
  [names_ release];
  [icons_ release];
  [prompt_ release];
  [pick_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return NSInteger(games_.count);
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return prompt_;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaIOSOnlineGamePickerCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor secondarySystemGroupedBackgroundColor];
    cell.textLabel.numberOfLines = 0;
  }
  NSNumber* title_id = games_[size_t(indexPath.row)];
  cell.textLabel.text = names_[title_id];
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.imageView.image =
      icons_[title_id] ?: XESettingsBadgeImage(@"gamecontroller.fill", [XeniaTheme accent]);
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  pick_(games_[size_t(indexPath.row)].unsignedIntValue,
        [tableView cellForRowAtIndexPath:indexPath] ?: self.view);
}

@end

// Who's online: how many players are in each game's public lobby, the player's games first.
@interface XeniaIOSOnlineViewController : XESheetTableViewController
- (instancetype)initWithLibrary:(const std::vector<XeniaIOSOnlineLibraryGame>&)library
                       openGame:(void (^)(uint32_t title_id))openGame;
@end

@implementation XeniaIOSOnlineViewController {
  // The player's games by title ID: names, and icons as given and as shown.
  NSMutableDictionary<NSNumber*, NSString*>* library_names_;
  NSMutableDictionary<NSNumber*, NSData*>* library_icons_;
  NSMutableDictionary<NSNumber*, UIImage*>* icons_;
  void (^open_game_)(uint32_t title_id);
  std::vector<std::vector<OnlineRow>> sections_;
  // Title ID and players, busiest first: the player's games, and the others.
  std::vector<std::pair<uint32_t, NSInteger>> mine_;
  std::vector<std::pair<uint32_t, NSInteger>> others_;
  NSDictionary<NSString*, NSString*>* names_;
  // Covers being fetched, so each is asked for once.
  NSMutableSet<NSNumber*>* cover_fetches_;
  // The top of the sheet: who's online, and the room buttons.
  UIView* summary_;
  UILabel* summary_title_;
  UILabel* summary_detail_;
  UIStackView* summary_buttons_;
  UIButton* create_button_;
  UIButton* join_button_;
  UIButton* turn_on_button_;
}

- (instancetype)initWithLibrary:(const std::vector<XeniaIOSOnlineLibraryGame>&)library
                       openGame:(void (^)(uint32_t title_id))openGame {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    library_names_ = [[NSMutableDictionary alloc] init];
    library_icons_ = [[NSMutableDictionary alloc] init];
    icons_ = [[NSMutableDictionary alloc] init];
    for (const XeniaIOSOnlineLibraryGame& game : library) {
      if (!game.title_id || library_names_[@(game.title_id)]) {
        continue;
      }
      library_names_[@(game.title_id)] = game.name.length ? game.name : UnnamedGame(game.title_id);
      if (game.icon.length) {
        library_icons_[@(game.title_id)] = game.icon;
      }
    }
    open_game_ = [openGame copy];
    self.title = NSLocalizedString(@"Online", @"online view controller: title");
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [library_names_ release];
  [library_icons_ release];
  [icons_ release];
  [open_game_ release];
  [names_ release];
  [cover_fetches_ release];
  [summary_ release];
  [summary_title_ release];
  [summary_detail_ release];
  [summary_buttons_ release];
  [create_button_ release];
  [join_button_ release];
  [turn_on_button_ release];
  [super dealloc];
}

// A capsule for the summary: white for the main action, grey for the other.
- (UIButton*)newSummaryButtonWithTitle:(NSString*)title
                                symbol:(NSString*)symbol
                               primary:(BOOL)primary
                                action:(SEL)action {
  UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
  config.title = title;
  config.image = [UIImage systemImageNamed:symbol];
  config.imagePadding = 6.0;
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.baseBackgroundColor = primary ? [UIColor whiteColor] : [UIColor colorWithWhite:1.0
                                                                                  alpha:0.12];
  config.baseForegroundColor = primary ? [UIColor blackColor] : [XeniaTheme textPrimary];
  config.contentInsets = NSDirectionalEdgeInsetsMake(0.0, 16.0, 0.0, 16.0);
  config.preferredSymbolConfigurationForImage =
      [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleSubheadline];
  UIButton* button = [[UIButton buttonWithConfiguration:config primaryAction:nil] retain];
  xe_apply_button_title_font(button, UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
  button.titleLabel.adjustsFontSizeToFitWidth = YES;
  button.titleLabel.minimumScaleFactor = 0.8;
  [button.heightAnchor constraintGreaterThanOrEqualToConstant:44.0].active = YES;
  [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  return button;
}

- (void)buildSummary {
  summary_title_ = [[UILabel alloc] init];
  summary_title_.textColor = [XeniaTheme textPrimary];
  summary_title_.font = xe_scaled_system_font(UIFontTextStyleTitle1, 28.0, UIFontWeightBold);
  summary_title_.adjustsFontForContentSizeCategory = YES;
  summary_title_.numberOfLines = 0;
  summary_title_.accessibilityTraits = UIAccessibilityTraitHeader;
  summary_detail_ = [[UILabel alloc] init];
  summary_detail_.textColor = [XeniaTheme textSecondary];
  summary_detail_.font = xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0,
                                               UIFontWeightRegular);
  summary_detail_.adjustsFontForContentSizeCategory = YES;
  summary_detail_.numberOfLines = 0;
  create_button_ = [self newSummaryButtonWithTitle:NSLocalizedString(
                                                       @"Create Room",
                                                       @"online view controller: button")
                                            symbol:@"plus"
                                           primary:YES
                                            action:@selector(createTapped:)];
  join_button_ = [self newSummaryButtonWithTitle:NSLocalizedString(@"Join with Code",
                                                                   @"online view controller: "
                                                                   @"button")
                                          symbol:@"keyboard"
                                         primary:NO
                                          action:@selector(joinTapped:)];
  turn_on_button_ = [self newSummaryButtonWithTitle:NSLocalizedString(
                                                        @"Turn On Online Features",
                                                        @"online view controller: button")
                                             symbol:@"network"
                                            primary:YES
                                             action:@selector(turnOnTapped:)];
  summary_buttons_ = [[UIStackView alloc]
      initWithArrangedSubviews:@[ create_button_, join_button_, turn_on_button_ ]];
  summary_buttons_.spacing = 10.0;
  summary_buttons_.distribution = UIStackViewDistributionFillEqually;
  UIStackView* text = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ summary_title_, summary_detail_, summary_buttons_ ]] autorelease];
  text.axis = UILayoutConstraintAxisVertical;
  text.spacing = 4.0;
  [text setCustomSpacing:16.0 afterView:summary_detail_];
  text.translatesAutoresizingMaskIntoConstraints = NO;
  summary_ = [[UIView alloc] init];
  summary_.preservesSuperviewLayoutMargins = YES;
  [summary_ addSubview:text];
  [NSLayoutConstraint activateConstraints:@[
    [text.leadingAnchor constraintEqualToAnchor:summary_.layoutMarginsGuide.leadingAnchor],
    [text.trailingAnchor constraintEqualToAnchor:summary_.layoutMarginsGuide.trailingAnchor],
    [text.topAnchor constraintEqualToAnchor:summary_.topAnchor constant:8.0],
    [text.bottomAnchor constraintEqualToAnchor:summary_.bottomAnchor constant:-4.0],
  ]];
}

- (void)createTapped:(id)sender {
  if (!library_names_.count) {
    return;
  }
  [self pickGameWithPrompt:NSLocalizedString(@"Which game is the room for?",
                                             @"online game picker: header")
                      then:^(uint32_t title_id, UIView* source) {
                        NSString* code = xe_ios_new_system_link_room_code();
                        xe_ios_set_system_link_room(title_id, code);
                        [self shareRoom:code
                                titleID:title_id
                                   from:source ?: self.navigationController.navigationBar];
                      }];
}

- (void)joinTapped:(id)__unused sender {
  if (library_names_.count) {
    [self presentJoinAlert];
  }
}

- (void)turnOnTapped:(id)__unused sender {
  ConfirmTurnOnOnlineFeatures(self, ^{
    xe_ios_set_online_features_enabled(YES);
    [self reloadRows];
    xe_ios_system_link_refresh_online();
  });
}

- (void)playTapped:(UIButton*)sender {
  [self finishWithTitleID:uint32_t(sender.tag)];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.sectionHeaderTopPadding = 0.0;
  cover_fetches_ = [[NSMutableSet alloc] init];
  // The sheet bar holds the title, between ✕ and +.
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  [self buildSummary];
  // + holds the room actions, as in the mockup's sheet bar.
  __unsafe_unretained XeniaIOSOnlineViewController* unsafe_self = self;
  UIMenu* rooms = [UIMenu menuWithChildren:@[
    [UIAction actionWithTitle:NSLocalizedString(@"Create Room", @"online view controller: button")
                        image:[UIImage systemImageNamed:@"plus.circle"]
                   identifier:nil
                      handler:^(__unused UIAction* action) {
                        [unsafe_self createTapped:nil];
                      }],
    [UIAction actionWithTitle:NSLocalizedString(@"Join with Code",
                                                @"online view controller: button")
                        image:[UIImage systemImageNamed:@"keyboard"]
                   identifier:nil
                      handler:^(__unused UIAction* action) {
                        [unsafe_self joinTapped:nil];
                      }],
  ]];
  self.navigationItem.rightBarButtonItem =
      [[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"plus"]
                                         menu:rooms] autorelease];
  self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                               target:self
             action:@selector(doneTapped:)] autorelease];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onlineChanged:)
                                               name:XeniaIOSSystemLinkOnlineDidChangeNotification
                                             object:nil];
  [self reloadRows];
  xe_ios_system_link_refresh_online();
}

- (void)onlineChanged:(NSNotification* __unused)note {
  [self reloadRows];
}

- (void)doneTapped:(id)__unused sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)reloadRows {
  sections_.clear();
  mine_.clear();
  others_.clear();
  [self updateSummary];
  if (!xe_ios_online_features_enabled()) {
    [self.tableView reloadData];
    return;
  }
  if (xe_ios_system_link_uses_relay() && xe_ios_system_link_online_titles()) {
    for (const auto& game : SortedOnline()) {
      if (library_names_[@(game.first)]) {
        mine_.push_back(game);
      } else if (others_.size() < kMaxOnlineGames) {
        others_.push_back(game);
      }
    }
    // With nobody in a game the sheet is one centred message, not an empty group.
    if (!mine_.empty() || !others_.empty()) {
      sections_.push_back(mine_.empty() ? std::vector<OnlineRow>{OnlineRow::kNoneMine}
                                        : std::vector<OnlineRow>(mine_.size(), OnlineRow::kMine));
    }
    if (!others_.empty()) {
      sections_.push_back(std::vector<OnlineRow>(others_.size(), OnlineRow::kOther));
    }
  }
  [self.tableView reloadData];
}

// Who's online, as the sheet's large title and the line under it.
- (void)updateSummary {
  const BOOL on = xe_ios_online_features_enabled();
  const BOOL relay = on && xe_ios_system_link_uses_relay();
  create_button_.hidden = !relay;
  join_button_.hidden = !relay;
  create_button_.enabled = library_names_.count != 0;
  join_button_.enabled = library_names_.count != 0;
  turn_on_button_.hidden = on;
  summary_buttons_.hidden = relay ? NO : on;
  NSString* title = nil;
  NSString* detail = nil;
  if (!on) {
    title = NSLocalizedString(@"Online Is Off", @"online view controller: summary title");
    detail = NSLocalizedString(@"Seeing who's online, and System Link over the internet, need "
                               @"Online features.",
                               @"online view controller: footer");
  } else {
    NSDictionary<NSNumber*, NSNumber*>* titles = xe_ios_system_link_online_titles();
    // Apps open, this one included, when the lobby says; players in public lobbies.
    const NSInteger here = xe_ios_system_link_online_here();
    const NSInteger in_lobbies = xe_ios_system_link_online_total();
    const NSInteger in_private = std::max<NSInteger>(0, xe_ios_system_link_online_private());
    const NSInteger total = std::max(here, in_lobbies + in_private);
    if (!xe_ios_system_link_uses_relay()) {
      title = NSLocalizedString(@"Not Playing over the Internet",
                                @"online view controller: summary row");
    } else if (!titles) {
      title =
          NSLocalizedString(@"Checking Who's Online…", @"online view controller: summary row");
    } else if (total <= 0) {
      title = NSLocalizedString(@"Nobody Is Online Right Now",
                                @"online view controller: summary row");
      detail = here >= 0
                   ? NSLocalizedString(@"Players show up here while they have XeniOS open with "
                                       @"Online features on.",
                                       @"online view controller: summary row detail")
                   : NSLocalizedString(@"Players show up here while they're in a game's public "
                                       @"lobby.",
                                       @"online view controller: summary row detail");
    } else if (here == 1 && in_lobbies + in_private == 0) {
      title = NSLocalizedString(@"Only You Are Online", @"online view controller: summary row");
      detail = NSLocalizedString(@"Others show up here when they open XeniOS with Online "
                                 @"features on.",
                                 @"online view controller: summary row detail");
    } else if (here >= 0) {
      title = total == 1
                  ? NSLocalizedString(@"1 Player Online", @"online view controller: summary row")
                  : [NSString localizedStringWithFormat:NSLocalizedString(
                                                            @"%ld Players Online",
                                                            @"online view controller: summary "
                                                            @"row; %ld is how many"),
                                                        long(total)];
      if (in_lobbies + in_private == 0) {
        detail = NSLocalizedString(@"Nobody is in a game yet.",
                                   @"online view controller: summary row detail, after \"3 "
                                   @"Players Online\"");
      } else if (in_private && in_lobbies == 0) {
        detail =
            in_private == 1
                ? NSLocalizedString(@"1 of them is in a private room.",
                                    @"online view controller: summary row detail, after "
                                    @"\"3 Players Online\"")
                : [NSString localizedStringWithFormat:NSLocalizedString(
                                                          @"%ld of them are in private rooms.",
                                                          @"online view controller: summary row "
                                                          @"detail, after \"3 Players Online\"; "
                                                          @"%ld is how many"),
                                                      long(in_private)];
      } else if (in_private) {
        detail = [NSString
            localizedStringWithFormat:NSLocalizedString(
                                          @"%1$ld of them are in public lobbies, %2$ld in "
                                          @"private rooms.",
                                          @"online view controller: summary row detail, after "
                                          @"\"3 Players Online\"; %1$ld and %2$ld are how "
                                          @"many"),
                                      long(in_lobbies), long(in_private)];
      } else if (titles.count == 1) {
        detail =
            in_lobbies == 1
                ? NSLocalizedString(@"1 of them is in a game.",
                                    @"online view controller: summary row detail, after "
                                    @"\"3 Players Online\"")
                : [NSString localizedStringWithFormat:NSLocalizedString(
                                                          @"%ld of them are in 1 game.",
                                                          @"online view controller: summary row "
                                                          @"detail, after \"3 Players Online\"; "
                                                          @"%ld is how many"),
                                                      long(in_lobbies)];
      } else {
        detail = [NSString
            localizedStringWithFormat:NSLocalizedString(
                                          @"%1$ld of them are in %2$ld games.",
                                          @"online view controller: summary row detail, after "
                                          @"\"3 Players Online\"; %1$ld players, %2$ld games"),
                                      long(in_lobbies), long(titles.count)];
      }
    } else {
      title = total == 1
                  ? NSLocalizedString(@"1 Player Online", @"online view controller: summary row")
                  : [NSString localizedStringWithFormat:NSLocalizedString(
                                                            @"%ld Players Online",
                                                            @"online view controller: summary "
                                                            @"row; %ld is how many"),
                                                        long(total)];
      detail =
          titles.count == 1
              ? NSLocalizedString(@"In 1 game.", @"online view controller: summary row "
                                                 @"detail, after \"3 Players Online\"")
              : [NSString localizedStringWithFormat:NSLocalizedString(
                                                        @"In %ld games.",
                                                        @"online view controller: summary row "
                                                        @"detail, after \"3 Players Online\"; "
                                                        @"%ld is how many"),
                                                    long(titles.count)];
    }
    if (!xe_ios_system_link_uses_relay()) {
      detail = [NSString
          stringWithFormat:NSLocalizedString(@"System Link is set to “%@” in Settings, so it "
                                             @"doesn't play over the internet.",
                                             @"online view controller: footer; %@ is the "
                                             @"systemlink_transport setting"),
                           [NSString stringWithUTF8String:cvars::systemlink_transport.c_str()]];
    }
  }
  summary_title_.text = title;
  summary_detail_.text = detail;
  self.navigationItem.rightBarButtonItem.hidden = !relay || library_names_.count == 0;
  [self setNeedsUpdateContentUnavailableConfiguration];
}

- (OnlineRow)rowAtIndexPath:(NSIndexPath*)indexPath {
  return sections_[size_t(indexPath.section)][size_t(indexPath.row)];
}

// The game's box cover, rounded for a row; asks for it once when it isn't on hand yet.
- (UIImage*)coverForTitleID:(uint32_t)title_id {
  if (UIImage* cover = xe_cached_game_art_thumbnail(title_id) ?: xe_cached_game_art(title_id)) {
    return cover;
  }
  if (![cover_fetches_ containsObject:@(title_id)]) {
    [cover_fetches_ addObject:@(title_id)];
    xe_fetch_game_art(title_id, ^(UIImage* image) {
      if (image && self.isViewLoaded) {
        [self.tableView reloadData];
      }
    });
  }
  return nil;
}

// The game's icon, rounded and sized for a row, or nil without one.
- (UIImage*)iconForTitleID:(uint32_t)title_id {
  if (UIImage* icon = icons_[@(title_id)]) {
    return icon;
  }
  UIImage* source = [UIImage imageWithData:library_icons_[@(title_id)]];
  if (!source) {
    return nil;
  }
  const CGRect bounds = CGRectMake(0.0, 0.0, kOnlineIconSize, kOnlineIconSize);
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:bounds.size] autorelease];
  UIImage* icon = [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused context) {
    [[UIBezierPath bezierPathWithRoundedRect:bounds cornerRadius:8.0] addClip];
    [source drawInRect:bounds];
  }];
  icons_[@(title_id)] = icon;
  return icon;
}

- (NSString*)otherNameForTitleID:(uint32_t)title_id {
  if (!names_) {
    names_ = [LoadGameNames() copy];
  }
  return names_[XEFormatTitleIDHexUpper(title_id)] ?: UnnamedGame(title_id);
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return NSInteger(sections_.size());
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return NSInteger(sections_[size_t(section)].size());
}

- (UIView*)tableView:(UITableView* __unused)tableView viewForHeaderInSection:(NSInteger)section {
  const OnlineRow row = sections_[size_t(section)].front();
  const auto& games = row == OnlineRow::kOther ? others_ : mine_;
  NSInteger players = 0;
  for (const auto& game : games) {
    players += game.second;
  }
  NSString* count = players > 0 ? [NSString localizedStringWithFormat:@"%ld", long(players)] : nil;
  return NewOnlineSectionHeader(
      row == OnlineRow::kOther
          ? NSLocalizedString(@"Other Games", @"online view controller: section header")
          : NSLocalizedString(@"Your Games", @"online view controller: section header"),
      count);
}

// Under the last group: who's online in all, then how the counts work.
- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (size_t(section) + 1 != sections_.size()) {
    return nil;
  }
  NSMutableArray<NSString*>* lines = [NSMutableArray array];
  if (summary_title_.text.length) {
    [lines addObject:summary_detail_.text.length
                         ? [NSString stringWithFormat:@"%@. %@", summary_title_.text,
                                                      summary_detail_.text]
                         : summary_title_.text];
  }
  if (xe_ios_system_link_online_here() < 0) {
    [lines addObject:NSLocalizedString(@"Players in each game's public lobby, updated every "
                                       @"minute. Private rooms aren't counted.",
                                       @"system link view controller: footer")];
  }
  return [lines componentsJoinedByString:@"\n"];
}

// Nothing to list (Online off, not over the internet, or still asking): one centred message.
- (void)updateContentUnavailableConfigurationUsingState:(UIContentUnavailableConfigurationState*)
                                                            __unused state {
  if (!sections_.empty()) {
    self.contentUnavailableConfiguration = nil;
    return;
  }
  UIContentUnavailableConfiguration* config = [UIContentUnavailableConfiguration emptyConfiguration];
  const BOOL on = xe_ios_online_features_enabled();
  config.image = [UIImage systemImageNamed:on ? @"globe" : @"network.slash"];
  config.text = summary_title_.text;
  config.secondaryText = summary_detail_.text;
  if (!on) {
    UIButtonConfiguration* button = [UIButtonConfiguration filledButtonConfiguration];
    button.title = NSLocalizedString(@"Turn On Online Features",
                                     @"online view controller: button");
    button.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    button.baseBackgroundColor = [UIColor whiteColor];
    button.baseForegroundColor = [UIColor blackColor];
    button.contentInsets = NSDirectionalEdgeInsetsMake(12.0, 22.0, 12.0, 22.0);
    config.button = button;
    __unsafe_unretained XeniaIOSOnlineViewController* unsafe_self = self;
    config.buttonProperties.primaryAction =
        [UIAction actionWithHandler:^(__unused UIAction* action) {
          [unsafe_self turnOnTapped:nil];
        }];
  }
  self.contentUnavailableConfiguration = config;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaIOSOnlineCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor secondarySystemGroupedBackgroundColor];
  }
  const OnlineRow row = [self rowAtIndexPath:indexPath];
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.textProperties.font = xe_scaled_system_font(UIFontTextStyleBody, 17.0,
                                                      UIFontWeightSemibold);
  content.textProperties.color = [XeniaTheme textPrimary];
  content.secondaryTextProperties.font =
      xe_scaled_system_font(UIFontTextStyleSubheadline, 14.0, UIFontWeightRegular);
  content.textToSecondaryTextVerticalPadding = 1.0;
  content.imageToTextPadding = 13.0;
  content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(11.0, 16.0, 11.0, 12.0);
  cell.accessoryView = nil;
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.accessibilityTraits = UIAccessibilityTraitStaticText;
  if (row == OnlineRow::kNoneMine) {
    content.text = NSLocalizedString(@"Nobody is playing your games online right now.",
                                     @"online view controller: row");
    content.textProperties.font =
        xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
    content.textProperties.color = [XeniaTheme textSecondary];
    content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(14.0, 16.0, 14.0, 16.0);
    cell.contentConfiguration = content;
    return cell;
  }
  const bool mine = row == OnlineRow::kMine;
  const auto& [title_id, players] = (mine ? mine_ : others_)[size_t(indexPath.row)];
  content.text = mine ? library_names_[@(title_id)] : [self otherNameForTitleID:title_id];
  content.secondaryAttributedText = PlayingText(players);
  // The box cover, as in the library; the icon or a plain tile until it arrives.
  UIImage* cover = [self coverForTitleID:title_id];
  content.image = cover ?: [self iconForTitleID:title_id]
                               ?: XESettingsBadgeImage(@"gamecontroller.fill",
                                                       [UIColor systemGrayColor]);
  content.imageProperties.reservedLayoutSize = CGSizeMake(kOnlineCoverWidth, kOnlineCoverHeight);
  content.imageProperties.maximumSize =
      cover ? CGSizeMake(kOnlineCoverWidth, kOnlineCoverHeight)
            : CGSizeMake(kOnlineIconSize, kOnlineIconSize);
  content.imageProperties.cornerRadius = cover ? 5.0 : 8.0;
  cell.contentConfiguration = content;
  if (mine) {
    // Play opens the game's System Link sheet: its public lobby or a private room.
    UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
    config.title = NSLocalizedString(@"Play", @"online view controller: button on a game row");
    config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    config.baseBackgroundColor = [UIColor colorWithWhite:1.0 alpha:0.14];
    config.baseForegroundColor = [XeniaTheme textPrimary];
    config.contentInsets = NSDirectionalEdgeInsetsMake(6.0, 14.0, 6.0, 14.0);
    UIButton* play = [UIButton buttonWithConfiguration:config primaryAction:nil];
    xe_apply_button_title_font(play, UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
    play.tag = NSInteger(title_id);
    play.accessibilityLabel = [NSString
        stringWithFormat:NSLocalizedString(@"Play %@", @"online view controller: VoiceOver "
                                                       @"label of a game row's Play button"),
                         content.text];
    [play addTarget:self action:@selector(playTapped:) forControlEvents:UIControlEventTouchUpInside];
    [play sizeToFit];
    cell.accessoryView = play;
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    cell.accessibilityTraits = UIAccessibilityTraitButton;
  }
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if ([self rowAtIndexPath:indexPath] == OnlineRow::kMine) {
    [self finishWithTitleID:mine_[size_t(indexPath.row)].first];
  }
}

// Closes the sheet and opens the game's System Link sheet, to play.
- (void)finishWithTitleID:(uint32_t)title_id {
  void (^open)(uint32_t) = [open_game_ copy];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             if (open) {
                               open(title_id);
                             }
                             [open release];
                           }];
}

- (void)pickGameWithPrompt:(NSString*)prompt
                      then:(void (^)(uint32_t title_id, UIView* source))then {
  NSArray<NSNumber*>* games =
      [library_names_ keysSortedByValueUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
  NSMutableDictionary<NSNumber*, UIImage*>* icons = [NSMutableDictionary dictionary];
  for (NSNumber* title_id in games) {
    if (UIImage* icon = [self iconForTitleID:title_id.unsignedIntValue]) {
      icons[title_id] = icon;
    }
  }
  XeniaIOSOnlineGamePickerViewController* picker =
      [[[XeniaIOSOnlineGamePickerViewController alloc] initWithGames:games
                                                               names:library_names_
                                                               icons:icons
                                                              prompt:prompt
                                                                pick:then] autorelease];
  [self.navigationController pushViewController:picker animated:YES];
}

// Shares a new room's invite, then opens the game's System Link sheet with the room chosen.
- (void)shareRoom:(NSString*)code titleID:(uint32_t)title_id from:(UIView*)source {
  NSString* text = [NSString
      stringWithFormat:NSLocalizedString(@"Play %1$@ with me over System Link in XeniOS. Room "
                                         @"code: %2$@",
                                         @"System Link invite text; %1$@ is the game, %2$@ the "
                                         @"room code"),
                       library_names_[@(title_id)] ?: @"", code];
  UIActivityViewController* share = [[[UIActivityViewController alloc]
      initWithActivityItems:@[ text, xe_ios_system_link_invite_url(title_id, code) ]
      applicationActivities:nil] autorelease];
  share.popoverPresentationController.sourceView = source;
  share.popoverPresentationController.sourceRect = source.bounds;
  share.completionWithItemsHandler = ^(UIActivityType __unused type, BOOL __unused completed,
                                       NSArray* __unused items, NSError* __unused error) {
    [self finishWithTitleID:title_id];
  };
  [self.navigationController.topViewController presentViewController:share
                                                            animated:YES
                                                          completion:nil];
}

- (void)presentJoinAlert {
  __block UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Join a Private Room",
                                                 @"system link view controller: alert title")
                       message:NSLocalizedString(@"Enter the 6-character code you were sent.",
                                                 @"system link view controller: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
    field.placeholder = @"ABC123";
    field.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    field.spellCheckingType = UITextSpellCheckingTypeNo;
    field.font = [UIFont monospacedSystemFontOfSize:17.0 weight:UIFontWeightRegular];
  }];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel",
                                                         @"system link view controller: button")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  UIAlertAction* join = [UIAlertAction
      actionWithTitle:NSLocalizedString(@"Join", @"system link view controller: button")
                style:UIAlertActionStyleDefault
              handler:^(__unused UIAlertAction* action) {
                NSString* code =
                    xe_ios_normalize_system_link_room_code(alert.textFields.firstObject.text);
                if (!code) {
                  XEPresentOKAlert(
                      self,
                      NSLocalizedString(@"Not a Room Code",
                                        @"system link view controller: alert title"),
                      NSLocalizedString(@"Room codes have 6 letters and digits.",
                                        @"system link view controller: alert message"));
                  return;
                }
                [self pickGameWithPrompt:NSLocalizedString(@"Which game is the room for?",
                                                           @"online game picker: header")
                                    then:^(uint32_t title_id, UIView* __unused source) {
                                      xe_ios_set_system_link_room(title_id, code);
                                      [self finishWithTitleID:title_id];
                                    }];
              }];
  [alert addAction:join];
  alert.preferredAction = join;
  [self presentViewController:alert animated:YES completion:nil];
}

@end

void xe_ios_present_online_sheet(UIViewController* presenter,
                                 const std::vector<XeniaIOSOnlineLibraryGame>& library,
                                 void (^open_game)(uint32_t title_id)) {
  XeniaIOSOnlineViewController* controller =
      [[XeniaIOSOnlineViewController alloc] initWithLibrary:library openGame:open_game];
  XeniaLandscapeNavigationController* navigation =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:controller];
  XEConfigureDestinationPresentation(navigation, presenter.view, CGSizeMake(520.0, 620.0), NO);
  [presenter presentViewController:navigation animated:YES completion:nil];
  [navigation release];
  [controller release];
}

namespace {

enum class StatusRow {
  kState,
  kRoom,
  kShare,
  kLeaveRoom,
  kCreateRoom,
  kJoinRoom,
  kWaiting,
  kDirect,
  kRelayed,
  kNotConnected,
  kOtherVersion,
  kThisGame,
  kOtherGame,
  kNobodyOnline,
};

// Other games listed in the status sheet, at most.
constexpr size_t kMaxStatusOtherGames = 10;

}  // namespace

// The running game's System Link: how it's connected, its room and players, and who's online.
@interface XeniaIOSSystemLinkStatusViewController : XESheetTableViewController
@end

@implementation XeniaIOSSystemLinkStatusViewController {
  XeniaIOSSystemLinkSnapshot snapshot_;
  std::vector<std::vector<StatusRow>> sections_;
  std::vector<std::pair<uint32_t, NSInteger>> others_;
  NSDictionary<NSString*, NSString*>* names_;
}

- (instancetype)init {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = NSLocalizedString(@"System Link", @"system link view controller: title");
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [snapshot_.room_code release];
  [snapshot_.problem release];
  [names_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                               target:self
             action:@selector(doneTapped:)] autorelease];
  for (NSString* name in @[
         XeniaIOSSystemLinkStatusDidChangeNotification,
         XeniaIOSSystemLinkOnlineDidChangeNotification
       ]) {
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(changed:)
                                                 name:name
                                               object:nil];
  }
  [self reloadRows];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  xe_ios_system_link_keep_online(YES);
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  xe_ios_system_link_keep_online(NO);
}

- (void)changed:(NSNotification* __unused)note {
  [self reloadRows];
}

- (void)doneTapped:(id)__unused sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (NSString*)nameForTitleID:(uint32_t)title_id {
  if (!names_) {
    names_ = [LoadGameNames() copy];
  }
  return names_[XEFormatTitleIDHexUpper(title_id)] ?: UnnamedGame(title_id);
}

- (void)reloadRows {
  using State = XeniaIOSSystemLinkSnapshot::State;
  [snapshot_.room_code release];
  [snapshot_.problem release];
  snapshot_ = xe_ios_system_link_snapshot();
  [snapshot_.room_code retain];
  [snapshot_.problem retain];
  sections_.clear();
  others_.clear();
  sections_.push_back({StatusRow::kState});
  if (snapshot_.state != State::kOff) {
    std::vector<StatusRow> room{StatusRow::kRoom};
    if (snapshot_.room_code.length) {
      room.push_back(StatusRow::kShare);
    }
    // Rooms change while the game runs, once it's known which game.
    if (snapshot_.title_id && snapshot_.room_code.length) {
      room.insert(room.end(), {StatusRow::kLeaveRoom, StatusRow::kJoinRoom});
    } else if (snapshot_.title_id) {
      room.insert(room.end(), {StatusRow::kCreateRoom, StatusRow::kJoinRoom});
    }
    sections_.push_back(room);
  }
  if (snapshot_.state == State::kOnline) {
    std::vector<StatusRow> players;
    const size_t waiting =
        snapshot_.others - std::min(snapshot_.others, snapshot_.direct + snapshot_.relayed);
    if (!snapshot_.others) {
      players.push_back(StatusRow::kWaiting);
    }
    if (snapshot_.direct) {
      players.push_back(StatusRow::kDirect);
    }
    if (snapshot_.relayed) {
      players.push_back(StatusRow::kRelayed);
    }
    if (waiting) {
      players.push_back(StatusRow::kNotConnected);
    }
    if (snapshot_.other_version) {
      players.push_back(StatusRow::kOtherVersion);
    }
    sections_.push_back(players);
  }
  if (xe_ios_system_link_online_titles()) {
    std::vector<StatusRow> online;
    if (snapshot_.title_id) {
      online.push_back(StatusRow::kThisGame);
    }
    for (const auto& game : SortedOnline()) {
      if (game.first != snapshot_.title_id && others_.size() < kMaxStatusOtherGames) {
        others_.push_back(game);
      }
    }
    online.insert(online.end(), others_.size(), StatusRow::kOtherGame);
    sections_.push_back(online.empty() ? std::vector<StatusRow>{StatusRow::kNobodyOnline} : online);
  }
  [self.tableView reloadData];
}

- (StatusRow)rowAtIndexPath:(NSIndexPath*)indexPath {
  return sections_[size_t(indexPath.section)][size_t(indexPath.row)];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return NSInteger(sections_.size());
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return NSInteger(sections_[size_t(section)].size());
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  switch (sections_[size_t(section)].front()) {
    case StatusRow::kRoom:
      return NSLocalizedString(@"Room", @"system link view controller: section header");
    case StatusRow::kWaiting:
    case StatusRow::kDirect:
    case StatusRow::kRelayed:
    case StatusRow::kNotConnected:
    case StatusRow::kOtherVersion:
      return NSLocalizedString(@"Players in This Room",
                               @"system link status view controller: section header");
    case StatusRow::kThisGame:
    case StatusRow::kOtherGame:
    case StatusRow::kNobodyOnline:
      return NSLocalizedString(@"Playing Online Now",
                               @"system link view controller: section header");
    default:
      return nil;
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  switch (sections_[size_t(section)].front()) {
    case StatusRow::kRoom:
      return snapshot_.others
                 ? NSLocalizedString(@"Changing rooms takes a few seconds. The players in this "
                                     @"room lose their connection to you.",
                                     @"system link status view controller: footer")
                 : NSLocalizedString(@"Changing rooms takes a few seconds.",
                                     @"system link status view controller: footer");
    case StatusRow::kWaiting:
    case StatusRow::kDirect:
    case StatusRow::kRelayed:
    case StatusRow::kNotConnected:
    case StatusRow::kOtherVersion:
      return NSLocalizedString(@"Players in your room can see your IP address and a profile ID.",
                               @"system link status view controller: footer");
    case StatusRow::kThisGame:
    case StatusRow::kOtherGame:
    case StatusRow::kNobodyOnline: {
      const NSInteger here = xe_ios_system_link_online_here();
      if (here > 1) {
        return [NSString
            localizedStringWithFormat:NSLocalizedString(
                                          @"%ld players have XeniOS open with Online features "
                                          @"on, you included. Games list their public lobbies; "
                                          @"private rooms aren't listed.",
                                          @"system link status view controller: footer; %ld is "
                                          @"how many"),
                                      long(here)];
      }
      return NSLocalizedString(@"Players in each game's public lobby, updated every minute. "
                               @"Private rooms aren't counted.",
                               @"system link view controller: footer");
    }
    default:
      return nil;
  }
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  using State = XeniaIOSSystemLinkSnapshot::State;
  const StatusRow row = [self rowAtIndexPath:indexPath];
  const bool count_row =
      row == StatusRow::kDirect || row == StatusRow::kRelayed || row == StatusRow::kNotConnected;
  NSString* identifier = count_row ? @"XeniaIOSSystemLinkStatusCount" : @"XeniaIOSSystemLinkStatus";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:identifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc]
          initWithStyle:count_row ? UITableViewCellStyleValue1 : UITableViewCellStyleSubtitle
        reuseIdentifier:identifier] autorelease];
    cell.backgroundColor = [UIColor secondarySystemGroupedBackgroundColor];
    cell.detailTextLabel.numberOfLines = count_row ? 1 : 0;
    cell.textLabel.numberOfLines = 0;
  }
  NSString* title = nil;
  NSString* detail = nil;
  NSString* symbol = nil;
  UIColor* tint = [XeniaTheme accent];
  BOOL button = NO;
  switch (row) {
    case StatusRow::kState:
      switch (snapshot_.state) {
        case State::kOff:
          title = NSLocalizedString(@"Not Started", @"system link status view controller: state");
          detail = NSLocalizedString(@"System Link starts when the game looks for other players.",
                                     @"system link status view controller: state detail");
          symbol = @"network.slash";
          tint = [UIColor systemGrayColor];
          break;
        case State::kConnecting:
          title = NSLocalizedString(@"Connecting…", @"system link status view controller: state");
          symbol = @"network";
          tint = [UIColor systemGrayColor];
          break;
        case State::kReconnecting:
          title =
              NSLocalizedString(@"Reconnecting…", @"system link status view controller: state");
          detail = NSLocalizedString(@"The server still holds your old connection. Rejoining.",
                                     @"system link status view controller: state detail");
          symbol = @"network";
          tint = [UIColor systemGrayColor];
          break;
        case State::kOnline:
          title = NSLocalizedString(@"Online", @"system link status view controller: state");
          detail = NSLocalizedString(@"Connected to the System Link server.",
                                     @"system link status view controller: state detail");
          symbol = @"network";
          break;
        case State::kOffline:
          title = NSLocalizedString(@"Can't Reach the Server",
                                    @"system link status view controller: state");
          detail = NSLocalizedString(@"Still trying. Check your internet connection.",
                                     @"system link status view controller: state detail");
          symbol = @"exclamationmark.triangle.fill";
          tint = [XeniaTheme statusWarning];
          break;
        case State::kRejected:
          title = NSLocalizedString(@"Turned Away", @"system link status view controller: state");
          detail = snapshot_.problem;
          symbol = @"exclamationmark.triangle.fill";
          tint = [XeniaTheme statusWarning];
          break;
      }
      break;
    case StatusRow::kRoom:
      title = xe_ios_system_link_room_name(snapshot_.room_code);
      detail = snapshot_.room_code.length
                   ? NSLocalizedString(@"Only players with the code.",
                                       @"system link view controller: room row detail")
                   : NSLocalizedString(@"Everyone playing this game.",
                                       @"system link view controller: room row detail");
      symbol = snapshot_.room_code.length ? @"lock" : @"globe";
      break;
    case StatusRow::kShare:
      title = NSLocalizedString(@"Share Invite…", @"system link view controller: button row");
      symbol = @"square.and.arrow.up";
      button = YES;
      break;
    case StatusRow::kLeaveRoom:
      title = NSLocalizedString(@"Play in the Public Lobby",
                                @"system link status view controller: button row");
      symbol = @"globe";
      button = YES;
      break;
    case StatusRow::kCreateRoom:
      title = NSLocalizedString(@"Make a Private Room", @"system link view controller: button row");
      symbol = @"lock";
      button = YES;
      break;
    case StatusRow::kJoinRoom:
      title =
          snapshot_.room_code.length
              ? NSLocalizedString(@"Join Another Room…",
                                  @"system link status view controller: button row")
              : NSLocalizedString(@"Join with Code…", @"system link view controller: button row");
      symbol = @"keyboard";
      button = YES;
      break;
    case StatusRow::kWaiting:
      title = NSLocalizedString(@"Waiting for Players", @"system link status view controller: row");
      detail = NSLocalizedString(@"They show up here when they start this game in the same room.",
                                 @"system link status view controller: row detail");
      symbol = @"person.2.slash";
      tint = [UIColor systemGrayColor];
      break;
    case StatusRow::kDirect:
      title = NSLocalizedString(@"Connected Directly", @"system link status view controller: row");
      detail = [NSString stringWithFormat:@"%lu", (unsigned long)snapshot_.direct];
      symbol = @"person.2.fill";
      break;
    case StatusRow::kRelayed:
      title = NSLocalizedString(@"Connected Through the Relay",
                                @"system link status view controller: row");
      detail = [NSString stringWithFormat:@"%lu", (unsigned long)snapshot_.relayed];
      symbol = @"arrow.triangle.branch";
      break;
    case StatusRow::kNotConnected:
      title = NSLocalizedString(@"Not Connected Yet", @"system link status view controller: row");
      detail = [NSString
          stringWithFormat:@"%lu", (unsigned long)(snapshot_.others -
                                                   std::min(snapshot_.others,
                                                            snapshot_.direct + snapshot_.relayed))];
      symbol = @"person.crop.circle.badge.questionmark";
      tint = [XeniaTheme statusWarning];
      break;
    case StatusRow::kOtherVersion:
      title =
          NSLocalizedString(@"Different Game Version", @"system link status view controller: row");
      detail = NSLocalizedString(@"Another player runs a different version of this game. Games "
                                 @"usually only see players on the same version.",
                                 @"system link status view controller: row detail");
      symbol = @"exclamationmark.triangle.fill";
      tint = [XeniaTheme statusWarning];
      break;
    case StatusRow::kThisGame: {
      const NSInteger players = xe_ios_system_link_online_players(snapshot_.title_id);
      title = [self nameForTitleID:snapshot_.title_id];
      detail = players ? PlayersText(players)
                       : NSLocalizedString(@"Nobody is in its public lobby.",
                                           @"system link status view controller: row detail");
      symbol = @"person.2.fill";
      break;
    }
    case StatusRow::kOtherGame: {
      const auto& [title_id, players] =
          others_[size_t(indexPath.row) - (snapshot_.title_id ? 1 : 0)];
      title = [self nameForTitleID:title_id];
      detail = PlayersText(players);
      symbol = @"person.2";
      break;
    }
    case StatusRow::kNobodyOnline:
      title = NSLocalizedString(@"Nobody is in a public lobby right now.",
                                @"system link view controller: row");
      symbol = @"person.2.slash";
      tint = [UIColor systemGrayColor];
      break;
  }
  cell.textLabel.text = title;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text = detail;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.imageView.image = XESettingsBadgeImage(symbol, tint);
  cell.selectionStyle =
      button ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
  cell.accessibilityTraits = button ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  UIView* source = [tableView cellForRowAtIndexPath:indexPath] ?: self.view;
  const uint32_t title_id = snapshot_.title_id;
  switch ([self rowAtIndexPath:indexPath]) {
    case StatusRow::kShare:
      if (snapshot_.room_code.length) {
        [self shareRoom:snapshot_.room_code from:source];
      }
      break;
    case StatusRow::kLeaveRoom:
      if (title_id) {
        xe_ios_set_system_link_room(title_id, nil);
      }
      break;
    case StatusRow::kCreateRoom:
      if (title_id) {
        NSString* code = xe_ios_new_system_link_room_code();
        xe_ios_set_system_link_room(title_id, code);
        [self shareRoom:code from:source];
      }
      break;
    case StatusRow::kJoinRoom:
      if (title_id) {
        [self presentJoinAlertForTitleID:title_id];
      }
      break;
    default:
      break;
  }
}

- (void)presentJoinAlertForTitleID:(uint32_t)title_id {
  __block UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Join a Private Room",
                                                 @"system link view controller: alert title")
                       message:NSLocalizedString(@"Enter the 6-character code you were sent.",
                                                 @"system link view controller: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
    field.placeholder = @"ABC123";
    field.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    field.spellCheckingType = UITextSpellCheckingTypeNo;
    field.font = [UIFont monospacedSystemFontOfSize:17.0 weight:UIFontWeightRegular];
  }];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel",
                                                         @"system link view controller: button")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  UIAlertAction* join = [UIAlertAction
      actionWithTitle:NSLocalizedString(@"Join", @"system link view controller: button")
                style:UIAlertActionStyleDefault
              handler:^(__unused UIAlertAction* action) {
                NSString* code =
                    xe_ios_normalize_system_link_room_code(alert.textFields.firstObject.text);
                if (!code) {
                  XEPresentOKAlert(
                      self,
                      NSLocalizedString(@"Not a Room Code",
                                        @"system link view controller: alert title"),
                      NSLocalizedString(@"Room codes have 6 letters and digits.",
                                        @"system link view controller: alert message"));
                  return;
                }
                xe_ios_set_system_link_room(title_id, code);
              }];
  [alert addAction:join];
  alert.preferredAction = join;
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)shareRoom:(NSString*)code from:(UIView*)source {
  NSString* text = [NSString
      stringWithFormat:NSLocalizedString(@"Play %1$@ with me over System Link in XeniOS. Room "
                                         @"code: %2$@",
                                         @"System Link invite text; %1$@ is the game, %2$@ the "
                                         @"room code"),
                       [self nameForTitleID:snapshot_.title_id], code];
  UIActivityViewController* share = [[[UIActivityViewController alloc]
      initWithActivityItems:@[ text, xe_ios_system_link_invite_url(snapshot_.title_id, code) ]
      applicationActivities:nil] autorelease];
  share.popoverPresentationController.sourceView = source;
  share.popoverPresentationController.sourceRect = source.bounds;
  [self presentViewController:share animated:YES completion:nil];
}

@end

void xe_ios_present_system_link_status_sheet(UIViewController* presenter) {
  XeniaIOSSystemLinkStatusViewController* controller =
      [[XeniaIOSSystemLinkStatusViewController alloc] init];
  XeniaLandscapeNavigationController* navigation =
      [[XeniaLandscapeNavigationController alloc] initWithRootViewController:controller];
  XEConfigureDestinationPresentation(navigation, presenter.view, CGSizeMake(520.0, 620.0), NO);
  [presenter presentViewController:navigation animated:YES completion:nil];
  [navigation release];
  [controller release];
}
