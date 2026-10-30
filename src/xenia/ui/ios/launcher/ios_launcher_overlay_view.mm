/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_launcher_overlay_view.h"

#include <cmath>

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/launcher/ios_game_tile_cell.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

@implementation XeniaIOSLauncherGameSnapshot

- (void)dealloc {
  [_title release];
  [_contentTypeName release];
  [_compatStatus release];
  [_iconData release];
  [super dealloc];
}

@end

@interface XeniaIOSLauncherOverlayView () <UICollectionViewDataSource,
                                           UICollectionViewDelegateFlowLayout>
@property(nonatomic, strong) NSArray<XeniaIOSLauncherGameSnapshot*>* games;
@property(nonatomic, assign) BOOL jitAcquired;
@property(nonatomic, assign) BOOL memoryEntitlementEnabled;
@end

@implementation XeniaIOSLauncherOverlayView {
  UIView* _navSep;
  UILabel* _titleLabel;
  UIButton* _settingsButton;
  UIButton* _profileButton;
  UIButton* _openGameButton;
  UIButton* _convertLibraryButton;
  UIView* _jitWarningCard;
  UIView* _jitStatusDot;
  UIView* _jitStatusRing;
  UILabel* _jitStatusLabel;
  UIView* _memoryWarningCard;
  UIImageView* _memoryStatusIcon;
  UILabel* _memoryStatusLabel;
  UIView* _jitReadyDot;
  UIView* _jitReadyRing;
  UILabel* _jitReadyLabel;
  UILabel* _libraryLabel;
  UIStackView* _topInfoStack;
  UILabel* _emptyLabel;
  UILabel* _statusLabel;
  UICollectionView* _gamesCollectionView;
  NSInteger _focusedGameIndex;
  BOOL _actionsEnabled;
  BOOL _controllerNavigationEnabled;
  BOOL _libraryFocusActive;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }
  _focusedGameIndex = -1;
  _jitAcquired = NO;
  _memoryEntitlementEnabled = YES;
  _actionsEnabled = YES;
  _controllerNavigationEnabled = NO;
  _libraryFocusActive = NO;

  self.backgroundColor = [XeniaTheme bgPrimary];
  self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  UILayoutGuide* safe = self.safeAreaLayoutGuide;
  CGFloat hPad = 16.0;

  // ── Nav bar: XeniOS (left) · gear + profile (right) ────────────────────

  _titleLabel = [[UILabel alloc] init];
  {
    UIFont* titleFont = xe_scaled_system_font(UIFontTextStyleTitle2, 22.0, UIFontWeightBold);
    NSDictionary* whiteAttrs = @{
      NSFontAttributeName : titleFont,
      NSForegroundColorAttributeName : [XeniaTheme textPrimary],
      NSKernAttributeName : @(0.8),
    };
    NSDictionary* accentAttrs = @{
      NSFontAttributeName : titleFont,
      NSForegroundColorAttributeName : [XeniaTheme accent],
      NSKernAttributeName : @(0.8),
    };
    NSMutableAttributedString* title =
        [[NSMutableAttributedString alloc] initWithString:@"Xeni" attributes:whiteAttrs];
    NSAttributedString* titleSuffix = [[NSAttributedString alloc] initWithString:@"OS"
                                                                      attributes:accentAttrs];
    [title appendAttributedString:titleSuffix];
    [titleSuffix release];
    _titleLabel.attributedText = title;
    [title release];
  }
  _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _titleLabel.adjustsFontForContentSizeCategory = YES;
  [self addSubview:_titleLabel];

  UIButtonConfiguration* settingsCfg = [UIButtonConfiguration plainButtonConfiguration];
  settingsCfg.image =
      [UIImage systemImageNamed:@"gearshape"
              withConfiguration:[UIImageSymbolConfiguration
                                    configurationWithPointSize:20
                                                        weight:UIImageSymbolWeightRegular]];
  settingsCfg.baseForegroundColor = [XeniaTheme textMuted];
  settingsCfg.contentInsets = NSDirectionalEdgeInsetsMake(8, 8, 8, 8);
  _settingsButton = [[UIButton buttonWithConfiguration:settingsCfg primaryAction:nil] retain];
  _settingsButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_settingsButton addTarget:self
                      action:@selector(settingsTapped:)
            forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(_settingsButton, @"Settings", nil, @"Opens XeniOS settings.",
                       UIAccessibilityTraitButton);
  [self addSubview:_settingsButton];

  UIButtonConfiguration* profileCfg = [UIButtonConfiguration plainButtonConfiguration];
  profileCfg.image =
      [UIImage systemImageNamed:@"person.circle"
              withConfiguration:[UIImageSymbolConfiguration
                                    configurationWithPointSize:20
                                                        weight:UIImageSymbolWeightRegular]];
  profileCfg.baseForegroundColor = [XeniaTheme textMuted];
  profileCfg.contentInsets = NSDirectionalEdgeInsetsMake(8, 8, 8, 8);
  _profileButton = [[UIButton buttonWithConfiguration:profileCfg primaryAction:nil] retain];
  _profileButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_profileButton addTarget:self
                     action:@selector(profileTapped:)
           forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(_profileButton, @"Profile", nil, @"Opens profile management.",
                       UIAccessibilityTraitButton);
  [self addSubview:_profileButton];

  // JIT ready indicator in nav bar.
  _jitReadyRing = [[UIView alloc] init];
  _jitReadyRing.translatesAutoresizingMaskIntoConstraints = NO;
  _jitReadyRing.backgroundColor = [UIColor clearColor];
  _jitReadyRing.layer.cornerRadius = 7.0;  // 14pt diameter / 2 — circular indicator.
  _jitReadyRing.layer.borderWidth = 1.25;
  _jitReadyRing.layer.borderColor = [XeniaTheme accent].CGColor;
  _jitReadyRing.alpha = 0;
  _jitReadyRing.userInteractionEnabled = NO;
  [self addSubview:_jitReadyRing];

  _jitReadyDot = [[UIView alloc] init];
  _jitReadyDot.translatesAutoresizingMaskIntoConstraints = NO;
  _jitReadyDot.backgroundColor = [XeniaTheme accent];
  _jitReadyDot.layer.cornerRadius = 4.0;  // 8pt diameter / 2 — circular dot.
  [self addSubview:_jitReadyDot];

  _jitReadyLabel = [[UILabel alloc] init];
  _jitReadyLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _jitReadyLabel.text = @"JIT Enabled";
  _jitReadyLabel.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(_jitReadyLabel, UIFontTextStyleCaption2, 11.0, UIFontWeightSemibold);
  [self addSubview:_jitReadyLabel];

  // Thin separator below the nav bar.
  _navSep = [[UIView alloc] init];
  _navSep.translatesAutoresizingMaskIntoConstraints = NO;
  _navSep.backgroundColor = [XeniaTheme border];
  [self addSubview:_navSep];

  // ── Compact JIT notice row shown only when JIT is not enabled ──────────

  _jitWarningCard = [[UIView alloc] init];
  _jitWarningCard.translatesAutoresizingMaskIntoConstraints = NO;
  _jitWarningCard.backgroundColor = [XeniaTheme bgSurface];
  _jitWarningCard.layer.cornerRadius = XeniaRadiusLg;
  _jitWarningCard.layer.borderWidth = 0.5;
  _jitWarningCard.layer.borderColor = [XeniaTheme border].CGColor;

  _jitStatusRing = [[UIView alloc] init];
  _jitStatusRing.translatesAutoresizingMaskIntoConstraints = NO;
  _jitStatusRing.backgroundColor = [UIColor clearColor];
  _jitStatusRing.layer.cornerRadius = 6.0;  // 12pt diameter / 2 — circular indicator.
  _jitStatusRing.layer.borderWidth = 1.25;
  _jitStatusRing.layer.borderColor = [XeniaTheme statusError].CGColor;
  _jitStatusRing.alpha = 0;
  _jitStatusRing.userInteractionEnabled = NO;
  [_jitWarningCard addSubview:_jitStatusRing];

  _jitStatusDot = [[UIView alloc] init];
  _jitStatusDot.translatesAutoresizingMaskIntoConstraints = NO;
  _jitStatusDot.backgroundColor = [XeniaTheme statusError];
  _jitStatusDot.layer.cornerRadius = 3.5;  // 7pt diameter / 2 — circular dot.
  [_jitWarningCard addSubview:_jitStatusDot];

  _jitStatusLabel = [[UILabel alloc] init];
  _jitStatusLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _jitStatusLabel.text = @"Waiting for JIT...";
  _jitStatusLabel.textColor = [XeniaTheme textPrimary];
  xe_apply_label_font(_jitStatusLabel, UIFontTextStyleFootnote, 13.0, UIFontWeightMedium);
  _jitStatusLabel.numberOfLines = 0;
  [_jitWarningCard addSubview:_jitStatusLabel];
  XEApplyAccessibility(_jitWarningCard, @"JIT status", _jitStatusLabel.text, nil,
                       UIAccessibilityTraitStaticText);

  // Compact memory-entitlement notice shown when the installed signature lacks
  // com.apple.developer.kernel.increased-memory-limit.
  _memoryWarningCard = [[UIView alloc] init];
  _memoryWarningCard.translatesAutoresizingMaskIntoConstraints = NO;
  _memoryWarningCard.backgroundColor = [XeniaTheme bgSurface];
  _memoryWarningCard.layer.cornerRadius = XeniaRadiusLg;
  _memoryWarningCard.layer.borderWidth = 0.5;
  _memoryWarningCard.layer.borderColor = [XeniaTheme border].CGColor;
  _memoryWarningCard.hidden = YES;

  UIImageSymbolConfiguration* memoryIconConfig =
      [UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightSemibold];
  UIImage* memoryIconImage = [UIImage systemImageNamed:@"exclamationmark.triangle.fill"
                                     withConfiguration:memoryIconConfig];
  _memoryStatusIcon = [[UIImageView alloc] initWithImage:memoryIconImage];
  _memoryStatusIcon.translatesAutoresizingMaskIntoConstraints = NO;
  _memoryStatusIcon.tintColor = [XeniaTheme statusWarning];
  _memoryStatusIcon.contentMode = UIViewContentModeScaleAspectFit;
  [_memoryWarningCard addSubview:_memoryStatusIcon];

  _memoryStatusLabel = [[UILabel alloc] init];
  _memoryStatusLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _memoryStatusLabel.text = xe_memory_entitlement_missing_status_message();
  _memoryStatusLabel.textColor = [XeniaTheme textPrimary];
  xe_apply_label_font(_memoryStatusLabel, UIFontTextStyleFootnote, 13.0, UIFontWeightMedium);
  _memoryStatusLabel.numberOfLines = 0;
  [_memoryWarningCard addSubview:_memoryStatusLabel];
  XEApplyAccessibility(_memoryWarningCard, @"Memory entitlement status", _memoryStatusLabel.text,
                       nil, UIAccessibilityTraitStaticText);

  // ── Library header: "Library" (left) + convert + "+" (right) ───────────

  UIView* libraryRow = [[UIView alloc] init];
  libraryRow.translatesAutoresizingMaskIntoConstraints = NO;

  _libraryLabel = [[UILabel alloc] init];
  _libraryLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _libraryLabel.text = @"Library";
  _libraryLabel.textColor = [XeniaTheme textPrimary];
  xe_apply_label_font(_libraryLabel, UIFontTextStyleTitle3, 20.0, UIFontWeightSemibold);
  _libraryLabel.accessibilityTraits = UIAccessibilityTraitHeader;
  [libraryRow addSubview:_libraryLabel];

  UIButtonConfiguration* convertCfg = [UIButtonConfiguration plainButtonConfiguration];
  convertCfg.image =
      [UIImage systemImageNamed:@"opticaldisc"
              withConfiguration:[UIImageSymbolConfiguration
                                    configurationWithPointSize:20
                                                        weight:UIImageSymbolWeightMedium]];
  convertCfg.baseForegroundColor = [XeniaTheme accent];
  convertCfg.contentInsets = NSDirectionalEdgeInsetsMake(6, 6, 6, 6);
  _convertLibraryButton = [[UIButton buttonWithConfiguration:convertCfg primaryAction:nil] retain];
  _convertLibraryButton.translatesAutoresizingMaskIntoConstraints = NO;
  _convertLibraryButton.hidden = YES;
  [_convertLibraryButton addTarget:self
                            action:@selector(convertLibraryTapped:)
                  forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(_convertLibraryButton, @"Convert library to ZAR", nil,
                       @"Converts non-ZAR games in the library.", UIAccessibilityTraitButton);

  UIButtonConfiguration* importCfg = [UIButtonConfiguration plainButtonConfiguration];
  importCfg.image =
      [UIImage systemImageNamed:@"plus"
              withConfiguration:[UIImageSymbolConfiguration
                                    configurationWithPointSize:20
                                                        weight:UIImageSymbolWeightMedium]];
  importCfg.baseForegroundColor = [XeniaTheme accent];
  importCfg.contentInsets = NSDirectionalEdgeInsetsMake(6, 6, 6, 6);
  _openGameButton = [[UIButton buttonWithConfiguration:importCfg primaryAction:nil] retain];
  _openGameButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_openGameButton addTarget:self
                      action:@selector(importTapped:)
            forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(_openGameButton, @"Import game", nil,
                       @"Opens the document picker to import a game.", UIAccessibilityTraitButton);

  UIStackView* libraryActionsStack =
      [[UIStackView alloc] initWithArrangedSubviews:@[ _convertLibraryButton, _openGameButton ]];
  libraryActionsStack.axis = UILayoutConstraintAxisHorizontal;
  libraryActionsStack.alignment = UIStackViewAlignmentCenter;
  libraryActionsStack.spacing = 4.0;
  libraryActionsStack.translatesAutoresizingMaskIntoConstraints = NO;
  [libraryRow addSubview:libraryActionsStack];

  // ── Collapsible stack: warnings → library header ───────────────────────

  _topInfoStack = [[UIStackView alloc]
      initWithArrangedSubviews:@[ _jitWarningCard, _memoryWarningCard, libraryRow ]];
  _topInfoStack.axis = UILayoutConstraintAxisVertical;
  _topInfoStack.spacing = 12;
  _topInfoStack.translatesAutoresizingMaskIntoConstraints = NO;
  [self addSubview:_topInfoStack];

  // ── Games grid ─────────────────────────────────────────────────────────

  UICollectionViewFlowLayout* gridLayout = [[UICollectionViewFlowLayout alloc] init];
  gridLayout.minimumInteritemSpacing = 16;
  gridLayout.minimumLineSpacing = 20;
  gridLayout.sectionInset = UIEdgeInsetsZero;
  _gamesCollectionView = [[UICollectionView alloc] initWithFrame:CGRectZero
                                            collectionViewLayout:gridLayout];
  _gamesCollectionView.translatesAutoresizingMaskIntoConstraints = NO;
  _gamesCollectionView.dataSource = self;
  _gamesCollectionView.delegate = self;
  if (@available(iOS 11.0, *)) {
    _gamesCollectionView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  }
  _gamesCollectionView.backgroundColor = [UIColor clearColor];
  _gamesCollectionView.alwaysBounceVertical = YES;
  [_gamesCollectionView registerClass:[XeniaGameTileCell class]
           forCellWithReuseIdentifier:@"ImportedGameCell"];
  [self addSubview:_gamesCollectionView];

  // Empty-state label.
  UIView* emptyBg = [[UIView alloc] initWithFrame:CGRectZero];
  emptyBg.frame = _gamesCollectionView.bounds;
  emptyBg.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  _emptyLabel = [[UILabel alloc] init];
  _emptyLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _emptyLabel.text =
      @"No games yet.\nTransfer ISO or GOD files to the\nDocuments folder, or tap +.";
  _emptyLabel.textColor = [XeniaTheme textMuted];
  xe_apply_label_font(_emptyLabel, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  _emptyLabel.textAlignment = NSTextAlignmentCenter;
  _emptyLabel.numberOfLines = 0;
  [emptyBg addSubview:_emptyLabel];
  NSLayoutConstraint* emptyLabelLeading =
      [_emptyLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:emptyBg.leadingAnchor
                                                             constant:32];
  emptyLabelLeading.priority = UILayoutPriorityDefaultHigh;
  NSLayoutConstraint* emptyLabelTrailing =
      [_emptyLabel.trailingAnchor constraintLessThanOrEqualToAnchor:emptyBg.trailingAnchor
                                                           constant:-32];
  emptyLabelTrailing.priority = UILayoutPriorityDefaultHigh;
  [NSLayoutConstraint activateConstraints:@[
    [_emptyLabel.centerXAnchor constraintEqualToAnchor:emptyBg.centerXAnchor],
    [_emptyLabel.centerYAnchor constraintEqualToAnchor:emptyBg.centerYAnchor],
    emptyLabelLeading,
    emptyLabelTrailing,
  ]];
  _gamesCollectionView.backgroundView = emptyBg;
  [emptyBg release];

  // ── Status label (ephemeral, bottom overlay) ─────────────────────────────

  _statusLabel = [[UILabel alloc] init];
  _statusLabel.text = @"";
  _statusLabel.textColor = [XeniaTheme textMuted];
  xe_apply_label_font(_statusLabel, UIFontTextStyleCaption2, 11.0, UIFontWeightRegular);
  _statusLabel.textAlignment = NSTextAlignmentCenter;
  _statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _statusLabel.hidden = YES;
  _statusLabel.userInteractionEnabled = NO;
  [self addSubview:_statusLabel];

  // ── Layout ─────────────────────────────────────────────────────────────

  [NSLayoutConstraint activateConstraints:@[
    // Nav bar.
    [_titleLabel.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:hPad],
    [_titleLabel.topAnchor constraintEqualToAnchor:safe.topAnchor constant:6],
    [_profileButton.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-8],
    [_profileButton.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
    [_settingsButton.trailingAnchor constraintEqualToAnchor:_profileButton.leadingAnchor
                                                   constant:-2],
    [_settingsButton.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
    // JIT ready dot + ring + label sit right after the title.
    [_jitReadyDot.leadingAnchor constraintEqualToAnchor:_titleLabel.trailingAnchor constant:10],
    [_jitReadyDot.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
    [_jitReadyDot.widthAnchor constraintEqualToConstant:8],
    [_jitReadyDot.heightAnchor constraintEqualToConstant:8],
    [_jitReadyRing.centerXAnchor constraintEqualToAnchor:_jitReadyDot.centerXAnchor],
    [_jitReadyRing.centerYAnchor constraintEqualToAnchor:_jitReadyDot.centerYAnchor],
    [_jitReadyRing.widthAnchor constraintEqualToConstant:14],
    [_jitReadyRing.heightAnchor constraintEqualToConstant:14],
    [_jitReadyLabel.leadingAnchor constraintEqualToAnchor:_jitReadyDot.trailingAnchor constant:6],
    [_jitReadyLabel.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
    [_jitReadyLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_settingsButton.leadingAnchor
                                                            constant:-8],

    // Nav separator.
    [_navSep.topAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor constant:6],
    [_navSep.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
    [_navSep.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    [_navSep.heightAnchor constraintEqualToConstant:0.5],

    // Header stack (JIT banner + library row) below separator.
    [_topInfoStack.topAnchor constraintEqualToAnchor:_navSep.bottomAnchor constant:12],
    [_topInfoStack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:hPad],
    [_topInfoStack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-hPad],

    // JIT banner internals.
    [_jitStatusDot.leadingAnchor constraintEqualToAnchor:_jitWarningCard.leadingAnchor constant:14],
    [_jitStatusDot.centerYAnchor constraintEqualToAnchor:_jitWarningCard.centerYAnchor],
    [_jitStatusDot.widthAnchor constraintEqualToConstant:7],
    [_jitStatusDot.heightAnchor constraintEqualToConstant:7],
    [_jitStatusRing.centerXAnchor constraintEqualToAnchor:_jitStatusDot.centerXAnchor],
    [_jitStatusRing.centerYAnchor constraintEqualToAnchor:_jitStatusDot.centerYAnchor],
    [_jitStatusRing.widthAnchor constraintEqualToConstant:12],
    [_jitStatusRing.heightAnchor constraintEqualToConstant:12],
    [_jitStatusLabel.leadingAnchor constraintEqualToAnchor:_jitStatusDot.trailingAnchor constant:8],
    [_jitStatusLabel.trailingAnchor constraintEqualToAnchor:_jitWarningCard.trailingAnchor
                                                   constant:-14],
    [_jitStatusLabel.topAnchor constraintEqualToAnchor:_jitWarningCard.topAnchor constant:10],
    [_jitStatusLabel.bottomAnchor constraintEqualToAnchor:_jitWarningCard.bottomAnchor
                                                 constant:-10],

    // Memory entitlement banner internals.
    [_memoryStatusIcon.leadingAnchor constraintEqualToAnchor:_memoryWarningCard.leadingAnchor
                                                    constant:14],
    [_memoryStatusIcon.centerYAnchor constraintEqualToAnchor:_memoryWarningCard.centerYAnchor],
    [_memoryStatusIcon.widthAnchor constraintEqualToConstant:18],
    [_memoryStatusIcon.heightAnchor constraintEqualToConstant:18],
    [_memoryStatusLabel.leadingAnchor constraintEqualToAnchor:_memoryStatusIcon.trailingAnchor
                                                     constant:10],
    [_memoryStatusLabel.trailingAnchor constraintEqualToAnchor:_memoryWarningCard.trailingAnchor
                                                      constant:-14],
    [_memoryStatusLabel.topAnchor constraintEqualToAnchor:_memoryWarningCard.topAnchor constant:10],
    [_memoryStatusLabel.bottomAnchor constraintEqualToAnchor:_memoryWarningCard.bottomAnchor
                                                    constant:-10],

    // Library row internals.
    [_libraryLabel.leadingAnchor constraintEqualToAnchor:libraryRow.leadingAnchor],
    [_libraryLabel.centerYAnchor constraintEqualToAnchor:libraryRow.centerYAnchor],
    [_libraryLabel.trailingAnchor
        constraintLessThanOrEqualToAnchor:libraryActionsStack.leadingAnchor
                                 constant:-8],
    [libraryActionsStack.trailingAnchor constraintEqualToAnchor:libraryRow.trailingAnchor],
    [libraryActionsStack.centerYAnchor constraintEqualToAnchor:libraryRow.centerYAnchor],
    [libraryRow.heightAnchor constraintEqualToConstant:34],

    // Games grid.
    [_gamesCollectionView.topAnchor constraintEqualToAnchor:_topInfoStack.bottomAnchor constant:8],
    [_gamesCollectionView.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:hPad],
    [_gamesCollectionView.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor
                                                        constant:-hPad],
    [_gamesCollectionView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],

    // Status label.
    [_statusLabel.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
    [_statusLabel.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-6],
  ]];

  [gridLayout release];
  [libraryActionsStack release];
  [libraryRow release];
  [self setJITAcquired:NO];
  [self setMemoryEntitlementEnabled:YES];
  [self applyActionsEnabledState];
  return self;
}

- (void)dealloc {
  [_games release];
  [_gamesCollectionView release];
  [_statusLabel release];
  [_settingsHandler release];
  [_profileHandler release];
  [_importHandler release];
  [_bulkZarConversionHandler release];
  [_gameLaunchedHandler release];
  [_copyLaunchURLHandler release];
  [_gameSettingsHandler release];
  [_touchLayoutHandler release];
  [_compatibilityHandler release];
  [_manageContentHandler release];
  [_discSelectionHandler release];
  [_patchesHandler release];
  [_zarConversionHandler release];
  [_titleLabel release];
  [_settingsButton release];
  [_profileButton release];
  [_openGameButton release];
  [_convertLibraryButton release];
  [_jitWarningCard release];
  [_jitStatusDot release];
  [_jitStatusRing release];
  [_jitStatusLabel release];
  [_memoryWarningCard release];
  [_memoryStatusIcon release];
  [_memoryStatusLabel release];
  [_jitReadyDot release];
  [_jitReadyRing release];
  [_jitReadyLabel release];
  [_libraryLabel release];
  [_topInfoStack release];
  [_emptyLabel release];
  [_navSep release];
  [super dealloc];
}

#pragma mark - Button actions (forward to handlers)

- (void)settingsTapped:(UIButton*)__unused sender {
  if (_settingsHandler) {
    _settingsHandler();
  }
}

- (void)profileTapped:(UIButton*)__unused sender {
  if (_profileHandler) {
    _profileHandler();
  }
}

- (void)importTapped:(UIButton*)__unused sender {
  if (_importHandler) {
    _importHandler();
  }
}

- (void)convertLibraryTapped:(UIButton*)__unused sender {
  if (_bulkZarConversionHandler) {
    _bulkZarConversionHandler();
  }
}

#pragma mark - Focus visuals

- (void)applyActionsEnabledState {
  _settingsButton.enabled = _actionsEnabled;
  _profileButton.enabled = _actionsEnabled;
  _openGameButton.enabled = _actionsEnabled;
  _convertLibraryButton.enabled = _actionsEnabled && !_convertLibraryButton.hidden;

  CGFloat actionAlpha = _actionsEnabled ? 1.0 : 0.5;
  _settingsButton.alpha = actionAlpha;
  _profileButton.alpha = actionAlpha;
  _openGameButton.alpha = actionAlpha;
  _convertLibraryButton.alpha = _convertLibraryButton.hidden ? 0.0 : actionAlpha;
}

- (void)setActionsEnabled:(BOOL)actionsEnabled {
  if (_actionsEnabled == actionsEnabled) {
    return;
  }
  _actionsEnabled = actionsEnabled;
  [self applyActionsEnabledState];
}

- (BOOL)isActionsEnabled {
  return _actionsEnabled;
}

- (void)setButton:(UIButton*)button controllerFocused:(BOOL)focused {
  if (!button) {
    return;
  }
  button.layer.cornerRadius = XeniaRadiusMd;
  button.layer.borderWidth = focused ? 1.5 : 0.0;
  button.layer.borderColor = focused ? [XeniaTheme accent].CGColor : [UIColor clearColor].CGColor;
  button.layer.shadowColor = [XeniaTheme accent].CGColor;
  button.layer.shadowOpacity = focused ? 0.35f : 0.0f;
  button.layer.shadowRadius = focused ? 6.0f : 0.0f;
  button.layer.shadowOffset = CGSizeZero;
}

- (void)clearButtonFocusChrome:(UIButton*)button {
  if (!button) {
    return;
  }
  button.layer.cornerRadius = XeniaRadiusMd;
  button.layer.borderWidth = 0.0f;
  button.layer.borderColor = [UIColor clearColor].CGColor;
  button.layer.shadowOpacity = 0.0f;
  button.layer.shadowRadius = 0.0f;
  button.layer.shadowOffset = CGSizeZero;
}

- (void)setControllerNavigationEnabled:(BOOL)enabled
                       settingsFocused:(BOOL)settingsFocused
                        profileFocused:(BOOL)profileFocused
                         importFocused:(BOOL)importFocused
                    libraryFocusActive:(BOOL)libraryFocusActive {
  BOOL previousLibraryFocusActive = _libraryFocusActive;
  _controllerNavigationEnabled = enabled;
  _libraryFocusActive = enabled && libraryFocusActive;

  if (!_controllerNavigationEnabled) {
    _libraryFocusActive = NO;
    [self clearButtonFocusChrome:_settingsButton];
    [self clearButtonFocusChrome:_profileButton];
    [self clearButtonFocusChrome:_openGameButton];
    [self clearButtonFocusChrome:_convertLibraryButton];
    if (_focusedGameIndex >= 0 && _focusedGameIndex < static_cast<NSInteger>(_games.count)) {
      NSIndexPath* focusedPath = [NSIndexPath indexPathForItem:_focusedGameIndex inSection:0];
      [_gamesCollectionView reloadItemsAtIndexPaths:@[ focusedPath ]];
    }
    return;
  }

  [self setButton:_settingsButton controllerFocused:settingsFocused];
  [self setButton:_profileButton controllerFocused:profileFocused];
  [self setButton:_openGameButton controllerFocused:importFocused];
  [self clearButtonFocusChrome:_convertLibraryButton];

  if (_libraryFocusActive && _focusedGameIndex < 0 && _games.count > 0) {
    [self setFocusedGameIndex:0 scroll:NO];
  }

  if (previousLibraryFocusActive != _libraryFocusActive && _focusedGameIndex >= 0 &&
      _focusedGameIndex < static_cast<NSInteger>(_games.count)) {
    NSIndexPath* focusedPath = [NSIndexPath indexPathForItem:_focusedGameIndex inSection:0];
    [_gamesCollectionView reloadItemsAtIndexPaths:@[ focusedPath ]];
  }
}

#pragma mark - Data ingestion

- (void)setGames:(NSArray<XeniaIOSLauncherGameSnapshot*>*)games {
  [_games release];
  _games = [games copy];
  _emptyLabel.hidden = _games.count > 0;
  BOOL hasConvertibleGames = NO;
  for (XeniaIOSLauncherGameSnapshot* game in _games) {
    if (game.supportsZarConversion) {
      hasConvertibleGames = YES;
      break;
    }
  }
  _convertLibraryButton.hidden = !hasConvertibleGames;
  if (_games.count == 0) {
    _focusedGameIndex = -1;
  } else if (_focusedGameIndex < 0 || _focusedGameIndex >= static_cast<NSInteger>(_games.count)) {
    _focusedGameIndex = 0;
  }
  [self applyActionsEnabledState];
  [_gamesCollectionView reloadData];
}

- (void)setJITAcquired:(BOOL)acquired {
  BOOL previous_acquired = _jitAcquired;
  _jitAcquired = acquired;
  if (acquired) {
    _jitStatusDot.backgroundColor = [XeniaTheme accent];
    _jitStatusLabel.text = @"JIT Enabled";
    _jitReadyDot.hidden = NO;
    _jitReadyLabel.hidden = NO;
    _jitReadyLabel.text = @"JIT Enabled";
    [_jitStatusRing.layer removeAllAnimations];
    _jitStatusRing.alpha = 0;
    xe_add_jit_ring_pulse(_jitReadyRing.layer, @"xenia.jit.ready.pulse", 1.7, 0.5, 2.0);
  } else {
    _jitStatusDot.backgroundColor = [XeniaTheme statusError];
    _jitReadyDot.hidden = YES;
    _jitReadyLabel.hidden = YES;
    [_jitReadyRing.layer removeAllAnimations];
    _jitReadyRing.alpha = 0;
    xe_add_jit_ring_pulse(_jitStatusRing.layer, @"xenia.jit.warn.pulse", 1.55, 0.42, 1.9);
  }

  BOOL previousHidden = _jitWarningCard.hidden;
  _jitWarningCard.hidden = acquired;
  if (previousHidden != _jitWarningCard.hidden) {
    [_gamesCollectionView.collectionViewLayout invalidateLayout];
  }
  _jitWarningCard.accessibilityValue = acquired ? @"JIT is enabled" : _jitStatusLabel.text;
  _jitReadyLabel.accessibilityLabel = acquired ? @"JIT is enabled" : @"JIT is not enabled";
  if (previous_acquired != acquired) {
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification,
                                    acquired ? @"JIT is enabled" : _jitStatusLabel.text);
  }
  [self applyActionsEnabledState];
}

- (void)setJITStatusText:(NSString*)text {
  _jitStatusLabel.text = text ?: @"";
  _jitWarningCard.accessibilityValue = _jitStatusLabel.text;
}

- (void)setMemoryEntitlementEnabled:(BOOL)enabled {
  BOOL previous_enabled = _memoryEntitlementEnabled;
  _memoryEntitlementEnabled = enabled;

  BOOL previous_hidden = _memoryWarningCard.hidden;
  _memoryWarningCard.hidden = enabled;
  if (previous_hidden != _memoryWarningCard.hidden) {
    [_gamesCollectionView.collectionViewLayout invalidateLayout];
  }

  if (!enabled && !_memoryStatusLabel.text.length) {
    _memoryStatusLabel.text = xe_memory_entitlement_missing_status_message();
  }
  _memoryWarningCard.accessibilityValue =
      enabled ? @"Increased memory entitlement is enabled" : _memoryStatusLabel.text;
  if (previous_enabled != enabled) {
    UIAccessibilityPostNotification(
        UIAccessibilityAnnouncementNotification,
        enabled ? @"Increased memory entitlement is enabled" : _memoryStatusLabel.text);
  }
}

- (void)setMemoryEntitlementStatusText:(NSString*)text {
  _memoryStatusLabel.text = text ?: @"";
  _memoryWarningCard.accessibilityValue = _memoryStatusLabel.text;
}

- (void)setFocusedGameIndex:(NSInteger)index scroll:(BOOL)scroll {
  if (_games.count == 0) {
    index = -1;
  } else {
    if (index < 0) {
      index = 0;
    }
    NSInteger maxIndex = static_cast<NSInteger>(_games.count - 1);
    if (index > maxIndex) {
      index = maxIndex;
    }
  }
  NSInteger previous = _focusedGameIndex;
  _focusedGameIndex = index;

  NSMutableArray<NSIndexPath*>* reloadPaths = [NSMutableArray array];
  if (previous >= 0 && previous < static_cast<NSInteger>(_games.count)) {
    [reloadPaths addObject:[NSIndexPath indexPathForItem:previous inSection:0]];
  }
  if (_focusedGameIndex >= 0 && _focusedGameIndex < static_cast<NSInteger>(_games.count) &&
      _focusedGameIndex != previous) {
    [reloadPaths addObject:[NSIndexPath indexPathForItem:_focusedGameIndex inSection:0]];
  }
  if (reloadPaths.count > 0) {
    [_gamesCollectionView reloadItemsAtIndexPaths:reloadPaths];
  }

  if (scroll && _focusedGameIndex >= 0 &&
      _focusedGameIndex < static_cast<NSInteger>(_games.count)) {
    NSIndexPath* path = [NSIndexPath indexPathForItem:_focusedGameIndex inSection:0];
    [_gamesCollectionView scrollToItemAtIndexPath:path
                                 atScrollPosition:UICollectionViewScrollPositionCenteredVertically
                                         animated:YES];
  }
}

- (NSInteger)focusedGameIndex {
  return _focusedGameIndex;
}

- (void)reloadGames {
  [_gamesCollectionView reloadData];
}

- (UILabel*)statusLabel {
  return _statusLabel;
}

- (UICollectionView*)gamesCollectionView {
  return _gamesCollectionView;
}

- (BOOL)isOverlayVisible {
  return !self.hidden;
}

- (NSInteger)pageStep {
  NSArray<NSIndexPath*>* visible = _gamesCollectionView.indexPathsForVisibleItems;
  if (visible.count > 0) {
    return visible.count;
  }
  return 6;
}

- (void)refreshChromeForCurrentTraits {
  _jitReadyRing.layer.borderColor = [XeniaTheme accent].CGColor;
  _jitWarningCard.layer.borderColor = [XeniaTheme border].CGColor;
  _memoryWarningCard.layer.borderColor = [XeniaTheme border].CGColor;
  _memoryStatusIcon.tintColor = [XeniaTheme statusWarning];
  _jitStatusRing.layer.borderColor = _jitStatusDot.backgroundColor
                                         ? _jitStatusDot.backgroundColor.CGColor
                                         : [XeniaTheme statusError].CGColor;
  [_gamesCollectionView reloadData];
}

- (void)setOverlayVisible:(BOOL)visible animated:(BOOL)animated {
  if (visible == !self.hidden) {
    return;
  }
  if (!animated) {
    self.hidden = !visible;
    self.alpha = visible ? 1.0 : 0.0;
    return;
  }
  if (visible) {
    self.hidden = NO;
    self.alpha = 0.0;
    [UIView animateWithDuration:0.3
                     animations:^{
                       self.alpha = 1.0;
                     }];
  } else {
    [UIView animateWithDuration:0.3
        animations:^{
          self.alpha = 0.0;
        }
        completion:^(__unused BOOL finished) {
          self.hidden = YES;
          self.alpha = 1.0;
        }];
  }
}

#pragma mark - Grid sizing helpers

- (BOOL)usesCompactLandscapeLayoutForContentSize:(CGSize)contentSize {
  return contentSize.width > contentSize.height && contentSize.height < 430.0f;
}

- (NSInteger)columnCountForContentSize:(CGSize)contentSize {
  CGFloat contentWidth = contentSize.width;
  BOOL compactLandscape = [self usesCompactLandscapeLayoutForContentSize:contentSize];
  CGFloat gridSpacing = compactLandscape ? 12.0f : 16.0f;
  CGFloat minimumTileWidth = compactLandscape ? 170.0f : 190.0f;
  NSInteger minimumColumns = compactLandscape ? 4 : 2;
  NSInteger maximumColumns = compactLandscape ? 5 : 6;

  NSInteger columns = static_cast<NSInteger>(
      floor((contentWidth + gridSpacing) / (minimumTileWidth + gridSpacing)));
  columns = MAX(columns, minimumColumns);
  columns = MIN(columns, maximumColumns);
  return columns;
}

- (NSInteger)columnCount {
  return [self columnCountForContentSize:_gamesCollectionView.bounds.size];
}

- (CGFloat)interitemSpacingForCollectionView:(UICollectionView*)collectionView {
  return [self usesCompactLandscapeLayoutForContentSize:collectionView.bounds.size] ? 12.0f : 16.0f;
}

- (CGFloat)lineSpacingForCollectionView:(UICollectionView*)collectionView {
  return [self usesCompactLandscapeLayoutForContentSize:collectionView.bounds.size] ? 14.0f : 20.0f;
}

- (CGFloat)titleHeightForCollectionView:(UICollectionView*)collectionView {
  return [self usesCompactLandscapeLayoutForContentSize:collectionView.bounds.size] ? 56.0f : 60.0f;
}

- (CGFloat)tileWidthForCollectionView:(UICollectionView*)collectionView
                              columns:(NSInteger)columns
                     interitemSpacing:(CGFloat)spacing {
  CGFloat contentWidth = collectionView.bounds.size.width;
  CGFloat totalSpacing = spacing * MAX(columns - 1, 0);
  static constexpr CGFloat kTileShadowMargin = 16.0f;
  CGFloat availableWidth = MAX(contentWidth - totalSpacing - kTileShadowMargin, 0.0f);
  CGFloat tileWidth = availableWidth / MAX(columns, 1);
  CGFloat screenScale =
      collectionView.window.screen ? collectionView.window.screen.scale : UIScreen.mainScreen.scale;
  tileWidth = floor(tileWidth * screenScale) / screenScale;
  return MAX(tileWidth, 100.0f);
}

#pragma mark - UICollectionViewDataSource

- (NSInteger)collectionView:(UICollectionView* __unused)collectionView
     numberOfItemsInSection:(NSInteger)__unused section {
  return static_cast<NSInteger>(_games.count);
}

- (UICollectionViewCell*)collectionView:(UICollectionView*)collectionView
                 cellForItemAtIndexPath:(NSIndexPath*)indexPath {
  XeniaGameTileCell* cell =
      [collectionView dequeueReusableCellWithReuseIdentifier:@"ImportedGameCell"
                                                forIndexPath:indexPath];
  if (indexPath.item < 0 || static_cast<NSUInteger>(indexPath.item) >= _games.count) {
    cell.titleLabel.text = @"";
    cell.iconView.image = nil;
    cell.contentTypePill.text = @"";
    cell.contentTypePill.hidden = YES;
    cell.compatPill.text = @"";
    cell.compatPill.hidden = YES;
    cell.controllerFocused = NO;
    cell.accessibilityLabel = nil;
    cell.accessibilityValue = nil;
    return cell;
  }

  XeniaIOSLauncherGameSnapshot* game = _games[static_cast<NSUInteger>(indexPath.item)];
  cell.titleLabel.text = game.title;
  cell.contentTypePill.text = game.contentTypeName ?: @"";
  cell.contentTypePill.hidden = game.contentTypeName.length == 0;
  cell.contentTypePill.textColor = [XeniaTheme textMuted];
  cell.contentTypePill.backgroundColor = [UIColor clearColor];
  cell.contentTypePill.layer.borderColor = [UIColor clearColor].CGColor;
  if (game.hasCompatInfo) {
    cell.compatPill.text = xe_compat_status_label(game.compatStatus);
    UIColor* pillColor = xe_compat_status_color(game.compatStatus);
    cell.compatPill.textColor = pillColor;
    cell.compatPill.backgroundColor = [UIColor clearColor];
    cell.compatPill.layer.borderWidth = 0.0;
    cell.compatPill.layer.borderColor = [UIColor clearColor].CGColor;
    cell.compatPill.hidden = NO;
  } else {
    cell.compatPill.text = @"";
    cell.compatPill.hidden = YES;
  }
  cell.controllerFocused =
      _controllerNavigationEnabled && _libraryFocusActive && _focusedGameIndex == indexPath.item;
  NSMutableArray<NSString*>* accessibility_values = [NSMutableArray array];
  if (game.contentTypeName.length) {
    [accessibility_values addObject:game.contentTypeName];
  }
  if (game.hasCompatInfo) {
    [accessibility_values addObject:xe_compat_status_label(game.compatStatus)];
  }
  cell.accessibilityLabel = game.title.length ? game.title : @"Untitled game";
  cell.accessibilityValue = [accessibility_values componentsJoinedByString:@", "];
  cell.accessibilityHint = @"Opens the game. Long press for more actions.";
  cell.accessibilityTraits = UIAccessibilityTraitButton;

  UIImage* icon = game.supportsRemoteArt ? xe_cached_game_art(game.titleId) : nil;
  if (icon) {
    cell.iconView.image = icon;
  } else {
    UIImage* fallback = nil;
    if (game.iconData.length) {
      fallback = [UIImage imageWithData:game.iconData];
    }
    if (!fallback) {
      fallback = [UIImage imageNamed:@"128"];
    }
    if (!fallback) {
      fallback = [UIImage systemImageNamed:@"gamecontroller.fill"];
    }
    cell.iconView.image = fallback;
    if (game.titleId && game.supportsRemoteArt) {
      uint32_t fetchTitleId = game.titleId;
      UICollectionView* cv = collectionView;
      xe_fetch_game_art(fetchTitleId, ^(UIImage* fetched) {
        if (!fetched || !cv) {
          return;
        }
        // Reload any cell that shares this title ID.
        __unsafe_unretained XeniaIOSLauncherOverlayView* unsafeSelf = self;
        NSMutableArray* reloadPaths = [NSMutableArray array];
        for (NSUInteger i = 0; i < unsafeSelf->_games.count; ++i) {
          if (unsafeSelf->_games[i].titleId == fetchTitleId) {
            [reloadPaths addObject:[NSIndexPath indexPathForItem:static_cast<NSInteger>(i)
                                                       inSection:0]];
          }
        }
        if (reloadPaths.count > 0) {
          [cv reloadItemsAtIndexPaths:reloadPaths];
        }
      });
    }
  }
  return cell;
}

#pragma mark - UICollectionViewDelegate

- (void)collectionView:(UICollectionView*)collectionView
    didSelectItemAtIndexPath:(NSIndexPath*)indexPath {
  [collectionView deselectItemAtIndexPath:indexPath animated:YES];
  if (indexPath.item < 0 || static_cast<NSUInteger>(indexPath.item) >= _games.count) {
    return;
  }
  [self setFocusedGameIndex:indexPath.item scroll:NO];
  if (_gameLaunchedHandler) {
    _gameLaunchedHandler(static_cast<NSUInteger>(indexPath.item));
  }
}

- (UITargetedPreview*)contextMenuPreviewForConfiguration:(UIContextMenuConfiguration*)configuration
                                          collectionView:(UICollectionView*)collectionView {
  id identifier = configuration.identifier;
  NSIndexPath* indexPath =
      [identifier isKindOfClass:[NSIndexPath class]] ? (NSIndexPath*)identifier : nil;
  if (!indexPath) {
    return nil;
  }
  XeniaGameTileCell* cell = (XeniaGameTileCell*)[collectionView cellForItemAtIndexPath:indexPath];
  if (![cell isKindOfClass:[XeniaGameTileCell class]]) {
    return nil;
  }
  UIPreviewParameters* parameters = [[[UIPreviewParameters alloc] init] autorelease];
  parameters.backgroundColor = [UIColor clearColor];
  parameters.visiblePath = [UIBezierPath bezierPathWithRoundedRect:cell.cardView.bounds
                                                      cornerRadius:XeniaRadiusLg];
  return [[[UITargetedPreview alloc] initWithView:cell.cardView parameters:parameters] autorelease];
}

- (void)resetContextMenuCellTransforms {
  for (UICollectionViewCell* visibleCell in _gamesCollectionView.visibleCells) {
    if (![visibleCell isKindOfClass:[XeniaGameTileCell class]]) {
      continue;
    }
    XeniaGameTileCell* gameCell = (XeniaGameTileCell*)visibleCell;
    gameCell.transform = CGAffineTransformIdentity;
    gameCell.contentView.transform = CGAffineTransformIdentity;
    gameCell.cardView.transform = CGAffineTransformIdentity;
    gameCell.layer.zPosition = gameCell.controllerFocused ? 1.0f : 0.0f;
    [gameCell setNeedsLayout];
  }
  [_gamesCollectionView.collectionViewLayout invalidateLayout];
}

- (UIContextMenuConfiguration*)collectionView:(UICollectionView*)collectionView
    contextMenuConfigurationForItemAtIndexPath:(NSIndexPath*)indexPath
                                         point:(CGPoint)point {
  (void)collectionView;
  (void)point;
  if (indexPath.item < 0 || static_cast<NSUInteger>(indexPath.item) >= _games.count) {
    return nil;
  }

  NSUInteger gameIndex = static_cast<NSUInteger>(indexPath.item);
  return [UIContextMenuConfiguration
      configurationWithIdentifier:indexPath
                  previewProvider:nil
                   actionProvider:^UIMenu*(NSArray<UIMenuElement*>* __unused suggestedActions) {
                     __unsafe_unretained XeniaIOSLauncherOverlayView* unsafeSelf = self;
                     if (gameIndex >= unsafeSelf->_games.count) {
                       return nil;
                     }
                     XeniaIOSLauncherGameSnapshot* game = unsafeSelf->_games[gameIndex];

                     UIAction* playAction =
                         [UIAction actionWithTitle:@"Play"
                                             image:[UIImage systemImageNamed:@"play.fill"]
                                        identifier:nil
                                           handler:^(__unused UIAction* action) {
                                             if (unsafeSelf->_gameLaunchedHandler) {
                                               unsafeSelf->_gameLaunchedHandler(gameIndex);
                                             }
                                           }];
                     UIAction* gameSettingsAction =
                         [UIAction actionWithTitle:@"Game Settings"
                                             image:[UIImage systemImageNamed:@"slider.horizontal.3"]
                                        identifier:nil
                                           handler:^(__unused UIAction* action) {
                                             if (unsafeSelf->_gameSettingsHandler) {
                                               unsafeSelf->_gameSettingsHandler(gameIndex);
                                             }
                                           }];
                     UIAction* touchLayoutAction =
                         [UIAction actionWithTitle:@"Touch Layout"
                                             image:[UIImage systemImageNamed:@"hand.tap"]
                                        identifier:nil
                                           handler:^(__unused UIAction* action) {
                                             if (unsafeSelf->_touchLayoutHandler) {
                                               unsafeSelf->_touchLayoutHandler(gameIndex);
                                             }
                                           }];
                     UIAction* compatAction =
                         [UIAction actionWithTitle:@"Compatibility"
                                             image:[UIImage systemImageNamed:@"checkmark.shield"]
                                        identifier:nil
                                           handler:^(__unused UIAction* action) {
                                             if (unsafeSelf->_compatibilityHandler) {
                                               unsafeSelf->_compatibilityHandler(gameIndex);
                                             }
                                           }];
                     UIAction* contentAction =
                         [UIAction actionWithTitle:@"Manage Content"
                                             image:[UIImage systemImageNamed:@"square.stack.3d.up"]
                                        identifier:nil
                                           handler:^(__unused UIAction* action) {
                                             if (unsafeSelf->_manageContentHandler) {
                                               unsafeSelf->_manageContentHandler(gameIndex);
                                             }
                                           }];
                     UIAction* discAction =
                         [UIAction actionWithTitle:@"Launch Disc"
                                             image:[UIImage systemImageNamed:@"opticaldisc"]
                                        identifier:nil
                                           handler:^(__unused UIAction* action) {
                                             if (unsafeSelf->_discSelectionHandler) {
                                               unsafeSelf->_discSelectionHandler(gameIndex);
                                             }
                                           }];
                     UIAction* patchesAction = [UIAction
                         actionWithTitle:@"Patches"
                                   image:[UIImage systemImageNamed:@"puzzlepiece.extension"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_patchesHandler) {
                                     unsafeSelf->_patchesHandler(gameIndex);
                                   }
                                 }];
                     UIAction* zarConversionAction =
                         [UIAction actionWithTitle:@"Convert to ZAR"
                                             image:[UIImage systemImageNamed:@"opticaldisc"]
                                        identifier:nil
                                           handler:^(__unused UIAction* action) {
                                             if (unsafeSelf->_zarConversionHandler) {
                                               unsafeSelf->_zarConversionHandler(gameIndex);
                                             }
                                           }];
                     UIAction* copyURLAction =
                         [UIAction actionWithTitle:@"Copy Launch URL"
                                             image:[UIImage systemImageNamed:@"link"]
                                        identifier:nil
                                           handler:^(__unused UIAction* action) {
                                             if (unsafeSelf->_copyLaunchURLHandler) {
                                               unsafeSelf->_copyLaunchURLHandler(gameIndex);
                                             }
                                           }];
                     if (!game.supportsCompatibility) {
                       compatAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.supportsManageContent) {
                       contentAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.supportsDiscSelection) {
                       discAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.supportsPatches) {
                       patchesAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.supportsZarConversion) {
                       zarConversionAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.titleId) {
                       gameSettingsAction.attributes = UIMenuElementAttributesDisabled;
                       touchLayoutAction.attributes = UIMenuElementAttributesDisabled;
                       copyURLAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     return [UIMenu menuWithTitle:@""
                                         children:@[
                                           playAction, gameSettingsAction, touchLayoutAction,
                                           discAction, compatAction, contentAction, patchesAction,
                                           zarConversionAction, copyURLAction
                                         ]];
                   }];
}

- (UITargetedPreview*)collectionView:(UICollectionView*)collectionView
    previewForHighlightingContextMenuWithConfiguration:(UIContextMenuConfiguration*)configuration {
  return [self contextMenuPreviewForConfiguration:configuration collectionView:collectionView];
}

- (UITargetedPreview*)collectionView:(UICollectionView*)collectionView
    previewForDismissingContextMenuWithConfiguration:(UIContextMenuConfiguration*)configuration {
  return [self contextMenuPreviewForConfiguration:configuration collectionView:collectionView];
}

- (void)collectionView:(UICollectionView*)collectionView
    willEndContextMenuInteractionWithConfiguration:(UIContextMenuConfiguration*)configuration
                                          animator:(id<UIContextMenuInteractionAnimating>)animator {
  (void)collectionView;
  (void)configuration;
  [animator addCompletion:^{
    [self resetContextMenuCellTransforms];
  }];
}

#pragma mark - UICollectionViewDelegateFlowLayout

- (CGSize)collectionView:(UICollectionView*)collectionView
                    layout:(UICollectionViewLayout* __unused)collectionViewLayout
    sizeForItemAtIndexPath:(NSIndexPath* __unused)indexPath {
  NSInteger columns = [self columnCountForContentSize:collectionView.bounds.size];
  CGFloat spacing = [self interitemSpacingForCollectionView:collectionView];
  CGFloat tileWidth = [self tileWidthForCollectionView:collectionView
                                               columns:columns
                                      interitemSpacing:spacing];
  CGFloat imageHeight = ceil(tileWidth * 300.0f / 219.0f);
  return CGSizeMake(tileWidth, imageHeight + [self titleHeightForCollectionView:collectionView]);
}

- (CGFloat)collectionView:(UICollectionView*)collectionView
                                      layout:(UICollectionViewLayout* __unused)collectionViewLayout
    minimumInteritemSpacingForSectionAtIndex:(NSInteger)__unused section {
  return [self interitemSpacingForCollectionView:collectionView];
}

- (CGFloat)collectionView:(UICollectionView*)collectionView
                                 layout:(UICollectionViewLayout* __unused)collectionViewLayout
    minimumLineSpacingForSectionAtIndex:(NSInteger)__unused section {
  return [self lineSpacingForCollectionView:collectionView];
}

- (UIEdgeInsets)collectionView:(UICollectionView*)collectionView
                        layout:(UICollectionViewLayout* __unused)collectionViewLayout
        insetForSectionAtIndex:(NSInteger)__unused section {
  NSInteger columns = [self columnCountForContentSize:collectionView.bounds.size];
  CGFloat spacing = [self interitemSpacingForCollectionView:collectionView];
  CGFloat tileWidth = [self tileWidthForCollectionView:collectionView
                                               columns:columns
                                      interitemSpacing:spacing];
  CGFloat consumedWidth = tileWidth * columns + spacing * MAX(columns - 1, 0);
  CGFloat remainder = MAX(collectionView.bounds.size.width - consumedWidth, 0.0f);
  CGFloat screenScale =
      collectionView.window.screen ? collectionView.window.screen.scale : UIScreen.mainScreen.scale;
  CGFloat leftInset = floor((remainder * 0.5f) * screenScale) / screenScale;
  CGFloat rightInset = MAX(remainder - leftInset, 0.0f);
  return UIEdgeInsetsMake(0.0f, leftInset, 0.0f, rightInset);
}

@end
