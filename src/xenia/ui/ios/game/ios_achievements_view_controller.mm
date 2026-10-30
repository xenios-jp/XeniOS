/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/game/ios_achievements_view_controller.h"

#include <algorithm>
#include <cmath>
#include <string>

#import "xenia/ui/ios/app/windowed_app_context_ios.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

NSString* ProgressText(const xe::ui::IOSAchievementsSnapshot& snapshot) {
  return [NSString stringWithFormat:@"%u/%u unlocked",
                                    snapshot.achievements_unlocked,
                                    snapshot.achievements_total];
}

NSString* GamerscoreText(const xe::ui::IOSAchievementsSnapshot& snapshot) {
  return [NSString stringWithFormat:@"%u/%u G",
                                    snapshot.gamerscore_earned,
                                    snapshot.gamerscore_total];
}

UIImage* ImageForAchievement(const xe::ui::IOSAchievementEntry& entry, BOOL revealed) {
  if (revealed && !entry.icon_data.empty()) {
    NSData* data = [NSData dataWithBytes:entry.icon_data.data()
                                  length:entry.icon_data.size()];
    UIImage* image = [UIImage imageWithData:data];
    if (image) {
      return image;
    }
  }
  return [UIImage systemImageNamed:entry.unlocked ? @"checkmark.seal.fill" : @"lock.fill"];
}

}  // namespace

@implementation XeniaAchievementsViewController {
  xe::ui::IOSWindowedAppContext* app_context_;
  uint32_t user_index_;
  uint32_t title_id_;
  BOOL show_locked_info_;
  xe::ui::IOSAchievementsSnapshot snapshot_;
  void (^dismissal_handler_)(void);
}

@synthesize dismissalHandler = dismissal_handler_;

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)appContext
                          userIndex:(uint32_t)userIndex
                            titleID:(uint32_t)titleID {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    app_context_ = appContext;
    user_index_ = userIndex;
    title_id_ = titleID;
    show_locked_info_ = NO;
  }
  return self;
}

- (void)dealloc {
  [dismissal_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = @"Achievements";
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 88.0;
  if (@available(iOS 15.0, *)) {
    self.tableView.sectionHeaderTopPadding = 0;
  }

  if (app_context_) {
    snapshot_ = app_context_->LoadAchievementsSnapshot(user_index_, title_id_);
  }
  std::sort(snapshot_.achievements.begin(), snapshot_.achievements.end(),
            [](const xe::ui::IOSAchievementEntry& a,
               const xe::ui::IOSAchievementEntry& b) {
              if (a.unlocked != b.unlocked) {
                return a.unlocked > b.unlocked;
              }
              return a.achievement_id < b.achievement_id;
            });

  self.navigationItem.leftBarButtonItem =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                     target:self
                                                     action:@selector(closeTapped:)] autorelease];
  self.navigationItem.rightBarButtonItem =
      [[[UIBarButtonItem alloc] initWithTitle:@"Show Hidden"
                                        style:UIBarButtonItemStylePlain
                                       target:self
                                       action:@selector(toggleHiddenInfo:)] autorelease];
  self.tableView.tableHeaderView = [self summaryHeaderView];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [self updateHeaderLayout];
}

- (void)updateHeaderLayout {
  UIView* header = self.tableView.tableHeaderView;
  if (!header) {
    return;
  }
  CGFloat width = CGRectGetWidth(self.tableView.bounds);
  CGSize size = [header systemLayoutSizeFittingSize:CGSizeMake(width, 0)
                      withHorizontalFittingPriority:UILayoutPriorityRequired
                            verticalFittingPriority:UILayoutPriorityFittingSizeLevel];
  CGRect frame = header.frame;
  if (fabs(frame.size.height - size.height) <= 0.5 &&
      fabs(frame.size.width - width) <= 0.5) {
    return;
  }
  frame.size.width = width;
  frame.size.height = ceil(size.height);
  header.frame = frame;
  self.tableView.tableHeaderView = header;
}

- (UIView*)summaryHeaderView {
  UIView* container = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 1, 132)] autorelease];
  container.backgroundColor = [UIColor clearColor];

  UIView* card = [[[UIView alloc] init] autorelease];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  card.backgroundColor = [XeniaTheme bgSurface];
  card.layer.cornerRadius = 12.0;
  card.layer.borderWidth = 0.5;
  card.layer.borderColor = [XeniaTheme border].CGColor;
  [container addSubview:card];

  UILabel* title = [[[UILabel alloc] init] autorelease];
  title.translatesAutoresizingMaskIntoConstraints = NO;
  title.textColor = [XeniaTheme textPrimary];
  title.numberOfLines = 2;
  title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
  title.text = snapshot_.title_name.empty()
                   ? [NSString stringWithFormat:@"Title %08X", snapshot_.title_id]
                   : ToNSString(snapshot_.title_name);
  [card addSubview:title];

  UILabel* progress = [[[UILabel alloc] init] autorelease];
  progress.translatesAutoresizingMaskIntoConstraints = NO;
  progress.textColor = [XeniaTheme textSecondary];
  progress.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  progress.text = [NSString stringWithFormat:@"%@  %@", ProgressText(snapshot_),
                                             GamerscoreText(snapshot_)];
  [card addSubview:progress];

  UIProgressView* progress_view =
      [[[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault] autorelease];
  progress_view.translatesAutoresizingMaskIntoConstraints = NO;
  progress_view.progressTintColor = [XeniaTheme accent];
  progress_view.trackTintColor = [XeniaTheme bgSurface2];
  progress_view.progress =
      snapshot_.achievements_total
          ? static_cast<float>(snapshot_.achievements_unlocked) /
                static_cast<float>(snapshot_.achievements_total)
          : 0.0f;
  [card addSubview:progress_view];

  [NSLayoutConstraint activateConstraints:@[
    [card.topAnchor constraintEqualToAnchor:container.topAnchor constant:8.0],
    [card.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:16.0],
    [card.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-16.0],
    [card.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-8.0],

    [title.topAnchor constraintEqualToAnchor:card.topAnchor constant:14.0],
    [title.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:14.0],
    [title.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-14.0],

    [progress.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:8.0],
    [progress.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
    [progress.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],

    [progress_view.topAnchor constraintEqualToAnchor:progress.bottomAnchor constant:12.0],
    [progress_view.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
    [progress_view.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
    [progress_view.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-16.0],
  ]];

  return container;
}

- (void)closeTapped:(id)sender {
  (void)sender;
  UIViewController* dismiss_target = self.navigationController ?: self;
  void (^dismissal_handler)(void) = [dismissal_handler_ copy];
  [dismiss_target dismissViewControllerAnimated:YES
                                     completion:^{
                                       if (dismissal_handler) {
                                         dismissal_handler();
                                       }
                                       [dismissal_handler release];
                                     }];
}

- (void)toggleHiddenInfo:(id)sender {
  show_locked_info_ = !show_locked_info_;
  self.navigationItem.rightBarButtonItem.title =
      show_locked_info_ ? @"Hide Hidden" : @"Show Hidden";
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:0]
                 withRowAnimation:UITableViewRowAnimationAutomatic];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  if (snapshot_.achievements.empty()) {
    return 1;
  }
  return static_cast<NSInteger>(snapshot_.achievements.size());
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaAchievementCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
  }

  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.accessoryView = nil;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.numberOfLines = 0;
  content.secondaryTextProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.secondaryTextProperties.numberOfLines = 0;
  content.imageProperties.maximumSize = CGSizeMake(48.0, 48.0);
  content.imageProperties.cornerRadius = 8.0;
  content.imageToTextPadding = 12.0;

  if (snapshot_.achievements.empty()) {
    content.text = @"No achievements data";
    content.secondaryText = @"This title has not published achievement metadata to the profile.";
    content.image = [UIImage systemImageNamed:@"seal"];
    content.imageProperties.tintColor = [XeniaTheme textMuted];
    cell.contentConfiguration = content;
    cell.isAccessibilityElement = YES;
    cell.accessibilityLabel = content.text;
    cell.accessibilityValue = content.secondaryText;
    cell.accessibilityTraits = UIAccessibilityTraitStaticText;
    return cell;
  }

  const xe::ui::IOSAchievementEntry& entry =
      snapshot_.achievements[static_cast<size_t>(indexPath.row)];
  const BOOL revealed = entry.unlocked || show_locked_info_ || entry.show_unachieved;
  NSString* title = revealed && !entry.title.empty() ? ToNSString(entry.title)
                                                     : @"Secret Achievement";
  std::string description;
  if (entry.unlocked) {
    description = entry.unlocked_description.empty() ? entry.locked_description
                                                     : entry.unlocked_description;
  } else if (revealed) {
    description = entry.locked_description;
  }
  if (description.empty()) {
    description = revealed ? "No description available." : "Unlock to reveal details.";
  }

  content.text = title;
  content.secondaryText = ToNSString(description);
  content.image = ImageForAchievement(entry, revealed);
  if (!revealed || entry.icon_data.empty()) {
    content.imageProperties.tintColor =
        entry.unlocked ? [XeniaTheme accent] : [XeniaTheme textMuted];
  }
  cell.contentConfiguration = content;

  UILabel* score = [[[UILabel alloc] init] autorelease];
  score.text = [NSString stringWithFormat:@"%u G", entry.gamerscore];
  score.textColor = entry.unlocked ? [XeniaTheme accent] : [XeniaTheme textMuted];
  score.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  [score sizeToFit];
  cell.accessoryView = score;
  cell.isAccessibilityElement = YES;
  cell.accessibilityLabel = title;
  cell.accessibilityValue =
      [NSString stringWithFormat:@"%@, %u gamerscore, %@",
                                 entry.unlocked ? @"Unlocked" : @"Locked", entry.gamerscore,
                                 ToNSString(description)];
  cell.accessibilityTraits = UIAccessibilityTraitStaticText;
  return cell;
}

@end
