/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_compatibility_view_controller.h"

#include <cstdint>

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_compat_report_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_compatibility_cells.h"
#import "xenia/ui/ios/launcher/ios_game_compatibility_discussion_controller.h"
#import "xenia/ui/ios/launcher/ios_game_compatibility_hero_view.h"
#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/shared/ios_theme.h"

@interface XeniaGameCompatibilityViewController () <
    XeniaGameCompatibilityDiscussionControllerDelegate>
@end

@implementation XeniaGameCompatibilityViewController {
  uint32_t title_id_;
  NSString* game_title_;
  NSDictionary* compat_info_;
  XeniaGameCompatibilityHeroView* hero_view_;
  XeniaGameCompatibilityDiscussionController* discussion_controller_;
}

- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                     compatData:(NSDictionary*)compat_data {
  self = [super initWithStyle:UITableViewStylePlain];
  if (self) {
    title_id_ = title_id;
    game_title_ = [title copy];
    compat_info_ = [compat_data retain];
    hero_view_ = [[XeniaGameCompatibilityHeroView alloc] initWithTitleID:title_id
                                                                   title:title
                                                             closeTarget:self
                                                             closeAction:@selector(doneTapped:)];
    discussion_controller_ = [[XeniaGameCompatibilityDiscussionController alloc]
        initWithTitleID:title_id
             compatInfo:compat_data];
    self.title = @"Compatibility";
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [hero_view_ hideAndRemoveFromSuperview];
  [game_title_ release];
  [compat_info_ release];
  [hero_view_ release];
  [discussion_controller_ release];
  [super dealloc];
}

- (void)setHeroArtwork:(UIImage*)image {
  [hero_view_ setHeroArtwork:image];
  if ([self isViewLoaded]) {
    [self layoutHeroHeaderIfNeeded];
  }
}

- (NSDictionary*)bestResultSource {
  NSDictionary* preferred_summary = xe_preferred_summary_from_compat_info(compat_info_);
  return preferred_summary ?: [self latestDiscussionReport];
}

- (void)refreshHeroContent {
  [hero_view_ setCompatInfo:compat_info_ summarySource:[self bestResultSource]];
}

- (void)layoutHeroHeaderIfNeeded {
  UIView* host_view = self.navigationController.view ?: self.view.superview ?: self.view;
  [hero_view_ layoutInTableView:self.tableView controllerView:self.view hostView:host_view];
}

- (void)buildHeroHeaderIfNeeded {
  [self refreshHeroContent];
  [hero_view_ buildIfNeededWithTableView:self.tableView controllerView:self.view];
  [self layoutHeroHeaderIfNeeded];
}

- (void)loadHeroArtwork {
  [hero_view_ loadArtworkIfNeeded];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [XeniaTheme bgPrimary];
  self.tableView.backgroundColor = [UIColor clearColor];
  self.tableView.separatorStyle = UITableViewCellSeparatorStyleNone;
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 360.0;
  self.tableView.alwaysBounceVertical = YES;
  if (@available(iOS 11.0, *)) {
    self.tableView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  }
  if (@available(iOS 15.0, *)) {
    self.tableView.sectionHeaderTopPadding = 0;
  }
  self.title = @"";
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onDiscussionDidUpdate:)
                                               name:kXeniaDiscussionDidUpdateNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onCompatDataDidUpdate:)
                                               name:kXeniaCompatDataDidUpdateNotification
                                             object:nil];
  discussion_controller_.delegate = self;
  [self buildHeroHeaderIfNeeded];
  [self loadHeroArtwork];
  [discussion_controller_ loadFromCompatibilityData];
}

- (void)viewSafeAreaInsetsDidChange {
  [super viewSafeAreaInsetsDidChange];
  // Add bottom safe area inset so the Submit Report card isn't clipped
  // behind the home indicator.
  UIEdgeInsets insets = self.tableView.contentInset;
  CGFloat safe_bottom = 0.0;
  if (@available(iOS 11.0, *)) {
    safe_bottom = self.view.safeAreaInsets.bottom;
  }
  if (fabs(insets.bottom - safe_bottom) > 0.5) {
    insets.bottom = safe_bottom;
    self.tableView.contentInset = insets;
  }
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if (![self.traitCollection
          hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    return;
  }
  // The persistent hero header card has its border colour captured as a
  // CGColor, and the table cells re-render their cards on reload from the
  // dynamic XeniaTheme accessors.
  [hero_view_ updateTraitColors];
  [self.tableView reloadData];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self.navigationController setNavigationBarHidden:YES animated:NO];
  [self layoutHeroHeaderIfNeeded];

  id<UIViewControllerTransitionCoordinator> coordinator = self.transitionCoordinator;
  if (coordinator && coordinator.interactive) {
    // Interactive swipe-back: don't show hero yet — wait for completion.
    [hero_view_ setHidden:YES];
    [coordinator notifyWhenInteractionChangesUsingBlock:^(
                     id<UIViewControllerTransitionCoordinatorContext> context) {
      if (context.isCancelled) {
        // Swipe was cancelled — keep hidden, viewWillDisappear will handle.
        return;
      }
      // Swipe committed — show hero now.
      [self layoutHeroHeaderIfNeeded];
      [self->hero_view_ layoutOverlayFrames];
      [self->hero_view_ setHidden:NO];
    }];
  } else {
    // Non-interactive transition (back button, programmatic pop):
    // show immediately.
    [hero_view_ setHidden:NO];
  }
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [self.navigationController setNavigationBarHidden:YES animated:NO];
  [self layoutHeroHeaderIfNeeded];
  [hero_view_ layoutOverlayFrames];
}

- (void)viewWillDisappear:(BOOL)animated {
  [super viewWillDisappear:animated];
  [hero_view_ hideAndRemoveFromSuperview];
  [hero_view_ layoutOverlayFrames];
  [hero_view_ updateGradientFrames];
  [hero_view_ ensureTopGlowAnimation];
  [self.navigationController setNavigationBarHidden:NO animated:NO];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [self layoutHeroHeaderIfNeeded];
}

- (void)doneTapped:(id)__unused sender {
  [hero_view_ hideAndRemoveFromSuperview];
  if (self.navigationController.presentingViewController &&
      self.navigationController.viewControllers.firstObject == self) {
    [self.navigationController dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  [self.navigationController setNavigationBarHidden:NO animated:NO];
  [self.navigationController popViewControllerAnimated:YES];
}

- (void)submitReportTapped:(id)__unused sender {
  [hero_view_ hideAndRemoveFromSuperview];
  XeniaCompatReportViewController* report_controller =
      [[XeniaCompatReportViewController alloc] initWithTitleID:title_id_ title:game_title_];
  [self.navigationController pushViewController:report_controller animated:YES];
  [report_controller release];
}

- (void)onCompatDataDidUpdate:(NSNotification*)notification {
  NSNumber* updated_title_id = notification.userInfo[@"titleId"];
  if (![updated_title_id isKindOfClass:[NSNumber class]] ||
      [updated_title_id unsignedIntValue] != title_id_) {
    return;
  }

  NSDictionary* next_info = xe_dictionary_from_object(notification.userInfo[@"compatInfo"]);
  if (!next_info) {
    NSDictionary* cached_by_title_id = xe_load_cached_compat_data();
    NSString* title_id_string = XEFormatTitleIDHexUpper(title_id_);
    next_info = xe_dictionary_from_object(cached_by_title_id[title_id_string]);
  }
  if (!next_info) {
    return;
  }

  [compat_info_ release];
  compat_info_ = [next_info retain];
  [discussion_controller_ setCompatInfo:next_info];
  [self refreshHeroContent];
  [self.tableView reloadData];
}

- (void)onDiscussionDidUpdate:(NSNotification*)notification {
  [discussion_controller_ handleDiscussionNotification:notification];
}

- (void)compatibilityDiscussionControllerDidUpdate:
    (XeniaGameCompatibilityDiscussionController*)__unused controller {
  [self refreshHeroContent];
  [self.tableView reloadData];
}

- (NSDictionary*)latestDiscussionReport {
  return [discussion_controller_ latestReport];
}

- (void)viewIssueTapped:(id)__unused sender {
  NSString* issue_url = discussion_controller_.issueURL;
  if (!issue_url) {
    return;
  }
  NSURL* url = [NSURL URLWithString:issue_url];
  if (!url) {
    return;
  }
  [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
}

- (void)toggleDiscussionExpansionTapped:(id)__unused sender {
  [discussion_controller_ toggleShowAll];
}

- (void)toggleDiscussionReportExpansionTapped:(UIButton*)sender {
  if (!sender) {
    return;
  }
  [discussion_controller_ toggleReportExpandedAtIndex:sender.tag];
  NSIndexPath* discussion_path = [NSIndexPath indexPathForRow:0 inSection:0];
  [self.tableView reloadRowsAtIndexPaths:@[ discussion_path ]
                        withRowAnimation:UITableViewRowAnimationFade];
}

#pragma mark - UITableViewDataSource

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
    numberOfRowsInSection:(NSInteger)__unused section {
  return 3;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.row == 0) {
    return [XeniaGameCompatibilityCells
        discussionCellWithReports:discussion_controller_.reports
             expandedReportIndexes:discussion_controller_.expandedReportIndexes
                           loading:discussion_controller_.loading
                           showAll:discussion_controller_.showAll
                          issueURL:discussion_controller_.issueURL
                       issueNumber:discussion_controller_.issueNumber
                            target:self
                   viewIssueAction:@selector(viewIssueTapped:)
             toggleExpansionAction:@selector(toggleDiscussionExpansionTapped:)
                toggleReportAction:@selector(toggleDiscussionReportExpansionTapped:)];
  }
  if (indexPath.row == 1) {
    return [XeniaGameCompatibilityCells detailsCellWithCompatInfo:compat_info_
                                                     latestReport:[self latestDiscussionReport]];
  }
  return [XeniaGameCompatibilityCells ctaCellWithTarget:self
                                           submitAction:@selector(submitReportTapped:)];
}

@end
