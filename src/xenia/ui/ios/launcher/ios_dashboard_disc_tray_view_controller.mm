/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_dashboard_disc_tray_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

// A longer library gets a search field.
constexpr NSUInteger kSearchableDiscCount = 8;
constexpr CGFloat kIconSize = 44.0;
NSString* const kDiscCellIdentifier = @"disc";

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
  self.navigationItem.leftBarButtonItem = leave_empty;
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
  UIImage* icon = [self iconForDisc:disc];

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
  content.imageProperties.reservedLayoutSize = CGSizeMake(kIconSize, kIconSize);
  content.imageProperties.maximumSize = CGSizeMake(kIconSize, kIconSize);
  content.imageProperties.cornerRadius = icon ? 8.0 : 0.0;
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
