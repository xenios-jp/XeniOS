/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_dashboard_disc_tray_view_controller.h"

#include <algorithm>
#include <cmath>

#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

// A longer library gets a search field.
constexpr NSUInteger kSearchableDiscCount = 8;
constexpr CGFloat kIconSize = 44.0;
// Box art is as wide as an icon, and taller.
constexpr CGFloat kCoverHeight = 60.0;
NSString* const kDiscCellIdentifier = @"disc";

// |cover| whole, at the size a row shows it.
UIImage* DrawCover(UIImage* cover, UIGraphicsImageRendererFormat* format) {
  if (!cover || cover.size.width <= 0.0 || cover.size.height <= 0.0) {
    return nil;
  }
  const CGFloat scale = std::min(kIconSize / cover.size.width, kCoverHeight / cover.size.height);
  const CGSize size =
      CGSizeMake(std::round(cover.size.width * scale), std::round(cover.size.height * scale));
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:size format:format] autorelease];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused context) {
    [cover drawInRect:CGRectMake(0.0, 0.0, size.width, size.height)];
  }];
}

}  // namespace

@implementation XeniaDashboardDisc

- (void)dealloc {
  [_title release];
  [_detail release];
  [_path release];
  [_iconPath release];
  [super dealloc];
}

@end

@interface XeniaDashboardDiscTrayViewController () <UISearchResultsUpdating>
@end

@implementation XeniaDashboardDiscTrayViewController {
  NSArray<XeniaDashboardDisc*>* discs_;
  NSArray<XeniaDashboardDisc*>* shown_discs_;
  // Icon path to its image, or NSNull for one that didn't load.
  NSMutableDictionary<NSString*, id>* icons_;
  // Title id to its cover, drawn small, or NSNull for a game without one.
  NSMutableDictionary<NSNumber*, id>* covers_;
  NSMutableSet<NSNumber*>* loading_covers_;
  void (^completion_)(NSString* path);
  UISearchController* search_controller_;
}

- (instancetype)initWithDiscs:(NSArray<XeniaDashboardDisc*>*)discs
                   completion:(void (^)(NSString* path))completion {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    discs_ = [discs copy];
    shown_discs_ = [discs_ retain];
    icons_ = [[NSMutableDictionary alloc] init];
    covers_ = [[NSMutableDictionary alloc] init];
    loading_covers_ = [[NSMutableSet alloc] init];
    completion_ = [completion copy];
    self.title = NSLocalizedString(@"Insert Disc", @"dashboard disc tray: title");
  }
  return self;
}

- (void)dealloc {
  // Whoever waits for the answer gets one, even from a sheet that never showed.
  [self answerEmpty];
  search_controller_.searchResultsUpdater = nil;
  [search_controller_ release];
  [discs_ release];
  [shown_discs_ release];
  [icons_ release];
  [covers_ release];
  [loading_covers_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  UIBarButtonItem* leave_empty = [[[UIBarButtonItem alloc]
      initWithTitle:NSLocalizedString(@"Leave Empty",
                                      @"dashboard disc tray: button, closes the tray with no disc")
              style:UIBarButtonItemStylePlain
             target:self
             action:@selector(leaveEmptyTapped:)] autorelease];
  leave_empty.accessibilityHint =
      NSLocalizedString(@"Closes the tray with no disc in the drive.",
                        @"VoiceOver hint for the disc tray's Leave Empty button");
  XESetSheetActions(self, @[ leave_empty ]);
  [self.tableView registerClass:[UITableViewCell class] forCellReuseIdentifier:kDiscCellIdentifier];

  if (discs_.count > kSearchableDiscCount) {
    search_controller_ = [[UISearchController alloc] initWithSearchResultsController:nil];
    search_controller_.searchResultsUpdater = self;
    search_controller_.obscuresBackgroundDuringPresentation = NO;
    search_controller_.searchBar.placeholder =
        NSLocalizedString(@"Search Games", @"dashboard disc tray: search field placeholder");
    self.navigationItem.searchController = search_controller_;
    self.navigationItem.hidesSearchBarWhenScrolling = NO;
    self.definesPresentationContext = YES;
  }
  [self updateEmptyState];
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  // Swiped away, B on a controller, or the game ending under it.
  if (self.navigationController.isBeingDismissed || self.isBeingDismissed) {
    [self answerEmpty];
  }
}

#pragma mark - Answering

- (void)answerWithPath:(NSString*)path {
  if (!completion_) {
    return;
  }
  void (^completion)(NSString*) = completion_;
  completion_ = nil;
  completion(path);
  [completion release];
}

- (void)answerEmpty {
  [self answerWithPath:nil];
}

- (void)dismissTray {
  UIViewController* sheet = self.navigationController ?: self;
  [sheet.presentingViewController dismissViewControllerAnimated:YES completion:nil];
}

- (void)leaveEmptyTapped:(id)__unused sender {
  [self answerEmpty];
  [self dismissTray];
}

#pragma mark - Contents

- (UIImage*)iconForDisc:(XeniaDashboardDisc*)disc {
  if (!disc.iconPath.length) {
    return nil;
  }
  id icon = icons_[disc.iconPath];
  if (!icon) {
    icon = [UIImage imageWithContentsOfFile:disc.iconPath] ?: [NSNull null];
    icons_[disc.iconPath] = icon;
  }
  return [icon isKindOfClass:[UIImage class]] ? icon : nil;
}

// The cover the library fetched for the disc's game, drawn off the main
// thread: nil until it is, and for a game without one.
- (UIImage*)coverForDisc:(XeniaDashboardDisc*)disc {
  if (!disc.titleId) {
    return nil;
  }
  NSNumber* key = @(disc.titleId);
  id cover = covers_[key];
  if (cover) {
    return [cover isKindOfClass:[UIImage class]] ? cover : nil;
  }
  if ([loading_covers_ containsObject:key]) {
    return nil;
  }
  [loading_covers_ addObject:key];
  const uint32_t title_id = disc.titleId;
  UIGraphicsImageRendererFormat* format =
      [UIGraphicsImageRendererFormat formatForTraitCollection:self.traitCollection];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    UIImage* drawn = DrawCover(xe_cached_game_art(title_id), format);
    dispatch_async(dispatch_get_main_queue(), ^{
      [loading_covers_ removeObject:key];
      covers_[key] = drawn ?: [NSNull null];
      if (drawn) {
        [self reconfigureDiscsWithTitleId:title_id];
      }
    });
  });
  return nil;
}

- (void)reconfigureDiscsWithTitleId:(uint32_t)title_id {
  NSMutableArray<NSIndexPath*>* rows = [NSMutableArray array];
  [shown_discs_ enumerateObjectsUsingBlock:^(XeniaDashboardDisc* disc, NSUInteger index,
                                             BOOL* __unused stop) {
    if (disc.titleId == title_id) {
      [rows addObject:[NSIndexPath indexPathForRow:static_cast<NSInteger>(index) inSection:0]];
    }
  }];
  if (rows.count) {
    [self.tableView reconfigureRowsAtIndexPaths:rows];
  }
}

- (void)updateEmptyState {
  if (shown_discs_.count) {
    self.contentUnavailableConfiguration = nil;
    return;
  }
  if (discs_.count) {
    self.contentUnavailableConfiguration = [UIContentUnavailableConfiguration searchConfiguration];
    return;
  }
  UIContentUnavailableConfiguration* empty = [UIContentUnavailableConfiguration emptyConfiguration];
  empty.image = [UIImage systemImageNamed:@"opticaldisc"];
  empty.text = NSLocalizedString(@"No Games", @"dashboard disc tray: empty library title");
  empty.secondaryText = NSLocalizedString(@"Add games to the library to put them in the drive.",
                                          @"dashboard disc tray: empty library text");
  self.contentUnavailableConfiguration = empty;
}

- (void)updateSearchResultsForSearchController:(UISearchController*)search_controller {
  NSString* query = [search_controller.searchBar.text
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  NSArray<XeniaDashboardDisc*>* shown = discs_;
  if (query.length) {
    NSMutableArray<XeniaDashboardDisc*>* matches = [NSMutableArray array];
    for (XeniaDashboardDisc* disc in discs_) {
      if ([disc.title localizedStandardContainsString:query]) {
        [matches addObject:disc];
      }
    }
    shown = matches;
  }
  [shown retain];
  [shown_discs_ release];
  shown_discs_ = shown;
  [self.tableView reloadData];
  [self updateEmptyState];
}

#pragma mark - UITableViewDataSource

- (NSInteger)tableView:(UITableView*)__unused tableView
    numberOfRowsInSection:(NSInteger)__unused section {
  return static_cast<NSInteger>(shown_discs_.count);
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kDiscCellIdentifier
                                                          forIndexPath:indexPath];
  XeniaDashboardDisc* disc = shown_discs_[static_cast<NSUInteger>(indexPath.row)];
  UIImage* cover = [self coverForDisc:disc];
  UIImage* icon = cover ?: [self iconForDisc:disc];

  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = disc.title;
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  content.secondaryText = disc.detail.length ? disc.detail : nil;
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.secondaryTextProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
  content.textToSecondaryTextVerticalPadding = 2.0;
  content.image = icon ?: [UIImage systemImageNamed:@"opticaldisc"];
  content.imageProperties.tintColor = [XeniaTheme textSecondary];
  // Every row has room for a cover, so one arriving doesn't move the rows.
  content.imageProperties.reservedLayoutSize = CGSizeMake(kIconSize, kCoverHeight);
  content.imageProperties.maximumSize = CGSizeMake(kIconSize, cover ? kCoverHeight : kIconSize);
  content.imageProperties.cornerRadius = cover ? 4.0 : (icon ? 8.0 : 0.0);
  content.imageToTextPadding = 12.0;
  cell.contentConfiguration = content;
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  cell.accessibilityHint = NSLocalizedString(@"Puts this disc in the drive.",
                                             @"VoiceOver hint for a disc in the disc tray");
  return cell;
}

#pragma mark - UITableViewDelegate

- (void)tableView:(UITableView*)__unused tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaDashboardDisc* disc = shown_discs_[static_cast<NSUInteger>(indexPath.row)];
  [self answerWithPath:disc.path];
  [self dismissTray];
}

@end
