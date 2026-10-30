/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_layout_library_view_ios.h"

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

@implementation XeniaTouchLayoutLibraryItem

@synthesize localID = _localID;
@synthesize displayName = _displayName;
@synthesize author = _author;
@synthesize official = _official;
@synthesize thumbnail = _thumbnail;
@synthesize isDefaultForCurrentTitle = _isDefaultForCurrentTitle;
@synthesize isDefaultForAllGames = _isDefaultForAllGames;
@synthesize isFavorite = _isFavorite;

- (void)dealloc {
  [_localID release];
  [_displayName release];
  [_author release];
  [_thumbnail release];
  [super dealloc];
}

@end

@implementation XeniaTouchLayoutLibraryRowCell {
  UILabel* overlayTitleLabel_;
  UILabel* overlaySubtitleLabel_;
  UIImageView* overlayAccessoryView_;
  UIImageView* thumbnailImageView_;
  UIImageView* defaultBadgeView_;
  BOOL showsDisclosure_;
  BOOL showsCheckmark_;
  BOOL showsDefaultBadge_;
}

- (instancetype)initWithStyle:(UITableViewCellStyle)style
              reuseIdentifier:(NSString*)reuseIdentifier {
  if (!(self = [super initWithStyle:style reuseIdentifier:reuseIdentifier])) {
    return nil;
  }

  self.backgroundColor = [UIColor clearColor];
  self.contentView.backgroundColor = [UIColor clearColor];

  UIView* card_view = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  card_view.backgroundColor =
      [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacitySubtle]];
  card_view.layer.cornerRadius = XeniaRadiusLg;
  card_view.layer.masksToBounds = YES;
  card_view.tag = 1;
  [self.contentView addSubview:card_view];

  UIView* selected_background =
      [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  selected_background.backgroundColor = [UIColor clearColor];
  UIView* selected_card =
      [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  selected_card.backgroundColor =
      [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacitySoft]];
  selected_card.layer.cornerRadius = XeniaRadiusLg;
  selected_card.layer.masksToBounds = YES;
  selected_card.tag = 1;
  [selected_background addSubview:selected_card];
  self.selectedBackgroundView = selected_background;

  overlayTitleLabel_ = [[UILabel alloc] initWithFrame:CGRectZero];
  overlayTitleLabel_.backgroundColor = [UIColor clearColor];
  xe_apply_label_font(overlayTitleLabel_, UIFontTextStyleSubheadline, 15.0,
                      UIFontWeightSemibold);
  overlayTitleLabel_.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.96];
  overlayTitleLabel_.numberOfLines = 1;
  [card_view addSubview:overlayTitleLabel_];

  overlaySubtitleLabel_ = [[UILabel alloc] initWithFrame:CGRectZero];
  overlaySubtitleLabel_.backgroundColor = [UIColor clearColor];
  xe_apply_label_font(overlaySubtitleLabel_, UIFontTextStyleCaption1, 12.0,
                      UIFontWeightRegular);
  overlaySubtitleLabel_.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.64];
  overlaySubtitleLabel_.numberOfLines = 1;
  overlaySubtitleLabel_.lineBreakMode = NSLineBreakByTruncatingTail;
  [card_view addSubview:overlaySubtitleLabel_];

  overlayAccessoryView_ = [[UIImageView alloc] initWithFrame:CGRectZero];
  overlayAccessoryView_.contentMode = UIViewContentModeScaleAspectFit;
  overlayAccessoryView_.tintColor =
      [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacityHeavy]];
  [card_view addSubview:overlayAccessoryView_];

  thumbnailImageView_ = [[UIImageView alloc] initWithFrame:CGRectZero];
  thumbnailImageView_.contentMode = UIViewContentModeScaleAspectFit;
  thumbnailImageView_.layer.cornerRadius = XeniaRadiusMd;
  thumbnailImageView_.layer.masksToBounds = YES;
  thumbnailImageView_.backgroundColor =
      [[UIColor blackColor] colorWithAlphaComponent:[XeniaTheme opacityStrong]];
  thumbnailImageView_.hidden = YES;
  [card_view addSubview:thumbnailImageView_];

  defaultBadgeView_ = [[UIImageView alloc] initWithFrame:CGRectZero];
  defaultBadgeView_.image = [UIImage systemImageNamed:@"star.fill"];
  defaultBadgeView_.tintColor =
      [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.95];
  defaultBadgeView_.contentMode = UIViewContentModeScaleAspectFit;
  defaultBadgeView_.hidden = YES;
  [card_view addSubview:defaultBadgeView_];
  return self;
}

- (void)dealloc {
  [defaultBadgeView_ release];
  [thumbnailImageView_ release];
  [overlayAccessoryView_ release];
  [overlaySubtitleLabel_ release];
  [overlayTitleLabel_ release];
  [super dealloc];
}

- (UILabel*)overlayTitleLabel {
  return overlayTitleLabel_;
}

- (UILabel*)overlaySubtitleLabel {
  return overlaySubtitleLabel_;
}

- (void)setShowsDisclosure:(BOOL)showsDisclosure {
  showsDisclosure_ = showsDisclosure;
  [self setNeedsLayout];
}

- (void)setShowsCheckmark:(BOOL)showsCheckmark {
  showsCheckmark_ = showsCheckmark;
  [self setNeedsLayout];
}

- (void)setThumbnailImage:(UIImage*)thumbnail {
  thumbnailImageView_.image = thumbnail;
  thumbnailImageView_.hidden = thumbnail == nil;
  [self setNeedsLayout];
}

- (void)setShowsDefaultBadge:(BOOL)showsDefaultBadge {
  showsDefaultBadge_ = showsDefaultBadge;
  defaultBadgeView_.hidden = !showsDefaultBadge;
  [self setNeedsLayout];
}

- (void)layoutSubviews {
  [super layoutSubviews];

  CGRect card_frame = CGRectInset(self.bounds, 0.0f, 4.0f);
  UIView* card_view = [self.contentView viewWithTag:1];
  card_view.frame = card_frame;
  self.selectedBackgroundView.frame = self.bounds;
  UIView* selected_card = [self.selectedBackgroundView viewWithTag:1];
  selected_card.frame = card_frame;

  const CGFloat horizontal_inset = 16.0f;
  const CGFloat accessory_size = 18.0f;
  const CGFloat accessory_gap = 12.0f;
  const CGFloat thumbnail_width = 60.0f;
  const CGFloat thumbnail_height = 34.0f;
  const CGFloat thumbnail_gap = 12.0f;
  const CGFloat default_badge_size = 14.0f;
  const CGFloat default_badge_gap = 6.0f;

  const BOOL has_thumbnail =
      !thumbnailImageView_.hidden && thumbnailImageView_.image != nil;
  const BOOL shows_accessory = showsDisclosure_ || showsCheckmark_;
  const CGFloat leading_inset =
      horizontal_inset +
      (has_thumbnail ? (thumbnail_width + thumbnail_gap) : 0.0f);
  const CGFloat accessory_area_width =
      shows_accessory ? (accessory_size + accessory_gap) : 0.0f;
  const CGFloat default_badge_area_width =
      showsDefaultBadge_ ? (default_badge_size + default_badge_gap) : 0.0f;
  const CGFloat text_width =
      MAX(CGRectGetWidth(card_frame) - leading_inset - horizontal_inset -
              accessory_area_width - default_badge_area_width,
          0.0f);

  if (has_thumbnail) {
    thumbnailImageView_.frame =
        CGRectMake(horizontal_inset,
                   (CGRectGetHeight(card_frame) - thumbnail_height) * 0.5f,
                   thumbnail_width, thumbnail_height);
  }

  overlayTitleLabel_.frame =
      CGRectMake(leading_inset, 8.0f, text_width, 20.0f);
  overlaySubtitleLabel_.frame =
      CGRectMake(leading_inset, 28.0f, text_width, 16.0f);

  if (showsDefaultBadge_) {
    const CGFloat badge_x =
        leading_inset + text_width + default_badge_gap * 0.5f;
    defaultBadgeView_.frame =
        CGRectMake(badge_x, 8.0f + (20.0f - default_badge_size) * 0.5f,
                   default_badge_size, default_badge_size);
  }

  overlayAccessoryView_.hidden = !shows_accessory;
  if (showsDisclosure_) {
    overlayAccessoryView_.image = [UIImage systemImageNamed:@"chevron.right"];
    overlayAccessoryView_.tintColor =
        [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacityHeavy]];
  } else if (showsCheckmark_) {
    overlayAccessoryView_.image = [UIImage systemImageNamed:@"checkmark"];
    overlayAccessoryView_.tintColor =
        [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.95];
  }
  overlayAccessoryView_.frame = CGRectMake(
      CGRectGetWidth(card_frame) - horizontal_inset - accessory_size,
      (CGRectGetHeight(card_frame) - accessory_size) * 0.5f, accessory_size,
      accessory_size);
}

@end
