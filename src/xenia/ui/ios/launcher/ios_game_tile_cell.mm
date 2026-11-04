/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_tile_cell.h"

@interface XeniaGameTileCell ()
@property(nonatomic, strong) UIView* footerView;
@property(nonatomic, strong) UIStackView* metadataStackView;
@end

namespace {
// The cover's corners, as in the mockups' grid.
constexpr CGFloat kCoverRadius = 8.0;
}  // namespace

@implementation XeniaGameTileCell

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) {
    return nil;
  }
  // The cover on its own, with the title and status under it on the page, like
  // the Games app's library: no card behind the text.
  self.backgroundColor = [UIColor clearColor];
  self.contentView.backgroundColor = [UIColor clearColor];
  self.isAccessibilityElement = YES;
  self.accessibilityTraits = UIAccessibilityTraitButton;
  // Pointer: lift the tile under a trackpad or mouse pointer on iPad.
  self.hoverStyle = [UIHoverStyle styleWithEffect:[UIHoverLiftEffect effect]
                                            shape:[UIShape rectShapeWithCornerRadius:kCoverRadius]];

  // `cardView` is the cover: what a press dims and a long press lifts.
  self.cardView = [[UIView alloc] init];
  self.cardView.translatesAutoresizingMaskIntoConstraints = NO;
  self.cardView.backgroundColor = [XeniaTheme bgSurface2];
  self.cardView.layer.cornerRadius = kCoverRadius;
  self.cardView.layer.cornerCurve = kCACornerCurveContinuous;
  self.cardView.clipsToBounds = YES;
  [self.contentView addSubview:self.cardView];

  self.iconView = [[UIImageView alloc] init];
  self.iconView.translatesAutoresizingMaskIntoConstraints = NO;
  self.iconView.contentMode = UIViewContentModeScaleAspectFill;
  self.iconView.clipsToBounds = YES;
  [self.cardView addSubview:self.iconView];

  // A hairline inside the cover's edge, so dark art keeps its shape on black.
  self.footerView = [[UIView alloc] init];
  self.footerView.translatesAutoresizingMaskIntoConstraints = NO;
  self.footerView.userInteractionEnabled = NO;
  self.footerView.layer.cornerRadius = kCoverRadius;
  self.footerView.layer.cornerCurve = kCACornerCurveContinuous;
  self.footerView.layer.borderWidth = 0.5;
  [self.cardView addSubview:self.footerView];

  self.contentTypePill = [[XeniaPaddedLabel alloc] init];
  self.contentTypePill.translatesAutoresizingMaskIntoConstraints = NO;
  self.contentTypePill.padding = UIEdgeInsetsZero;
  self.contentTypePill.textAlignment = NSTextAlignmentLeft;
  xe_apply_label_font(self.contentTypePill, UIFontTextStyleCaption1, 12.0, UIFontWeightRegular);
  self.contentTypePill.textColor = [XeniaTheme textMuted];
  self.contentTypePill.backgroundColor = [UIColor clearColor];
  self.contentTypePill.hidden = YES;
  [self.contentTypePill setContentHuggingPriority:UILayoutPriorityRequired
                                          forAxis:UILayoutConstraintAxisHorizontal];
  // The tag ("External") shows only on a tile with no status; -layoutSubviews hides it otherwise.
  self.contentTypePill.lineBreakMode = NSLineBreakByTruncatingTail;
  [self.contentTypePill setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                                        forAxis:UILayoutConstraintAxisHorizontal];

  self.titleLabel = [[UILabel alloc] init];
  self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  xe_apply_label_font(self.titleLabel, UIFontTextStyleFootnote, 13.5, UIFontWeightSemibold);
  self.titleLabel.textColor = [XeniaTheme textPrimary];
  self.titleLabel.textAlignment = NSTextAlignmentLeft;
  // Two lines, as on Apple TV, so "Call of Duty: Black Ops" reads whole.
  self.titleLabel.numberOfLines = 2;
  self.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
  self.titleLabel.allowsDefaultTighteningForTruncation = YES;
  [self.titleLabel setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                   forAxis:UILayoutConstraintAxisVertical];
  [self.contentView addSubview:self.titleLabel];

  self.compatPill = [[XeniaPaddedLabel alloc] init];
  self.compatPill.translatesAutoresizingMaskIntoConstraints = NO;
  self.compatPill.padding = UIEdgeInsetsZero;
  self.compatPill.textAlignment = NSTextAlignmentLeft;
  self.compatPill.backgroundColor = [UIColor clearColor];
  xe_apply_label_font(self.compatPill, UIFontTextStyleCaption1, 12.0, UIFontWeightRegular);
  self.compatPill.hidden = YES;
  [self.compatPill setContentHuggingPriority:UILayoutPriorityRequired
                                     forAxis:UILayoutConstraintAxisHorizontal];
  [self.compatPill setContentCompressionResistancePriority:UILayoutPriorityDefaultHigh + 10
                                                   forAxis:UILayoutConstraintAxisHorizontal];

  self.metadataStackView =
      [[UIStackView alloc] initWithArrangedSubviews:@[ self.compatPill, self.contentTypePill ]];
  self.metadataStackView.translatesAutoresizingMaskIntoConstraints = NO;
  self.metadataStackView.axis = UILayoutConstraintAxisHorizontal;
  self.metadataStackView.alignment = UIStackViewAlignmentCenter;
  self.metadataStackView.spacing = 8.0;
  [self.metadataStackView setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                                          forAxis:UILayoutConstraintAxisHorizontal];
  [self.contentView addSubview:self.metadataStackView];

  self.discBadge = [[XeniaPaddedLabel alloc] init];
  self.discBadge.translatesAutoresizingMaskIntoConstraints = NO;
  self.discBadge.padding = UIEdgeInsetsMake(2.0, 7.0, 2.0, 7.0);
  xe_apply_label_font(self.discBadge, UIFontTextStyleCaption2, 11.0, UIFontWeightSemibold);
  self.discBadge.textColor = UIColor.whiteColor;
  self.discBadge.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.62];
  self.discBadge.layer.cornerCurve = kCACornerCurveContinuous;
  self.discBadge.layer.masksToBounds = YES;
  self.discBadge.hidden = YES;
  [self.cardView addSubview:self.discBadge];

  [self updateConfigurationUsingState:self.configurationState];

  [NSLayoutConstraint activateConstraints:@[
    [self.cardView.topAnchor constraintEqualToAnchor:self.contentView.topAnchor],
    [self.cardView.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor],
    [self.cardView.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
    [self.cardView.heightAnchor constraintEqualToAnchor:self.cardView.widthAnchor
                                             multiplier:300.0 / 219.0],
    [self.iconView.topAnchor constraintEqualToAnchor:self.cardView.topAnchor],
    [self.iconView.leadingAnchor constraintEqualToAnchor:self.cardView.leadingAnchor],
    [self.iconView.trailingAnchor constraintEqualToAnchor:self.cardView.trailingAnchor],
    [self.iconView.bottomAnchor constraintEqualToAnchor:self.cardView.bottomAnchor],
    [self.footerView.topAnchor constraintEqualToAnchor:self.cardView.topAnchor],
    [self.footerView.leadingAnchor constraintEqualToAnchor:self.cardView.leadingAnchor],
    [self.footerView.trailingAnchor constraintEqualToAnchor:self.cardView.trailingAnchor],
    [self.footerView.bottomAnchor constraintEqualToAnchor:self.cardView.bottomAnchor],
    [self.titleLabel.topAnchor constraintEqualToAnchor:self.cardView.bottomAnchor constant:7.0],
    [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor],
    [self.titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.contentView.trailingAnchor],
    [self.metadataStackView.topAnchor constraintEqualToAnchor:self.titleLabel.bottomAnchor
                                                     constant:1.0],
    [self.metadataStackView.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor],
    [self.metadataStackView.trailingAnchor
        constraintLessThanOrEqualToAnchor:self.contentView.trailingAnchor],
    [self.discBadge.topAnchor constraintEqualToAnchor:self.cardView.topAnchor constant:6.0],
    [self.discBadge.trailingAnchor constraintEqualToAnchor:self.cardView.trailingAnchor
                                                  constant:-6.0],
  ]];
  return self;
}

- (void)prepareForReuse {
  [super prepareForReuse];
  self.iconView.image = nil;
  self.titleLabel.text = @"";
  self.contentTypePill.text = @"";
  self.contentTypePill.hidden = YES;
  self.contentTypePill.backgroundColor = [UIColor clearColor];
  self.contentTypePill.layer.borderColor = [UIColor clearColor].CGColor;
  self.compatPill.text = @"";
  self.compatPill.hidden = YES;
  self.compatPill.backgroundColor = [UIColor clearColor];
  self.compatPill.layer.borderColor = [UIColor clearColor].CGColor;
  self.discBadge.text = @"";
  self.discBadge.hidden = YES;
  self.transform = CGAffineTransformIdentity;
  self.contentView.transform = CGAffineTransformIdentity;
  self.accessibilityLabel = nil;
  self.accessibilityValue = nil;
  self.representedTitleId = 0;
  _controllerFocused = NO;
  // Setting _controllerFocused directly bypassed -setControllerFocused:, so
  // request the configuration pass by hand to keep recycled cells looking
  // identical to fresh ones.
  [self setNeedsUpdateConfiguration];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  // One line under the title: the status when there is one, otherwise the tag.
  const BOOL show_tag = self.contentTypePill.text.length && self.compatPill.hidden;
  if (self.contentTypePill.hidden == show_tag) {
    self.contentTypePill.hidden = !show_tag;
  }
  self.discBadge.layer.cornerRadius = CGRectGetHeight(self.discBadge.bounds) / 2.0;
}

- (void)setControllerFocused:(BOOL)controllerFocused {
  if (_controllerFocused == controllerFocused) {
    return;
  }
  _controllerFocused = controllerFocused;
  [self setNeedsUpdateConfiguration];
}

// Called by UIKit on every state change (focus, highlight, selection) and on
// trait changes, so the CGColor-backed border and shadow follow light/dark
// without a trait-change workaround. Colors are resolved against the state's
// trait collection because CGColor does not track appearance on its own.
- (void)updateConfigurationUsingState:(UICellConfigurationState*)state {
  [super updateConfigurationUsingState:state];
  UITraitCollection* traits = state.traitCollection;
  auto resolved = ^CGColorRef(UIColor* color) {
    return [color resolvedColorWithTraitCollection:traits].CGColor;
  };

  // Accessibility text sizes: a three-line title and stacked metadata.
  BOOL accessibilityText =
      UIContentSizeCategoryIsAccessibilityCategory(traits.preferredContentSizeCategory);
  self.titleLabel.numberOfLines = accessibilityText ? 3 : 2;
  self.metadataStackView.axis =
      accessibilityText ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
  self.metadataStackView.alignment =
      accessibilityText ? UIStackViewAlignmentLeading : UIStackViewAlignmentCenter;
  self.metadataStackView.spacing = accessibilityText ? 4.0 : 8.0;

  self.footerView.layer.borderColor =
      resolved([[XeniaTheme textPrimary] colorWithAlphaComponent:0.1]);
  // Controller focus: an accent ring around the cover. Keyboard focus gets the system's halo.
  const BOOL focused = self.controllerFocused;
  self.cardView.layer.borderWidth = focused ? 2.5f : 0.0f;
  self.cardView.layer.borderColor = focused ? resolved([XeniaTheme accent]) : nil;
  self.layer.zPosition = focused ? 1.0f : 0.0f;
  // Pressed look: dim the card slightly while the tile is highlighted.
  self.cardView.alpha = state.isHighlighted ? 0.85 : 1.0;
  self.contentTypePill.textColor = [XeniaTheme textMuted];
  self.contentTypePill.backgroundColor = [UIColor clearColor];
  self.contentTypePill.layer.borderColor = [UIColor clearColor].CGColor;
  self.compatPill.backgroundColor = [UIColor clearColor];
}

@end
