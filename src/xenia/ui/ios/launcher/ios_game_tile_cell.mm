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

@implementation XeniaGameTileCell

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) {
    return nil;
  }

  self.backgroundColor = [UIColor clearColor];
  self.layer.cornerRadius = XeniaRadiusLg;
  self.layer.masksToBounds = NO;
  self.layer.shadowOffset = CGSizeMake(0.0f, 6.0f);
  self.contentView.backgroundColor = [UIColor clearColor];
  self.isAccessibilityElement = YES;
  self.accessibilityTraits = UIAccessibilityTraitButton;

  self.cardView = [[UIView alloc] init];
  self.cardView.translatesAutoresizingMaskIntoConstraints = NO;
  self.cardView.backgroundColor = [XeniaTheme bgSurface];
  self.cardView.layer.cornerRadius = XeniaRadiusLg;
  self.cardView.layer.borderWidth = 0.5;
  self.cardView.layer.borderColor = [XeniaTheme border].CGColor;
  self.cardView.clipsToBounds = YES;
  [self.contentView addSubview:self.cardView];

  self.iconView = [[UIImageView alloc] init];
  self.iconView.translatesAutoresizingMaskIntoConstraints = NO;
  self.iconView.contentMode = UIViewContentModeScaleAspectFill;
  self.iconView.clipsToBounds = YES;
  self.iconView.backgroundColor = [XeniaTheme bgSurface2];
  [self.cardView addSubview:self.iconView];

  self.footerView = [[UIView alloc] init];
  self.footerView.translatesAutoresizingMaskIntoConstraints = NO;
  self.footerView.backgroundColor = [XeniaTheme bgSurface];
  [self.cardView addSubview:self.footerView];

  self.contentTypePill = [[XeniaPaddedLabel alloc] init];
  self.contentTypePill.translatesAutoresizingMaskIntoConstraints = NO;
  self.contentTypePill.padding = UIEdgeInsetsZero;
  self.contentTypePill.textAlignment = NSTextAlignmentLeft;
  self.contentTypePill.layer.cornerRadius = 0.0;
  self.contentTypePill.layer.borderWidth = 0.0;
  self.contentTypePill.clipsToBounds = NO;
  xe_apply_label_font(self.contentTypePill, UIFontTextStyleCaption2, 10.0, UIFontWeightSemibold);
  self.contentTypePill.textColor = [XeniaTheme textMuted];
  self.contentTypePill.backgroundColor = [UIColor clearColor];
  self.contentTypePill.hidden = YES;
  [self.contentTypePill setContentHuggingPriority:UILayoutPriorityRequired
                                          forAxis:UILayoutConstraintAxisHorizontal];
  [self.contentTypePill setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                        forAxis:UILayoutConstraintAxisHorizontal];

  self.titleLabel = [[UILabel alloc] init];
  self.titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  xe_apply_label_font(self.titleLabel, UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
  self.titleLabel.textColor = [XeniaTheme textPrimary];
  self.titleLabel.textAlignment = NSTextAlignmentLeft;
  self.titleLabel.numberOfLines = 1;
  self.titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
  self.titleLabel.allowsDefaultTighteningForTruncation = YES;
  self.titleLabel.adjustsFontSizeToFitWidth = NO;
  [self.titleLabel setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                   forAxis:UILayoutConstraintAxisHorizontal];
  [self.titleLabel setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                   forAxis:UILayoutConstraintAxisVertical];
  [self.footerView addSubview:self.titleLabel];

  self.compatPill = [[XeniaPaddedLabel alloc] init];
  self.compatPill.translatesAutoresizingMaskIntoConstraints = NO;
  self.compatPill.padding = UIEdgeInsetsZero;
  self.compatPill.textAlignment = NSTextAlignmentLeft;
  self.compatPill.layer.cornerRadius = 0.0;
  self.compatPill.layer.borderWidth = 0.0;
  self.compatPill.clipsToBounds = NO;
  self.compatPill.backgroundColor = [UIColor clearColor];
  xe_apply_label_font(self.compatPill, UIFontTextStyleCaption2, 10.0, UIFontWeightSemibold);
  self.compatPill.hidden = YES;
  [self.compatPill setContentHuggingPriority:UILayoutPriorityRequired
                                     forAxis:UILayoutConstraintAxisHorizontal];
  [self.compatPill setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                   forAxis:UILayoutConstraintAxisHorizontal];

  self.metadataStackView =
      [[UIStackView alloc] initWithArrangedSubviews:@[ self.contentTypePill, self.compatPill ]];
  self.metadataStackView.translatesAutoresizingMaskIntoConstraints = NO;
  self.metadataStackView.axis = UILayoutConstraintAxisHorizontal;
  self.metadataStackView.alignment = UIStackViewAlignmentCenter;
  self.metadataStackView.distribution = UIStackViewDistributionFill;
  self.metadataStackView.spacing = 8.0;
  [self.metadataStackView setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                                          forAxis:UILayoutConstraintAxisHorizontal];
  [self.metadataStackView setContentHuggingPriority:UILayoutPriorityRequired
                                            forAxis:UILayoutConstraintAxisVertical];
  [self.footerView addSubview:self.metadataStackView];

  // Apply the default chrome (shadow + border + label color) up front.
  // -updateControllerFocusAppearance only runs from -setControllerFocused:
  // and -traitCollectionDidChange:, so without this call a freshly-created
  // (never-focused) cell inherits the layer-default shadowOpacity = 0 and
  // never picks up the always-on tile shadow until it gets focused once.
  [self updateControllerFocusAppearance];

  [NSLayoutConstraint activateConstraints:@[
    [self.cardView.topAnchor constraintEqualToAnchor:self.contentView.topAnchor],
    [self.cardView.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor],
    [self.cardView.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor],
    [self.cardView.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor],
    [self.iconView.topAnchor constraintEqualToAnchor:self.cardView.topAnchor],
    [self.iconView.leadingAnchor constraintEqualToAnchor:self.cardView.leadingAnchor],
    [self.iconView.trailingAnchor constraintEqualToAnchor:self.cardView.trailingAnchor],
    [self.iconView.heightAnchor constraintEqualToAnchor:self.iconView.widthAnchor
                                             multiplier:300.0 / 219.0],
    [self.footerView.topAnchor constraintEqualToAnchor:self.iconView.bottomAnchor],
    [self.footerView.leadingAnchor constraintEqualToAnchor:self.cardView.leadingAnchor],
    [self.footerView.trailingAnchor constraintEqualToAnchor:self.cardView.trailingAnchor],
    [self.footerView.bottomAnchor constraintEqualToAnchor:self.cardView.bottomAnchor],
    [self.titleLabel.topAnchor constraintEqualToAnchor:self.footerView.topAnchor constant:8.0],
    [self.titleLabel.leadingAnchor constraintEqualToAnchor:self.footerView.leadingAnchor
                                                  constant:10.0],
    [self.titleLabel.trailingAnchor constraintEqualToAnchor:self.footerView.trailingAnchor
                                                   constant:-10.0],
    [self.titleLabel.bottomAnchor constraintLessThanOrEqualToAnchor:self.metadataStackView.topAnchor
                                                           constant:-2.0],
    [self.metadataStackView.leadingAnchor constraintEqualToAnchor:self.titleLabel.leadingAnchor],
    [self.metadataStackView.trailingAnchor
        constraintLessThanOrEqualToAnchor:self.titleLabel.trailingAnchor],
    [self.metadataStackView.bottomAnchor constraintEqualToAnchor:self.footerView.bottomAnchor
                                                        constant:-8.0],
  ]];

  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  self.layer.shadowPath =
      [UIBezierPath bezierPathWithRoundedRect:self.bounds cornerRadius:XeniaRadiusLg].CGPath;
  self.titleLabel.preferredMaxLayoutWidth = MAX(CGRectGetWidth(self.footerView.bounds) - 20.0, 0.0);
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection
          hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    // CGColor is captured at assignment time and won't auto-flip. Re-apply
    // the focus-aware chrome from the dynamic XeniaTheme accessors so it
    // tracks the new userInterfaceStyle.
    [self updateControllerFocusAppearance];
  }
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
  self.transform = CGAffineTransformIdentity;
  self.contentView.transform = CGAffineTransformIdentity;
  self.accessibilityLabel = nil;
  self.accessibilityValue = nil;
  _controllerFocused = NO;
  // Re-run the appearance setter explicitly: setting _controllerFocused
  // directly above bypassed -setControllerFocused: (since the previous
  // value may already have been NO if the cell was never focused), so the
  // shadow/border state has to be reset by hand to keep recycled cells
  // looking identical to fresh ones.
  [self updateControllerFocusAppearance];
}

- (void)setControllerFocused:(BOOL)controllerFocused {
  if (_controllerFocused == controllerFocused) {
    return;
  }
  _controllerFocused = controllerFocused;
  [self updateControllerFocusAppearance];
}

- (void)updateControllerFocusAppearance {
  self.footerView.backgroundColor = [XeniaTheme bgSurface];
  if (self.controllerFocused) {
    self.cardView.layer.borderWidth = 1.5f;
    self.cardView.layer.borderColor = [XeniaTheme accent].CGColor;
    self.titleLabel.textColor = [XeniaTheme textPrimary];
    self.layer.shadowColor = [XeniaTheme accent].CGColor;
    self.layer.shadowOpacity = 0.24f;
    self.layer.shadowRadius = 10.0f;
    self.layer.zPosition = 1.0f;
  } else {
    self.cardView.layer.borderWidth = 0.5f;
    self.cardView.layer.borderColor = [XeniaTheme border].CGColor;
    self.titleLabel.textColor = [XeniaTheme textPrimary];
    // A subtle always-on shadow so light-mode tiles read as floating cards
    // against a near-white background. In dark mode the shadow is black-on-
    // black at 18% opacity, which still gives a soft separation without
    // looking out of place.
    self.layer.shadowColor =
        [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
          return traits.userInterfaceStyle == UIUserInterfaceStyleLight
                     ? [[UIColor blackColor] colorWithAlphaComponent:0.18]
                     : [[UIColor blackColor] colorWithAlphaComponent:0.42];
        }].CGColor;
    self.layer.shadowOpacity = 0.18f;
    self.layer.shadowRadius = 6.0f;
    self.layer.zPosition = 0.0f;
  }
  self.contentTypePill.textColor = [XeniaTheme textMuted];
  self.contentTypePill.backgroundColor = [UIColor clearColor];
  self.contentTypePill.layer.borderColor = [UIColor clearColor].CGColor;
  self.compatPill.backgroundColor = [UIColor clearColor];
  self.compatPill.layer.borderWidth = 0.0f;
  self.compatPill.layer.borderColor = [UIColor clearColor].CGColor;
}

@end
