/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_storage_view_controller.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "xenia/base/logging.h"

#import "xenia/ui/ios/launcher/ios_content_management.h"
#import "xenia/ui/ios/launcher/ios_storage.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

constexpr IOSStorageKind kListedKinds[] = {
    IOSStorageKind::kGames,    IOSStorageKind::kLinked,  IOSStorageKind::kContent,
    IOSStorageKind::kSaves,    IOSStorageKind::kGameData, IOSStorageKind::kShaders,
    IOSStorageKind::kOther,    IOSStorageKind::kLeftovers,
};

NSString* KindName(IOSStorageKind kind) {
  switch (kind) {
    case IOSStorageKind::kGames:
      return NSLocalizedString(@"Games", @"storage: kind");
    case IOSStorageKind::kContent:
      return NSLocalizedString(@"Updates and Add-ons", @"storage: kind");
    case IOSStorageKind::kSaves:
      return NSLocalizedString(@"Saves and Profiles", @"storage: kind");
    case IOSStorageKind::kGameData:
      return NSLocalizedString(@"Game Data", @"storage: kind; what games install to the cache");
    case IOSStorageKind::kShaders:
      return NSLocalizedString(@"Shaders", @"storage: kind");
    case IOSStorageKind::kOther:
      return NSLocalizedString(@"Art, Logs and Other", @"storage: kind; also system software");
    case IOSStorageKind::kLeftovers:
      return NSLocalizedString(@"Leftover Files", @"storage: kind");
    case IOSStorageKind::kLinked:
      return NSLocalizedString(@"Linked Games", @"storage: kind; games in linked folders on "
                                                @"this device");
    default:
      return @"";
  }
}

UIColor* KindColor(IOSStorageKind kind) {
  switch (kind) {
    case IOSStorageKind::kGames:
      return [UIColor systemBlueColor];
    case IOSStorageKind::kContent:
      return [UIColor systemTealColor];
    case IOSStorageKind::kSaves:
      return [UIColor systemOrangeColor];
    case IOSStorageKind::kGameData:
      return [UIColor systemIndigoColor];
    case IOSStorageKind::kShaders:
      return [UIColor systemPurpleColor];
    case IOSStorageKind::kLeftovers:
      return [UIColor systemYellowColor];
    case IOSStorageKind::kLinked:
      return [UIColor systemGreenColor];
    default:
      return [UIColor systemGrayColor];
  }
}

uint64_t KindBytes(const IOSStorageBytes& bytes, IOSStorageKind kind) {
  return bytes[size_t(kind)];
}

NSString* ByteText(uint64_t bytes) {
  // Not the class method: it writes "Zero KB".
  static NSByteCountFormatter* formatter = [] {
    NSByteCountFormatter* f = [[NSByteCountFormatter alloc] init];
    f.countStyle = NSByteCountFormatterCountStyleFile;
    f.allowsNonnumericFormatting = NO;
    return f;
  }();
  return [formatter stringFromByteCount:static_cast<long long>(bytes)];
}

// A row's value at its trailing edge, with the chevron of a row that opens a page. A label of
// its own: beside a long name the cell's detail label is the one that truncates ("7.11…"),
// where this keeps its width and the name wraps.
UIView* ValueAccessory(NSString* text, BOOL chevron) {
  UILabel* label = [[[UILabel alloc] init] autorelease];
  xe_apply_label_font(label, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  label.textColor = [XeniaTheme textSecondary];
  label.text = text;
  [label sizeToFit];
  CGFloat width = ceil(label.bounds.size.width);
  const CGFloat height = ceil(label.bounds.size.height);
  UIView* view = [[[UIView alloc] init] autorelease];
  label.frame = CGRectMake(0.0, 0.0, width, height);
  [view addSubview:label];
  if (chevron) {
    UIImageSymbolConfiguration* symbol =
        [UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote
                                                         scale:UIImageSymbolScaleMedium];
    symbol = [symbol configurationByApplyingConfiguration:
                         [UIImageSymbolConfiguration
                             configurationWithWeight:UIImageSymbolWeightSemibold]];
    UIImageView* arrow = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:@"chevron.right" withConfiguration:symbol]]
        autorelease];
    arrow.tintColor = [UIColor tertiaryLabelColor];
    [arrow sizeToFit];
    const CGSize size = arrow.bounds.size;
    arrow.frame = CGRectMake(width + 10.0, round((height - size.height) / 2.0), size.width,
                             size.height);
    [view addSubview:arrow];
    width += 10.0 + ceil(size.width);
  }
  view.frame = CGRectMake(0.0, 0.0, width, height);
  view.isAccessibilityElement = NO;
  return view;
}

// The cell's detail text becomes its trailing value (see ValueAccessory).
void MoveDetailToAccessory(UITableViewCell* cell) {
  NSString* value = [[cell.detailTextLabel.text copy] autorelease];
  cell.detailTextLabel.text = nil;
  const BOOL chevron = cell.accessoryType == UITableViewCellAccessoryDisclosureIndicator;
  cell.accessoryView = value.length ? ValueAccessory(value, chevron) : nil;
  XEApplyAccessibility(cell, cell.textLabel.text, value, nil, cell.accessibilityTraits);
}

NSString* const kSortByNameDefaultsKey = @"XeniaStorageSortByName";

// Played from a linked folder, with no copy in the app's storage.
bool IsLinkedOnly(const IOSStorageTitle& title) {
  return title.has_linked_copy && !title.has_imported_copy;
}

// The same game in a newer report: by title ID, or by path for a game without one.
bool SameTitle(const IOSStorageTitle& a, const IOSStorageTitle& b) {
  return a.title_id ? a.title_id == b.title_id : !b.title_id && a.game_path == b.game_path;
}

// A row's identity while it is selected, across measures and sorts. Its section is part of
// it: a game that left the library keeps its data under the same title ID, and that row was
// never chosen.
std::string SelectionKey(const IOSStorageTitle& title) {
  std::string key = title.in_library ? "L:" : "N:";
  if (title.title_id) {
    char id[9];
    std::snprintf(id, sizeof(id), "%08X", title.title_id);
    return key + id;
  }
  return key + "/" + title.game_path;
}

// What original Xbox games keep together, under the console's Xbox emulator: not a game.
bool IsXboxShared(const IOSStorageTitle& title) {
  return !title.in_library && title.title_id == kIOSStorageXboxEmulatorTitleId;
}

NSString* TitleName(const IOSStorageTitle& title) {
  if (IsXboxShared(title)) {
    return NSLocalizedString(@"Shared by Original Xbox Games", @"storage: row");
  }
  if (!title.name.empty()) {
    if (NSString* name = [NSString stringWithUTF8String:title.name.c_str()]; name.length) {
      return name;
    }
  }
  if (!title.title_id && !title.game_path.empty()) {
    if (NSString* path = [NSString stringWithUTF8String:title.game_path.c_str()]; path.length) {
      return path.lastPathComponent.stringByDeletingPathExtension;
    }
  }
  return [NSString stringWithFormat:NSLocalizedString(@"Game %@", @"storage: unnamed game; %@ is "
                                                                  @"its title ID"),
                                    XEFormatTitleIDHexUpper(title.title_id)];
}

// The name a disc's file goes by: a folder game is the folder its executable is in.
NSString* DiscFileName(const IOSStorageDisc& disc) {
  NSString* path = [NSString stringWithUTF8String:disc.path.c_str()];
  NSString* name = path.lastPathComponent;
  if ([name caseInsensitiveCompare:@"default.xex"] == NSOrderedSame ||
      [name caseInsensitiveCompare:@"default.xbe"] == NSOrderedSame) {
    name = path.stringByDeletingLastPathComponent.lastPathComponent;
  }
  return name.length ? name : @"";
}

// What the launcher says when asked to delete a game while one runs.
void PresentCloseGameFirst(UIViewController* presenter) {
  XEPresentOKAlert(presenter,
                   NSLocalizedString(@"Couldn’t Delete Game", @"Delete game: alert title"),
                   NSLocalizedString(@"Close the game first.", @"Delete game: alert message"));
}

// A small rounded square in a kind's colour, as a row's leading image.
UIImage* SwatchImage(UIColor* color) {
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(29, 29)] autorelease];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused context) {
    [color setFill];
    [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(8.5, 8.5, 12, 12) cornerRadius:3] fill];
  }];
}

// What a measure found, with the device's free space.
struct Measurement {
  IOSStorageReport report;
  uint64_t free_bytes = 0;
};

}  // namespace

#pragma mark - Bar

// The space each kind takes, as one bar split in proportion.
@interface XeniaStorageBarView : UIView
- (void)setBytes:(const IOSStorageBytes&)bytes;
@end

@implementation XeniaStorageBarView {
  IOSStorageBytes bytes_;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.opaque = NO;
    self.contentMode = UIViewContentModeRedraw;
    self.isAccessibilityElement = NO;
  }
  return self;
}

- (CGSize)intrinsicContentSize {
  return CGSizeMake(UIViewNoIntrinsicMetric, 14);
}

- (void)setBytes:(const IOSStorageBytes&)bytes {
  bytes_ = bytes;
  [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)__unused rect {
  const CGRect bounds = self.bounds;
  [[UIBezierPath bezierPathWithRoundedRect:bounds cornerRadius:4] addClip];
  [[XeniaTheme bgSurface2] setFill];
  UIRectFill(bounds);
  uint64_t total = 0;
  for (uint64_t bytes : bytes_) {
    total += bytes;
  }
  if (!total) {
    return;
  }
  CGFloat x = bounds.origin.x;
  for (IOSStorageKind kind : kListedKinds) {
    const CGFloat width = bounds.size.width * CGFloat(KindBytes(bytes_, kind)) / CGFloat(total);
    if (width < 0.5) {
      continue;
    }
    [KindColor(kind) setFill];
    // A hairline between kinds.
    UIRectFill(
        CGRectMake(x, bounds.origin.y, std::max<CGFloat>(width - 1.5, 1.0), bounds.size.height));
    x += width;
  }
}

@end

#pragma mark - One game

@class XeniaStorageViewController;

@interface XeniaStorageViewController ()
- (void)confirmTitle:(NSString*)title
             message:(NSString*)message
              button:(NSString*)button
                from:(UIViewController*)presenter
                work:(BOOL (^)(std::string* error))work;
- (void)measure;
@property(nonatomic, readonly) BOOL gameRunning;
@end

// One game's files by kind, and what can be removed of them.
@interface XeniaStorageTitleViewController : XESheetTableViewController
- (instancetype)initWithTitle:(const IOSStorageTitle&)title
                        owner:(XeniaStorageViewController*)owner;
- (void)applyReport:(const IOSStorageReport&)report;
@end

namespace {
enum class TitleSection { kKinds, kDiscs, kActions };
enum class TitleAction {
  kClearShaders,
  kDeleteContent,
  kDeleteSaves,
  kDeleteGame,
  kDeleteAllData,
};
}  // namespace

@implementation XeniaStorageTitleViewController {
  IOSStorageTitle title_;
  XeniaStorageViewController* owner_;  // Not retained: it owns this page's stack.
  XEStatusHeaderView* header_;
  std::vector<TitleSection> sections_;
  std::vector<IOSStorageKind> kinds_;
  std::vector<TitleAction> actions_;
}

- (instancetype)initWithTitle:(const IOSStorageTitle&)title
                        owner:(XeniaStorageViewController*)owner {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    title_ = title;
    owner_ = owner;
    [self rebuild];
  }
  return self;
}

- (void)dealloc {
  [header_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  // A disc's row is two lines, and a long file name wraps.
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 52.0;
  header_ = [[XEStatusHeaderView alloc] init];
  [self refreshHeader];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [header_ fitAsHeaderOfTable:self.tableView];
}

- (void)applyReport:(const IOSStorageReport&)report {
  for (const IOSStorageTitle& title : report.titles) {
    if (SameTitle(title_, title)) {
      title_ = title;
      [self rebuild];
      [self refreshHeader];
      [self.tableView reloadData];
      return;
    }
  }
  // Nothing of it is left.
  if (self.navigationController.topViewController == self) {
    [self.navigationController popViewControllerAnimated:YES];
  }
}

- (void)rebuild {
  self.title = TitleName(title_);
  kinds_.clear();
  for (IOSStorageKind kind : kListedKinds) {
    // The Games row says what a linked copy takes.
    if (kind == IOSStorageKind::kLinked) {
      continue;
    }
    if (KindBytes(title_.bytes, kind) || (kind == IOSStorageKind::kGames && title_.in_library)) {
      kinds_.push_back(kind);
    }
  }
  actions_.clear();
  if (KindBytes(title_.bytes, IOSStorageKind::kShaders)) {
    actions_.push_back(TitleAction::kClearShaders);
  }
  if (title_.in_library) {
    if (KindBytes(title_.bytes, IOSStorageKind::kContent)) {
      actions_.push_back(TitleAction::kDeleteContent);
    }
    if (KindBytes(title_.bytes, IOSStorageKind::kSaves)) {
      actions_.push_back(TitleAction::kDeleteSaves);
    }
    actions_.push_back(TitleAction::kDeleteGame);
  } else {
    actions_.push_back(TitleAction::kDeleteAllData);
  }
  sections_.clear();
  sections_.push_back(TitleSection::kKinds);
  // One disc of several can go by itself; the only one goes with Delete Game.
  if (title_.in_library && title_.discs.size() > 1) {
    sections_.push_back(TitleSection::kDiscs);
  }
  sections_.push_back(TitleSection::kActions);
}

- (void)refreshHeader {
  if (!header_) {
    return;
  }
  header_.titleLabel.text = ByteText(title_.Total());
  header_.dot.backgroundColor = title_.in_library ? [XeniaTheme accent] : [XeniaTheme textMuted];
  header_.detailLabel.text =
      IsXboxShared(title_)
          ? NSLocalizedString(@"The console’s own Xbox emulator runs every original Xbox "
                              @"game, so their shaders and saved data are kept together.",
                              @"storage: game page header")
      : title_.in_library
          ? (title_.original_xbox
                 ? (IsLinkedOnly(title_)
                        ? NSLocalizedString(@"Original Xbox game, played from a linked folder",
                                            @"storage: game page header")
                        : NSLocalizedString(@"Original Xbox game, in your library",
                                            @"storage: game page header"))
             : IsLinkedOnly(title_)
                 ? NSLocalizedString(@"In your library, played from a linked folder",
                                     @"storage: game page header")
                 : NSLocalizedString(@"In your library", @"storage: game page header"))
          : NSLocalizedString(@"Not in your library. These files are left from a game you "
                              @"removed or never added.",
                              @"storage: game page header");
  [self.view setNeedsLayout];
}

- (NSString*)titleForKind:(IOSStorageKind)kind {
  if (kind == IOSStorageKind::kGames) {
    return NSLocalizedString(@"Game", @"storage: game page row");
  }
  if (kind == IOSStorageKind::kOther) {
    return NSLocalizedString(@"Art", @"storage: game page row");
  }
  return KindName(kind);
}

#pragma mark Table

- (NSInteger)numberOfSectionsInTableView:(__unused UITableView*)tableView {
  return NSInteger(sections_.size());
}

- (NSInteger)tableView:(__unused UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  switch (sections_[size_t(section)]) {
    case TitleSection::kKinds:
      return NSInteger(kinds_.size());
    case TitleSection::kDiscs:
      return NSInteger(title_.discs.size());
    case TitleSection::kActions:
      return NSInteger(actions_.size());
  }
  return 0;
}

- (NSString*)tableView:(__unused UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
  return sections_[size_t(section)] == TitleSection::kDiscs
             ? NSLocalizedString(@"Discs", @"storage: game page section")
             : nil;
}

- (NSString*)tableView:(__unused UITableView*)tableView titleForFooterInSection:(NSInteger)section {
  switch (sections_[size_t(section)]) {
    case TitleSection::kKinds:
      return nil;
    case TitleSection::kDiscs:
      return NSLocalizedString(@"Tap or swipe a disc to delete only that file. The game’s "
                               @"other discs, updates, add-ons and saves stay.",
                               @"storage: game page footer");
    case TitleSection::kActions:
      break;
  }
  if (IsXboxShared(title_)) {
    return NSLocalizedString(@"Shaders rebuild as you play. Deleting the data removes the "
                             @"saves of every original Xbox game.",
                             @"storage: game page footer");
  }
  if (title_.in_library && IsLinkedOnly(title_)) {
    return NSLocalizedString(@"Shaders rebuild as you play. You can take the game out of the "
                             @"library and leave it in its folder, or delete its files from the "
                             @"folder.",
                             @"storage: game page footer");
  }
  return title_.in_library
             ? NSLocalizedString(@"Shaders rebuild as you play. Deleting the game offers to "
                                 @"keep its updates, add-ons and saves.",
                                 @"storage: game page footer")
             : NSLocalizedString(@"Deleting its data removes its updates, add-ons, saves and "
                                 @"shaders.",
                                 @"storage: game page footer");
}

// A disc's label over its file name (two files can carry one label), with what deleting it
// frees.
- (UITableViewCell*)tableView:(UITableView*)tableView discCellForRow:(NSInteger)row {
  static NSString* const kDiscCellId = @"XeniaStorageDiscCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kDiscCellId];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kDiscCellId] autorelease];
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
  }
  const IOSStorageDisc& disc = title_.discs[size_t(row)];
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular);
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  NSString* label = [NSString stringWithUTF8String:disc.label.c_str()];
  if (!label.length) {
    label = NSLocalizedString(@"Disc", @"storage: game page row; a disc without a label");
  }
  NSString* file = DiscFileName(disc);
  if (disc.linked) {
    file = [NSString stringWithFormat:NSLocalizedString(@"Linked folder · %@",
                                                        @"storage: disc row detail; %@ is a "
                                                        @"file name"),
                                      file];
  }
  cell.textLabel.text = label;
  cell.detailTextLabel.text = file;
  cell.imageView.image = XERowSymbolImage(@"opticaldisc", nil);
  NSString* size = disc.bytes ? ByteText(disc.bytes) : nil;
  cell.accessoryView = size ? ValueAccessory(size, NO) : nil;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  XEApplyAccessibility(cell, [NSString stringWithFormat:@"%@, %@", label, file], size,
                       NSLocalizedString(@"Deletes this disc after asking.",
                                         @"storage: disc row accessibility hint"),
                       UIAccessibilityTraitButton);
  return cell;
}

// Asks, then has the launcher delete the one file: it holds the linked folders' access and
// scans the library again.
- (void)confirmDeleteDiscAtPath:(const std::string&)disc_path {
  if (owner_.gameRunning) {
    PresentCloseGameFirst(self);
    return;
  }
  const auto disc =
      std::find_if(title_.discs.begin(), title_.discs.end(),
                   [&disc_path](const IOSStorageDisc& other) { return other.path == disc_path; });
  // Never the last one this way: that is Delete Game.
  if (disc == title_.discs.end() || title_.discs.size() < 2) {
    return;
  }
  NSString* path = [NSString stringWithUTF8String:disc->path.c_str()];
  if (!path.length) {
    return;
  }
  NSString* label = [NSString stringWithUTF8String:disc->label.c_str()];
  NSString* game = TitleName(title_);
  NSString* what = label.length ? [NSString stringWithFormat:NSLocalizedString(
                                                                 @"%1$@ of %2$@",
                                                                 @"storage: alert message; a "
                                                                 @"disc's label, its game"),
                                                             label, game]
                                : game;
  if (disc->bytes) {
    what = [NSString stringWithFormat:@"%@, %@", what, ByteText(disc->bytes)];
  }
  NSString* where =
      disc->linked
          ? NSLocalizedString(@"The file is deleted from its linked folder.",
                              @"storage: alert message")
          : [NSString stringWithFormat:NSLocalizedString(@"The file is deleted from this %@.",
                                                         @"storage: alert message; %@ is the "
                                                         @"device's kind"),
                                       UIDevice.currentDevice.model];
  NSString* message =
      [NSString stringWithFormat:@"%@. %@ %@", what, where,
                                 NSLocalizedString(@"The game’s other discs, updates, add-ons "
                                                   @"and saves stay. This can’t be undone.",
                                                   @"storage: alert message")];
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:[NSString
                                   stringWithFormat:NSLocalizedString(@"Delete “%@”?",
                                                                      @"storage: alert title; "
                                                                      @"%@ is a disc's file name"),
                                                    DiscFileName(*disc)]
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel", @"storage: alert button")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  // The game and the disc by ID and path: the page can change under the alert.
  NSDictionary* request = @{
    XeniaStorageTitleIdKey : @(title_.title_id),
    XeniaStorageGamePathKey : [NSString stringWithUTF8String:title_.game_path.c_str()] ?: @"",
    XeniaStorageDiscPathKey : path,
  };
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Delete Disc", @"storage: alert button")
                                 style:UIAlertActionStyleDestructive
                               handler:^(UIAlertAction* __unused action) {
                                 [[NSNotificationCenter defaultCenter]
                                     postNotificationName:XeniaStorageDeleteDiscRequestNotification
                                                   object:nil
                                                 userInfo:request];
                               }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (UISwipeActionsConfiguration*)tableView:(__unused UITableView*)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (sections_[size_t(indexPath.section)] != TitleSection::kDiscs) {
    return nil;
  }
  const std::string disc_path = title_.discs[size_t(indexPath.row)].path;
  UIContextualAction* remove = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:NSLocalizedString(@"Delete", @"storage: swipe action")
                        handler:^(__unused UIContextualAction* action, __unused UIView* view,
                                  void (^done)(BOOL)) {
                          [self confirmDeleteDiscAtPath:disc_path];
                          done(NO);
                        }];
  remove.image = [UIImage systemImageNamed:@"trash"];
  UISwipeActionsConfiguration* actions =
      [UISwipeActionsConfiguration configurationWithActions:@[ remove ]];
  actions.performsFirstActionWithFullSwipe = NO;
  return actions;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (sections_[size_t(indexPath.section)] == TitleSection::kDiscs) {
    return [self tableView:tableView discCellForRow:indexPath.row];
  }
  static NSString* const kCellId = @"XeniaStorageTitleCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellId];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                   reuseIdentifier:kCellId] autorelease];
  }
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  if (sections_[size_t(indexPath.section)] == TitleSection::kKinds) {
    const IOSStorageKind kind = kinds_[size_t(indexPath.row)];
    cell.textLabel.text = [self titleForKind:kind];
    cell.textLabel.textColor = [XeniaTheme textPrimary];
    cell.detailTextLabel.text =
        kind == IOSStorageKind::kGames && IsLinkedOnly(title_)
            ? (title_.linked_bytes
                   ? [NSString
                         stringWithFormat:NSLocalizedString(@"Linked · %@",
                                                            @"storage: game page row value; %@ "
                                                            @"is the size in the linked folder"),
                                          ByteText(title_.linked_bytes)]
                   : NSLocalizedString(@"In a linked folder", @"storage: game page row value"))
            : ByteText(KindBytes(title_.bytes, kind));
    cell.imageView.image = SwatchImage(KindColor(kind));
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.accessibilityTraits = UIAccessibilityTraitStaticText;
    MoveDetailToAccessory(cell);
    return cell;
  }
  const TitleAction action = actions_[size_t(indexPath.row)];
  NSString* text = nil;
  NSString* symbol = nil;
  BOOL destructive = YES;
  switch (action) {
    case TitleAction::kClearShaders:
      text = NSLocalizedString(@"Clear Shaders", @"storage: game page action");
      symbol = @"sparkles";
      destructive = NO;
      break;
    case TitleAction::kDeleteContent:
      text = NSLocalizedString(@"Delete Updates and Add-ons", @"storage: game page action");
      symbol = @"shippingbox";
      break;
    case TitleAction::kDeleteSaves:
      text = NSLocalizedString(@"Delete Saves", @"storage: game page action");
      symbol = @"externaldrive.badge.xmark";
      break;
    case TitleAction::kDeleteGame:
      text = title_.has_imported_copy
                 ? NSLocalizedString(@"Delete Game…", @"storage: game page action")
                 : NSLocalizedString(@"Remove or Delete…", @"storage: game page action");
      symbol = @"trash";
      break;
    case TitleAction::kDeleteAllData:
      text = NSLocalizedString(@"Delete Its Data", @"storage: game page action");
      symbol = @"trash";
      break;
  }
  UIColor* color = destructive ? [XeniaTheme statusError] : [XeniaTheme accent];
  cell.textLabel.text = text;
  cell.textLabel.textColor = color;
  cell.detailTextLabel.text = nil;
  cell.accessoryView = nil;
  cell.imageView.image = XERowSymbolImage(symbol, color);
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  switch (sections_[size_t(indexPath.section)]) {
    case TitleSection::kKinds:
      return;
    case TitleSection::kDiscs: {
      // A copy: asking can refresh the page.
      const std::string disc_path = title_.discs[size_t(indexPath.row)].path;
      [self confirmDeleteDiscAtPath:disc_path];
      return;
    }
    case TitleSection::kActions:
      break;
  }
  const uint32_t title_id = title_.title_id;
  NSString* name = TitleName(title_);
  switch (actions_[size_t(indexPath.row)]) {
    case TitleAction::kClearShaders:
      [owner_ confirmTitle:[NSString stringWithFormat:NSLocalizedString(
                                                          @"Clear Shaders for %@?",
                                                          @"storage: alert title; %@ is a game"),
                                                      name]
                   message:NSLocalizedString(@"They rebuild as you play, with short pauses the "
                                             @"first time.",
                                             @"storage: alert message")
                    button:NSLocalizedString(@"Clear", @"storage: alert button")
                      from:self
                      work:^BOOL(std::string* error) {
                        std::error_code ec;
                        const bool ok = xe_ios_clear_title_shaders(title_id, nullptr, &ec);
                        *error = ec.message();
                        return ok;
                      }];
      break;
    case TitleAction::kDeleteContent:
      [owner_ confirmTitle:[NSString stringWithFormat:NSLocalizedString(
                                                          @"Delete Updates and Add-ons for %@?",
                                                          @"storage: alert title; %@ is a game"),
                                                      name]
                   message:NSLocalizedString(@"Add them again from Manage Content to use them. "
                                             @"This can’t be undone.",
                                             @"storage: alert message")
                    button:NSLocalizedString(@"Delete", @"storage: alert button")
                      from:self
                      work:^BOOL(std::string* error) {
                        return xe_remove_title_content(title_id, error);
                      }];
      break;
    case TitleAction::kDeleteSaves:
      [owner_ confirmTitle:[NSString stringWithFormat:NSLocalizedString(
                                                          @"Delete Saves for %@?",
                                                          @"storage: alert title; %@ is a game"),
                                                      name]
                   message:NSLocalizedString(@"Every profile’s saves for this game are deleted. "
                                             @"This can’t be undone.",
                                             @"storage: alert message")
                    button:NSLocalizedString(@"Delete Saves", @"storage: alert button")
                      from:self
                      work:^BOOL(std::string* error) {
                        return xe_remove_title_saves(title_id, error);
                      }];
      break;
    case TitleAction::kDeleteGame:
      // The launcher's own sheet, with its choices for updates, add-ons and saves.
      [[NSNotificationCenter defaultCenter]
          postNotificationName:XeniaStorageDeleteGameRequestNotification
                        object:nil
                      userInfo:@{
                        XeniaStorageTitleIdKey : @(title_id),
                        XeniaStorageGamePathKey :
                                [NSString stringWithUTF8String:title_.game_path.c_str()] ?: @"",
                      }];
      break;
    case TitleAction::kDeleteAllData:
      [owner_ confirmTitle:[NSString stringWithFormat:NSLocalizedString(
                                                          @"Delete the Data of %@?",
                                                          @"storage: alert title; %@ is a game"),
                                                      name]
                   message:IsXboxShared(title_)
                               ? NSLocalizedString(@"The saves and shaders of every original "
                                                   @"Xbox game are deleted. This can’t be "
                                                   @"undone.",
                                                   @"storage: alert message")
                               : NSLocalizedString(@"Its updates, add-ons, saves and shaders "
                                                   @"are deleted. This can’t be undone.",
                                                   @"storage: alert message")
                    button:NSLocalizedString(@"Delete", @"storage: alert button")
                      from:self
                      work:^BOOL(std::string* error) {
                        return xe_ios_remove_title_data(title_id, error);
                      }];
      break;
  }
}

@end

#pragma mark - Storage

namespace {
// kLibrary lists the library's Xbox 360 games and kXbox its original Xbox games, with what
// those share.
enum class Section { kKinds, kFree, kOptions, kLibrary, kXbox, kNotInLibrary };
enum class Option { kSort, kSelect };
enum class FreeAction { kLeftovers, kGameData, kShaders };
}  // namespace

@implementation XeniaStorageViewController {
  BOOL game_running_;
  BOOL loaded_;
  BOOL measuring_;
  BOOL measure_again_;
  Measurement measurement_;
  XEStatusHeaderView* header_;
  std::vector<Section> sections_;
  std::vector<FreeAction> free_actions_;
  std::vector<Option> options_;
  std::vector<size_t> library_titles_;
  std::vector<size_t> xbox_titles_;
  std::vector<size_t> other_titles_;
  BOOL has_linked_games_;
  // What the library's games take in linked folders on other drives: not this
  // device's storage, so not in the total.
  uint64_t linked_bytes_;
  // Select mode: rows of the game lists are chosen, then deleted together.
  BOOL selecting_;
  std::set<std::string> selected_;  // SelectionKey of each chosen row
  UIBarButtonItem* select_all_item_;
  UIBarButtonItem* select_done_item_;
  UIButton* delete_button_;  // Not retained: the page's commit button.
}

- (instancetype)initWithGameRunning:(BOOL)game_running {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    game_running_ = game_running;
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [header_ release];
  [select_all_item_ release];
  [select_done_item_ release];
  [super dealloc];
}

- (BOOL)gameRunning {
  return game_running_;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Storage", @"storage: title");
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 52.0;
  header_ = [[XEStatusHeaderView alloc] init];
  [self refreshHeader];
  select_all_item_ = [[UIBarButtonItem alloc]
      initWithTitle:NSLocalizedString(@"Select All", @"storage: header button")
              style:UIBarButtonItemStylePlain
             target:self
             action:@selector(onSelectAllTapped:)];
  select_done_item_ =
      [[UIBarButtonItem alloc] initWithTitle:NSLocalizedString(@"Done", @"storage: header button")
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(onSelectDoneTapped:)];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(storageDidChange:)
                                               name:XeniaStorageDidChangeNotification
                                             object:nil];
  [self measure];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [header_ fitAsHeaderOfTable:self.tableView];
}

- (void)storageDidChange:(NSNotification*)__unused notification {
  [self measure];
}

// Walks the container off the main thread. A request while one runs measures
// again after it, so the last change is always shown.
- (void)measure {
  if (measuring_) {
    measure_again_ = YES;
    return;
  }
  measuring_ = YES;
  [self retain];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    auto measurement = std::make_shared<Measurement>();
    measurement->report = xe_ios_measure_storage();
    NSNumber* free_bytes = nil;
    [[NSURL fileURLWithPath:NSHomeDirectory()]
        getResourceValue:&free_bytes
                  forKey:NSURLVolumeAvailableCapacityForImportantUsageKey
                   error:nil];
    measurement->free_bytes = free_bytes.unsignedLongLongValue;
    dispatch_async(dispatch_get_main_queue(), ^{
      measurement_ = std::move(*measurement);
      loaded_ = YES;
      measuring_ = NO;
      [self rebuild];
      [self refreshHeader];
      if (self.isViewLoaded) {
        [self.tableView reloadData];
      }
      for (UIViewController* page in self.navigationController.viewControllers) {
        if ([page isKindOfClass:[XeniaStorageTitleViewController class]]) {
          [(XeniaStorageTitleViewController*)page applyReport:measurement_.report];
        }
      }
      if (measure_again_) {
        measure_again_ = NO;
        [self measure];
      }
      [self release];
    });
  });
}

- (void)rebuild {
  const IOSStorageReport& report = measurement_.report;
  sections_.clear();
  free_actions_.clear();
  options_.clear();
  library_titles_.clear();
  xbox_titles_.clear();
  other_titles_.clear();
  has_linked_games_ = NO;
  linked_bytes_ = 0;
  if (!loaded_) {
    return;
  }
  sections_.push_back(Section::kKinds);
  if (KindBytes(report.bytes, IOSStorageKind::kLeftovers)) {
    free_actions_.push_back(FreeAction::kLeftovers);
  }
  if (KindBytes(report.bytes, IOSStorageKind::kGameData)) {
    free_actions_.push_back(FreeAction::kGameData);
  }
  if (KindBytes(report.bytes, IOSStorageKind::kShaders)) {
    free_actions_.push_back(FreeAction::kShaders);
  }
  if (!free_actions_.empty()) {
    sections_.push_back(Section::kFree);
  }
  const bool has_xbox_games =
      std::any_of(report.titles.begin(), report.titles.end(), [](const IOSStorageTitle& title) {
        return title.in_library && title.original_xbox;
      });
  for (size_t i = 0; i < report.titles.size(); ++i) {
    const IOSStorageTitle& title = report.titles[i];
    // What original Xbox games share is listed with them while there are any.
    (title.in_library                              ? (title.original_xbox ? xbox_titles_
                                                                          : library_titles_)
     : IsXboxShared(title) && has_xbox_games ? xbox_titles_
                                                   : other_titles_)
        .push_back(i);
    has_linked_games_ |= IsLinkedOnly(report.titles[i]);
    if (report.titles[i].in_library) {
      linked_bytes_ += report.titles[i].linked_bytes -
                       KindBytes(report.titles[i].bytes, IOSStorageKind::kLinked);
    }
  }
  if ([[NSUserDefaults standardUserDefaults] boolForKey:kSortByNameDefaultsKey]) {
    // The report is largest first.
    auto by_name = [&report](size_t a, size_t b) {
      return [TitleName(report.titles[a]) localizedStandardCompare:TitleName(report.titles[b])] ==
             NSOrderedAscending;
    };
    std::stable_sort(library_titles_.begin(), library_titles_.end(), by_name);
    std::stable_sort(xbox_titles_.begin(), xbox_titles_.end(), by_name);
    std::stable_sort(other_titles_.begin(), other_titles_.end(), by_name);
  }
  if ([self selectableCount] > 1) {
    options_ = {Option::kSort, Option::kSelect};
    sections_.push_back(Section::kOptions);
  }
  if (!library_titles_.empty()) {
    sections_.push_back(Section::kLibrary);
  }
  if (!xbox_titles_.empty()) {
    sections_.push_back(Section::kXbox);
  }
  if (!other_titles_.empty()) {
    sections_.push_back(Section::kNotInLibrary);
  }
  // A row stays selected only while it is still listed, in the same section.
  std::set<std::string> still_listed;
  for (const IOSStorageTitle& title : report.titles) {
    const std::string key = SelectionKey(title);
    if (selected_.count(key)) {
      still_listed.insert(key);
    }
  }
  selected_.swap(still_listed);
  if (![self selectableCount]) {
    selecting_ = NO;
  }
  [self refreshSelectChrome];
}

#pragma mark Select

- (size_t)selectableCount {
  return library_titles_.size() + xbox_titles_.size() + other_titles_.size();
}

// The rows of a section that lists games, or nullptr.
- (const std::vector<size_t>*)titlesOfSection:(Section)section {
  switch (section) {
    case Section::kLibrary:
      return &library_titles_;
    case Section::kXbox:
      return &xbox_titles_;
    case Section::kNotInLibrary:
      return &other_titles_;
    default:
      return nullptr;
  }
}

// Select mode starts from its own row above the games; while selecting, Select All and Done
// are in the header and Delete at the bottom.
- (void)refreshSelectChrome {
  if (!self.isViewLoaded || !select_all_item_) {
    return;
  }
  const size_t selectable = [self selectableCount];
  select_all_item_.title = selectable && selected_.size() >= selectable
                               ? NSLocalizedString(@"Deselect All", @"storage: header button")
                               : NSLocalizedString(@"Select All", @"storage: header button");
  // The same items each time, so the header is laid out again only when they change. Done is
  // last, next to ✕.
  XESetSheetActions(self, selecting_ ? @[ select_all_item_, select_done_item_ ] : @[]);
  // The header's buttons take the title's room on a phone.
  self.navigationItem.title = selecting_ ? @"" : self.title;
  if (selecting_ && !delete_button_) {
    delete_button_ = XESetFormCommit(self, @"", self, @selector(onDeleteSelectedTapped:));
    // The page's one commit button, in the colour of what it does.
    delete_button_.configurationUpdateHandler = ^(UIButton* button) {
      UIButtonConfiguration* config = [[button.configuration copy] autorelease];
      const BOOL usable = button.enabled;
      config.background.backgroundColor =
          [[XeniaTheme statusError] colorWithAlphaComponent:usable ? 1.0 : 0.3];
      config.baseForegroundColor = usable ? [UIColor colorWithWhite:0.0 alpha:1.0]
                                          : [UIColor colorWithWhite:1.0 alpha:0.5];
      button.configuration = config;
    };
  }
  if (!delete_button_) {
    return;
  }
  UIButtonConfiguration* config = [[delete_button_.configuration copy] autorelease];
  config.title =
      selected_.empty()
          ? NSLocalizedString(@"Delete", @"storage: delete the selected rows; none selected")
          : [NSString stringWithFormat:NSLocalizedString(@"Delete (%lu)",
                                                         @"storage: delete the selected rows; "
                                                         @"how many"),
                                       (unsigned long)selected_.size()];
  delete_button_.configuration = config;
  delete_button_.enabled = !selected_.empty();
  [delete_button_ setNeedsUpdateConfiguration];
  XESetFormCommitHidden(self, !selecting_);
}

// Leaving Select mode, or entering it, starts with nothing selected.
- (void)setSelecting:(BOOL)selecting {
  selecting_ = selecting;
  selected_.clear();
  [self refreshSelectChrome];
  [self.tableView reloadData];
}

- (void)onSelectDoneTapped:(id)__unused sender {
  [self setSelecting:NO];
}

- (void)onSelectAllTapped:(id)__unused sender {
  const BOOL all = selected_.size() >= [self selectableCount];
  selected_.clear();
  if (!all) {
    for (const IOSStorageTitle& title : measurement_.report.titles) {
      selected_.insert(SelectionKey(title));
    }
  }
  [self refreshSelectChrome];
  [self.tableView reloadData];
}

// A selectable row's leading mark in Select mode.
- (void)applySelected:(BOOL)selected toCell:(UITableViewCell*)cell {
  cell.imageView.image = XERowSymbolImage(selected ? @"checkmark.circle.fill" : @"circle",
                                          selected ? [XeniaTheme accent] : nil);
  cell.accessibilityTraits =
      UIAccessibilityTraitButton | (selected ? UIAccessibilityTraitSelected : 0);
}

// One sheet for everything selected: what goes, how many and how much, and for the library's
// games the choices Delete Game gives. The launcher does the deleting, as for one game.
- (void)onDeleteSelectedTapped:(id)__unused sender {
  if (game_running_) {
    PresentCloseGameFirst(self);
    return;
  }
  const IOSStorageReport& report = measurement_.report;
  // The rows by title ID and path, not by place: the list can sort or measure again while
  // the sheet is up.
  NSMutableArray<NSDictionary*>* entries = [NSMutableArray array];
  unsigned long imported = 0, linked_only = 0, others = 0;
  uint64_t imported_bytes = 0, other_bytes = 0;
  BOOL any_linked = NO, any_content = NO, any_saves = NO;
  for (const IOSStorageTitle& title : report.titles) {
    if (!selected_.count(SelectionKey(title)) || (!title.in_library && !title.title_id)) {
      continue;
    }
    [entries addObject:@{
      XeniaStorageTitleIdKey : @(title.title_id),
      XeniaStorageGamePathKey : [NSString stringWithUTF8String:title.game_path.c_str()] ?: @"",
      XeniaStorageNameKey : TitleName(title),
      XeniaStorageInLibraryKey : @(title.in_library),
    }];
    if (!title.in_library) {
      ++others;
      other_bytes += title.Total();
      continue;
    }
    if (title.has_imported_copy) {
      ++imported;
      imported_bytes += KindBytes(title.bytes, IOSStorageKind::kGames);
    } else {
      ++linked_only;
    }
    any_linked |= title.has_linked_copy;
    if (title.title_id) {
      any_content |= KindBytes(title.bytes, IOSStorageKind::kContent) != 0;
      any_saves |= KindBytes(title.bytes, IOSStorageKind::kSaves) != 0;
    }
  }
  if (!entries.count) {
    return;
  }
  NSString* device = UIDevice.currentDevice.model;
  NSMutableArray<NSString*>* lines = [NSMutableArray array];
  if (imported && others) {
    [lines addObject:[NSString stringWithFormat:NSLocalizedString(@"This frees %@.",
                                                                  @"storage: delete selected; "
                                                                  @"the total size"),
                                                ByteText(imported_bytes + other_bytes)]];
  }
  if (imported == 1) {
    [lines addObject:[NSString
                         stringWithFormat:NSLocalizedString(@"1 game is deleted from this %1$@ "
                                                            @"(%2$@).",
                                                            @"storage: delete selected; the "
                                                            @"device's kind, the game's size"),
                                          device, ByteText(imported_bytes)]];
  } else if (imported) {
    [lines addObject:[NSString
                         stringWithFormat:NSLocalizedString(@"%1$lu games are deleted from this "
                                                            @"%2$@ (%3$@).",
                                                            @"storage: delete selected; how "
                                                            @"many, the device's kind, their "
                                                            @"size"),
                                          imported, device, ByteText(imported_bytes)]];
  }
  if (linked_only == 1) {
    [lines addObject:NSLocalizedString(@"1 game in a linked folder is removed from the library.",
                                       @"storage: delete selected")];
  } else if (linked_only) {
    [lines addObject:[NSString
                         stringWithFormat:NSLocalizedString(@"%lu games in linked folders are "
                                                            @"removed from the library.",
                                                            @"storage: delete selected; how many"),
                                          linked_only]];
  }
  if (any_linked) {
    [lines addObject:NSLocalizedString(@"Files in linked folders stay there unless you choose "
                                       @"to delete them.",
                                       @"storage: delete selected")];
  }
  if (others == 1) {
    [lines addObject:[NSString
                         stringWithFormat:NSLocalizedString(@"The updates, add-ons, saves and "
                                                            @"shaders of 1 game that isn’t in "
                                                            @"your library are deleted (%@).",
                                                            @"storage: delete selected; their "
                                                            @"size"),
                                          ByteText(other_bytes)]];
  } else if (others) {
    [lines addObject:[NSString
                         stringWithFormat:NSLocalizedString(@"The updates, add-ons, saves and "
                                                            @"shaders of %1$lu games that "
                                                            @"aren’t in your library are "
                                                            @"deleted (%2$@).",
                                                            @"storage: delete selected; how "
                                                            @"many, their size"),
                                          others, ByteText(other_bytes)]];
  }
  if (any_content || any_saves) {
    [lines addObject:NSLocalizedString(@"Updates, add-ons and saves of the library’s games are "
                                       @"kept unless you choose otherwise. Achievements stay "
                                       @"on their profile.",
                                       @"storage: delete selected")];
  }
  const BOOL deletes = imported || others;
  [lines addObject:deletes ? NSLocalizedString(@"This can’t be undone.",
                                               @"storage: delete selected")
                           : NSLocalizedString(@"Deleting files from a folder can’t be undone.",
                                               @"storage: delete selected")];
  NSString* title =
      entries.count == 1
          ? NSLocalizedString(@"Delete 1 Item?", @"storage: delete selected; title")
          : [NSString stringWithFormat:NSLocalizedString(@"Delete %lu Items?",
                                                         @"storage: delete selected; title"),
                                       (unsigned long)entries.count];
  UIAlertController* confirm =
      [UIAlertController alertControllerWithTitle:title
                                          message:[lines componentsJoinedByString:@" "]
                                   preferredStyle:UIAlertControllerStyleActionSheet];
  // Reached from the button at the bottom: on iPad the sheet sits mid-screen.
  confirm.popoverPresentationController.sourceView = self.view;
  confirm.popoverPresentationController.sourceRect =
      CGRectMake(CGRectGetMidX(self.view.bounds), CGRectGetMidY(self.view.bounds), 0, 0);
  confirm.popoverPresentationController.permittedArrowDirections = 0;
  void (^add)(NSString*, BOOL, BOOL, BOOL) =
      ^(NSString* action_title, BOOL content, BOOL saves, BOOL linked_files) {
        [confirm addAction:[UIAlertAction
                               actionWithTitle:action_title
                                         style:UIAlertActionStyleDestructive
                                       handler:^(UIAlertAction* __unused action) {
                                         [self setSelecting:NO];
                                         [[NSNotificationCenter defaultCenter]
                                             postNotificationName:
                                                 XeniaStorageDeleteEntriesRequestNotification
                                                           object:nil
                                                         userInfo:@{
                                                           XeniaStorageEntriesKey : entries,
                                                           XeniaStorageDeleteContentKey :
                                                               @(content),
                                                           XeniaStorageDeleteSavesKey : @(saves),
                                                           XeniaStorageDeleteLinkedFilesKey :
                                                               @(linked_files),
                                                         }];
                                       }]];
      };
  add(deletes ? NSLocalizedString(@"Delete", @"storage: delete selected; button")
              : NSLocalizedString(@"Remove from Library", @"Delete game: button"),
      NO, NO, NO);
  if (any_linked) {
    add(NSLocalizedString(@"Also Delete Files from Linked Folders",
                          @"storage: delete selected; button"),
        NO, NO, YES);
  }
  if (any_content) {
    add(NSLocalizedString(@"Also Delete Updates & Add-ons", @"Delete game: button"), YES, NO, NO);
  }
  if (any_saves) {
    add(any_content
            ? NSLocalizedString(@"Also Delete Updates, Add-ons & Saves", @"Delete game: button")
            : NSLocalizedString(@"Also Delete Saves", @"Delete game: button"),
        any_content, YES, NO);
  }
  [confirm addAction:[UIAlertAction
                         actionWithTitle:NSLocalizedString(@"Cancel", @"storage: alert button")
                                   style:UIAlertActionStyleCancel
                                 handler:nil]];
  [self presentViewController:confirm animated:YES completion:nil];
}

// "● 10.3 GB" over the free space left on the device.
- (void)refreshHeader {
  if (!header_) {
    return;
  }
  if (!loaded_) {
    header_.titleLabel.text = NSLocalizedString(@"Measuring…", @"storage: status");
    header_.dot.backgroundColor = [XeniaTheme textMuted];
    header_.detailLabel.text = nil;
  } else {
    header_.titleLabel.text = ByteText(measurement_.report.Total());
    const uint64_t leftovers = KindBytes(measurement_.report.bytes, IOSStorageKind::kLeftovers);
    header_.dot.backgroundColor = leftovers ? [XeniaTheme statusWarning] : [XeniaTheme accent];
    NSString* used = NSLocalizedString(@"Used by XeniOS", @"storage: header detail");
    header_.detailLabel.text =
        measurement_.free_bytes
            ? [NSString
                  stringWithFormat:NSLocalizedString(@"%@ · %@ free on this %@",
                                                     @"storage: header detail; used, free bytes, "
                                                     @"device kind"),
                                   used, ByteText(measurement_.free_bytes),
                                   UIDevice.currentDevice.model]
            : used;
  }
  [self.view setNeedsLayout];
}

- (void)confirmTitle:(NSString*)title
             message:(NSString*)message
              button:(NSString*)button
                from:(UIViewController*)presenter
                work:(BOOL (^)(std::string* error))work {
  if (game_running_) {
    XEPresentOKAlert(presenter, title,
                     NSLocalizedString(@"Close the game first. Its files are in use while it runs.",
                                       @"storage: alert message"));
    return;
  }
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title
                                          message:message
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Cancel", @"storage: alert button")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  BOOL (^work_copy)(std::string*) = [[work copy] autorelease];
  [alert addAction:[UIAlertAction actionWithTitle:button
                                            style:UIAlertActionStyleDestructive
                                          handler:^(UIAlertAction* __unused action) {
                                            [self runWork:work_copy title:title from:presenter];
                                          }]];
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (void)runWork:(BOOL (^)(std::string* error))work
          title:(NSString*)title
           from:(UIViewController*)presenter {
  [self retain];
  [presenter retain];
  BOOL (^work_copy)(std::string*) = [work copy];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    auto error = std::make_shared<std::string>();
    const BOOL ok = work_copy(error.get());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!ok) {
        XELOGW("Storage: {} failed: {}", title.UTF8String, *error);
        XEPresentOKAlert(presenter,
                         NSLocalizedString(@"Couldn’t Free the Space", @"storage: alert title"),
                         error->empty() ? nil : [NSString stringWithUTF8String:error->c_str()]);
      }
      [self measure];
      [work_copy release];
      [presenter release];
      [self release];
    });
  });
}

#pragma mark Table

- (NSInteger)numberOfSectionsInTableView:(__unused UITableView*)tableView {
  return NSInteger(sections_.size());
}

- (NSInteger)tableView:(__unused UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  switch (sections_[size_t(section)]) {
    case Section::kKinds:
      // The bar, each kind that takes space, then the games in linked folders.
      return 1 + [self listedKindCount] + (linked_bytes_ ? 1 : 0);
    case Section::kFree:
      return NSInteger(free_actions_.size());
    case Section::kOptions:
      return NSInteger(options_.size());
    case Section::kLibrary:
    case Section::kXbox:
    case Section::kNotInLibrary:
      return NSInteger([self titlesOfSection:sections_[size_t(section)]]->size());
  }
  return 0;
}

- (NSString*)tableView:(__unused UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
  switch (sections_[size_t(section)]) {
    case Section::kKinds:
    case Section::kOptions:
      return nil;
    case Section::kFree:
      return NSLocalizedString(@"Free Up Space", @"storage: section");
    case Section::kLibrary:
      // Named for the console only when there are games of both.
      return xbox_titles_.empty() ? NSLocalizedString(@"Games", @"storage: section")
                                  : NSLocalizedString(@"Xbox 360 Games", @"storage: section");
    case Section::kXbox:
      return NSLocalizedString(@"Original Xbox Games", @"storage: section");
    case Section::kNotInLibrary:
      return NSLocalizedString(@"Not in Your Library", @"storage: section");
  }
  return nil;
}

- (NSString*)tableView:(__unused UITableView*)tableView titleForFooterInSection:(NSInteger)section {
  switch (sections_[size_t(section)]) {
    case Section::kFree:
      return NSLocalizedString(@"Game data is what games install to the console’s cache "
                               @"drive; they install it again when they need it. Shaders "
                               @"rebuild as you play.",
                               @"storage: footer");
    case Section::kLibrary:
      // One footer for the games, under the last of their sections.
      if (!xbox_titles_.empty()) {
        return nil;
      }
      [[fallthrough]];
    case Section::kXbox:
      return has_linked_games_
                 ? NSLocalizedString(@"A game in a linked folder shows its size there. "
                                     @"Linked folders on this device count in the total "
                                     @"above. Swipe a game to delete it.",
                                     @"storage: footer")
                 : NSLocalizedString(@"Swipe a game to delete it.", @"storage: footer");
    case Section::kNotInLibrary:
      return NSLocalizedString(@"Updates, saves and shaders of games that aren’t in your "
                               @"library.",
                               @"storage: footer");
    default:
      return nil;
  }
}

- (NSInteger)listedKindCount {
  return NSInteger(std::count_if(std::begin(kListedKinds), std::end(kListedKinds),
                                 [self](IOSStorageKind kind) {
                                   return KindBytes(measurement_.report.bytes, kind) != 0;
                                 }));
}

- (IOSStorageKind)kindAtRow:(NSInteger)row {
  NSInteger index = 0;
  for (IOSStorageKind kind : kListedKinds) {
    if (KindBytes(measurement_.report.bytes, kind) && ++index == row) {
      return kind;
    }
  }
  return IOSStorageKind::kOther;
}

- (UITableViewCell*)barCellForTableView:(UITableView*)tableView {
  static NSString* const kBarCellId = @"XeniaStorageBarCell";
  static const NSInteger kBarTag = 0x5354;
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kBarCellId];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kBarCellId] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    XeniaStorageBarView* bar = [[[XeniaStorageBarView alloc] init] autorelease];
    bar.tag = kBarTag;
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    [cell.contentView addSubview:bar];
    UILayoutGuide* margins = cell.contentView.layoutMarginsGuide;
    [NSLayoutConstraint activateConstraints:@[
      [bar.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
      [bar.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
      [bar.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:16],
      [bar.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-16],
    ]];
  }
  [(XeniaStorageBarView*)[cell.contentView viewWithTag:kBarTag] setBytes:measurement_.report.bytes];
  cell.isAccessibilityElement = NO;
  return cell;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  const Section section = sections_[size_t(indexPath.section)];
  if (section == Section::kKinds && indexPath.row == 0) {
    return [self barCellForTableView:tableView];
  }
  static NSString* const kCellId = @"XeniaStorageCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellId];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                   reuseIdentifier:kCellId] autorelease];
    cell.textLabel.numberOfLines = 0;
  }
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.accessoryView = nil;
  cell.detailTextLabel.text = nil;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  const IOSStorageReport& report = measurement_.report;
  switch (section) {
    case Section::kOptions: {
      if (options_[size_t(indexPath.row)] == Option::kSelect) {
        // Says what selecting is for; while selecting, what to do next.
        UIColor* color = selecting_ ? [XeniaTheme textSecondary] : [XeniaTheme accent];
        cell.textLabel.text =
            selecting_ ? NSLocalizedString(@"Tap the items to delete, then Delete",
                                           @"storage: row while selecting")
                       : NSLocalizedString(@"Select Items to Delete", @"storage: action");
        cell.textLabel.textColor = color;
        cell.imageView.image = XERowSymbolImage(@"checkmark.circle", color);
        if (selecting_) {
          cell.selectionStyle = UITableViewCellSelectionStyleNone;
          cell.accessibilityTraits = UIAccessibilityTraitStaticText;
        }
        break;
      }
      cell.textLabel.text = NSLocalizedString(@"Sort By", @"storage: row");
      cell.imageView.image = XERowSymbolImage(@"arrow.up.arrow.down", nil);
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
      UISegmentedControl* order = [[[UISegmentedControl alloc] initWithItems:@[
        NSLocalizedString(@"Size", @"storage: sort order"),
        NSLocalizedString(@"Name", @"storage: sort order"),
      ]] autorelease];
      order.selectedSegmentIndex =
          [[NSUserDefaults standardUserDefaults] boolForKey:kSortByNameDefaultsKey] ? 1 : 0;
      [order addTarget:self
                    action:@selector(onSortOrderChanged:)
          forControlEvents:UIControlEventValueChanged];
      [order sizeToFit];
      cell.accessoryView = order;
      return cell;
    }
    case Section::kKinds: {
      if (indexPath.row > [self listedKindCount]) {
        cell.textLabel.text =
            NSLocalizedString(@"Linked on Other Drives", @"storage: row; not in the total");
        cell.detailTextLabel.text = ByteText(linked_bytes_);
        cell.imageView.image = XERowSymbolImage(@"link", nil);
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.accessibilityTraits = UIAccessibilityTraitStaticText;
        break;
      }
      const IOSStorageKind kind = [self kindAtRow:indexPath.row];
      cell.textLabel.text = KindName(kind);
      cell.detailTextLabel.text = ByteText(KindBytes(report.bytes, kind));
      cell.imageView.image = SwatchImage(KindColor(kind));
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
      cell.accessibilityTraits = UIAccessibilityTraitStaticText;
      break;
    }
    case Section::kFree: {
      const FreeAction action = free_actions_[size_t(indexPath.row)];
      IOSStorageKind kind = IOSStorageKind::kLeftovers;
      NSString* symbol = @"sparkles";
      switch (action) {
        case FreeAction::kLeftovers:
          cell.textLabel.text = NSLocalizedString(@"Clean Up Leftover Files", @"storage: action");
          break;
        case FreeAction::kGameData:
          kind = IOSStorageKind::kGameData;
          symbol = @"internaldrive";
          cell.textLabel.text = NSLocalizedString(@"Clear Game Data", @"storage: action");
          break;
        case FreeAction::kShaders:
          kind = IOSStorageKind::kShaders;
          symbol = @"cube.transparent";
          cell.textLabel.text = NSLocalizedString(@"Clear All Shaders", @"storage: action");
          break;
      }
      // Select mode is for the games below: these wait.
      UIColor* color = selecting_ ? [XeniaTheme textMuted] : [XeniaTheme accent];
      cell.textLabel.textColor = color;
      cell.detailTextLabel.text = ByteText(KindBytes(report.bytes, kind));
      cell.imageView.image = XERowSymbolImage(symbol, color);
      if (selecting_) {
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.accessibilityTraits = UIAccessibilityTraitButton | UIAccessibilityTraitNotEnabled;
      }
      break;
    }
    case Section::kLibrary:
    case Section::kXbox:
    case Section::kNotInLibrary: {
      const std::vector<size_t>& rows = *[self titlesOfSection:section];
      const IOSStorageTitle& title = report.titles[rows[size_t(indexPath.row)]];
      cell.textLabel.text = TitleName(title);
      cell.detailTextLabel.text =
          ByteText(title.Total() + title.linked_bytes -
                   KindBytes(title.bytes, IOSStorageKind::kLinked));
      if (selecting_) {
        // A mark to tick in place of the row's symbol and chevron.
        [self applySelected:selected_.count(SelectionKey(title)) != 0 toCell:cell];
        break;
      }
      cell.imageView.image = XERowSymbolImage(IsXboxShared(title)   ? @"square.stack.3d.up"
                                              : !title.in_library   ? @"questionmark.folder"
                                              : IsLinkedOnly(title) ? @"link"
                                                                    : @"gamecontroller",
                                              nil);
      cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
      break;
    }
  }
  MoveDetailToAccessory(cell);
  return cell;
}

- (void)onSortOrderChanged:(UISegmentedControl*)sender {
  [[NSUserDefaults standardUserDefaults] setBool:sender.selectedSegmentIndex == 1
                                          forKey:kSortByNameDefaultsKey];
  [self rebuild];
  [self.tableView reloadData];
}

// A library game's row swipes to the launcher's delete sheet, as its page's Delete Game does.
- (UISwipeActionsConfiguration*)tableView:(__unused UITableView*)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  const std::vector<size_t>* rows = [self titlesOfSection:sections_[size_t(indexPath.section)]];
  if (selecting_ || !rows) {
    return nil;
  }
  const IOSStorageTitle& title = measurement_.report.titles[(*rows)[size_t(indexPath.row)]];
  if (!title.in_library) {
    return nil;
  }
  NSDictionary* request = @{
    XeniaStorageTitleIdKey : @(title.title_id),
    XeniaStorageGamePathKey : [NSString stringWithUTF8String:title.game_path.c_str()] ?: @"",
  };
  UIContextualAction* remove = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:NSLocalizedString(@"Delete", @"storage: swipe action")
                        handler:^(__unused UIContextualAction* action, __unused UIView* view,
                                  void (^done)(BOOL)) {
                          [[NSNotificationCenter defaultCenter]
                              postNotificationName:XeniaStorageDeleteGameRequestNotification
                                            object:nil
                                          userInfo:request];
                          done(NO);
                        }];
  remove.image = [UIImage systemImageNamed:@"trash"];
  UISwipeActionsConfiguration* actions =
      [UISwipeActionsConfiguration configurationWithActions:@[ remove ]];
  actions.performsFirstActionWithFullSwipe = NO;
  return actions;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  const Section section = sections_[size_t(indexPath.section)];
  const IOSStorageReport& report = measurement_.report;
  const std::vector<size_t>* titles = [self titlesOfSection:section];
  if (selecting_) {
    // Only the game lists select; a tap ticks or clears the row.
    if (titles) {
      const std::string key = SelectionKey(report.titles[(*titles)[size_t(indexPath.row)]]);
      const BOOL selected = selected_.erase(key) == 0;
      if (selected) {
        selected_.insert(key);
      }
      if (UITableViewCell* cell = [tableView cellForRowAtIndexPath:indexPath]) {
        [self applySelected:selected toCell:cell];
      }
      [self refreshSelectChrome];
    }
    return;
  }
  if (titles) {
    XeniaStorageTitleViewController* page = [[[XeniaStorageTitleViewController alloc]
        initWithTitle:report.titles[(*titles)[size_t(indexPath.row)]]
                owner:self] autorelease];
    [self.navigationController pushViewController:page animated:YES];
    return;
  }
  if (section == Section::kOptions && options_[size_t(indexPath.row)] == Option::kSelect) {
    [self setSelecting:YES];
    return;
  }
  if (section != Section::kFree) {
    return;
  }
  switch (free_actions_[size_t(indexPath.row)]) {
    case FreeAction::kLeftovers:
      [self
          confirmTitle:NSLocalizedString(@"Clean Up Leftover Files?", @"storage: alert title")
               message:[NSString stringWithFormat:
                                     NSLocalizedString(
                                         @"%@ of copies Metal left while saving shaders, "
                                         @"temporary files older than a day, and files you "
                                         @"deleted in the Files app that are still in Recently "
                                         @"Deleted. Nothing uses them, and the deleted files "
                                         @"can no longer be recovered.",
                                         @"storage: alert message; %@ is a size"),
                                     ByteText(KindBytes(report.bytes, IOSStorageKind::kLeftovers))]
                button:NSLocalizedString(@"Clean Up", @"storage: alert button")
                  from:self
                  work:^BOOL(std::string* __unused error) {
                    xe_ios_remove_storage_leftovers();
                    return YES;
                  }];
      break;
    case FreeAction::kGameData:
      [self confirmTitle:NSLocalizedString(@"Clear Game Data?", @"storage: alert title")
                 message:NSLocalizedString(@"Games that installed data to the console’s cache "
                                           @"drive install it again, which can take a few "
                                           @"minutes. Saves stay.",
                                           @"storage: alert message")
                  button:NSLocalizedString(@"Clear", @"storage: alert button")
                    from:self
                    work:^BOOL(std::string* error) {
                      return xe_ios_clear_game_data_drives(error);
                    }];
      break;
    case FreeAction::kShaders:
      [self confirmTitle:NSLocalizedString(@"Clear All Shaders?", @"storage: alert title")
                 message:NSLocalizedString(@"They rebuild as you play, with short pauses the "
                                           @"first time in each game.",
                                           @"storage: alert message")
                  button:NSLocalizedString(@"Clear", @"storage: alert button")
                    from:self
                    work:^BOOL(std::string* error) {
                      std::error_code ec;
                      const bool ok = xe_ios_clear_all_shaders(nullptr, &ec, true);
                      *error = ec.message();
                      return ok;
                    }];
      break;
  }
}

@end
