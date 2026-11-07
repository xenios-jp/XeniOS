/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_settings_hub_view_controller.h"

#import <GameController/GameController.h>

#include <algorithm>
#include <utility>

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_dashboard.h"
#import "xenia/ui/ios/launcher/ios_external_folders_view_controller.h"
#import "xenia/ui/ios/launcher/ios_library_ambient_view.h"
#import "xenia/ui/ios/settings/ios_acknowledgements_view_controller.h"
#import "xenia/ui/ios/settings/ios_config_catalog.h"
#import "xenia/ui/ios/settings/ios_config_storage.h"
#import "xenia/ui/ios/settings/ios_config_view_controller.h"
#import "xenia/ui/ios/settings/ios_debug_settings_view_controller.h"
#import "xenia/ui/ios/settings/ios_profile_view_controller.h"
#import "xenia/ui/ios/settings/ios_system_software_view_controller.h"
#import "xenia/ui/ios/shared/ios_art_wash.h"
#import "xenia/ui/ios/shared/ios_diagnostics.h"
#import "xenia/ui/ios/shared/ios_live_account.h"
#import "xenia/ui/ios/shared/ios_diagnostics_outbox.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_update_check.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

void XeniaOpenSettingsURL(NSString* url_string) {
  if (!url_string.length) {
    return;
  }
  NSURL* url = [NSURL URLWithString:url_string];
  if (!url) {
    return;
  }
  [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
}

NSString* XeniaBundleValue(NSString* key) {
  id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:key];
  return [value isKindOfClass:[NSString class]] ? value : nil;
}

static NSString* const kXeniaAppIconLightName = @"AppIconLight";
static NSString* const kXeniaAppIconDarkName = @"AppIconDark";

NSString* XeniaAppIconNameForRow(NSInteger row) {
  if (row == 1) {
    return kXeniaAppIconLightName;
  }
  if (row == 2) {
    return kXeniaAppIconDarkName;
  }
  return nil;
}

NSString* XeniaAppIconTitleForRow(NSInteger row) {
  if (row == 1) {
    return NSLocalizedString(@"Light", @"settings hub view controller: user-visible text");
  }
  if (row == 2) {
    return NSLocalizedString(@"Dark", @"settings hub view controller: user-visible text");
  }
  return NSLocalizedString(@"Automatic", @"settings hub view controller: user-visible text");
}

NSString* XeniaAppIconSubtitleForRow(NSInteger row) {
  if (row == 1) {
    return NSLocalizedString(@"Use the light icon.",
                             @"settings hub view controller: user-visible text");
  }
  if (row == 2) {
    return NSLocalizedString(@"Use the dark icon.",
                             @"settings hub view controller: user-visible text");
  }
  return NSLocalizedString(@"Light or dark, following the system appearance.",
                           @"settings hub view controller: user-visible text");
}

NSString* XeniaAppIconSymbolForRow(NSInteger row) {
  if (row == 1) {
    return @"sun.max.fill";
  }
  if (row == 2) {
    return @"moon.fill";
  }
  return @"app.fill";
}

// The icon itself, small and with the Home Screen's rounded corners. Automatic is the light
// icon with its lower right half dark, since it follows the system appearance.
UIImage* XeniaAppIconPreviewForRow(NSInteger row) {
  UIImage* light = [UIImage imageNamed:@"AppIconPreviewLight"];
  UIImage* dark = [UIImage imageNamed:@"AppIconPreviewDark"];
  if (!light || !dark) {
    return nil;
  }
  const CGFloat side = 44.0;
  const CGRect bounds = CGRectMake(0.0, 0.0, side, side);
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:bounds.size] autorelease];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused context) {
    UIBezierPath* shape = [UIBezierPath bezierPathWithRoundedRect:bounds
                                                     cornerRadius:side * 0.2237];
    [shape addClip];
    [(row == 2 ? dark : light) drawInRect:bounds];
    if (row == 0) {
      UIBezierPath* half = [UIBezierPath bezierPath];
      [half moveToPoint:CGPointMake(side, 0.0)];
      [half addLineToPoint:CGPointMake(side, side)];
      [half addLineToPoint:CGPointMake(0.0, side)];
      [half closePath];
      [half addClip];
      [dark drawInRect:bounds];
    }
    [[UIColor colorWithWhite:0.5 alpha:0.3] setStroke];
    shape.lineWidth = 1.0 / UIScreen.mainScreen.scale;
    [shape stroke];
  }];
}

BOOL XeniaAppIconNamesEqual(NSString* lhs, NSString* rhs) {
  if (!lhs.length && !rhs.length) {
    return YES;
  }
  return [lhs isEqualToString:rhs];
}

}  // namespace

typedef NS_ENUM(NSInteger, XeniaSettingsHubCategoryKind) {
  XeniaSettingsHubCategoryKindProfile,
  XeniaSettingsHubCategoryKindAppearance,
  XeniaSettingsHubCategoryKindDisplay,
  XeniaSettingsHubCategoryKindGraphics,
  XeniaSettingsHubCategoryKindAudio,
  XeniaSettingsHubCategoryKindControls,
  XeniaSettingsHubCategoryKindTouchLayouts,
  XeniaSettingsHubCategoryKindTouchBehavior,
  XeniaSettingsHubCategoryKindPerformance,
  XeniaSettingsHubCategoryKindCompatibility,
  XeniaSettingsHubCategoryKindDiagnostics,
  XeniaSettingsHubCategoryKindAdvancedDebug,
  XeniaSettingsHubCategoryKindAllCvars,
  XeniaSettingsHubCategoryKindAllSettings,
  XeniaSettingsHubCategoryKindLibrary,
  XeniaSettingsHubCategoryKindSystemSoftware,
  XeniaSettingsHubCategoryKindAutomation,
  XeniaSettingsHubCategoryKindPrivacy,
  XeniaSettingsHubCategoryKindOnline,
  XeniaSettingsHubCategoryKindAbout,
  XeniaSettingsHubCategoryKindLink,  // Opens `url` outside the app.
};

@interface XeniaSettingsHubItem : NSObject
@property(nonatomic, assign) XeniaSettingsHubCategoryKind kind;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* subtitle;
@property(nonatomic, copy) NSString* symbolName;
@property(nonatomic, retain) UIColor* tintColor;
// Search results: the current value (shown on the trailing edge and read by VoiceOver), the row to
// scroll to on the page it opens, and a VoiceOver hint that names that page.
@property(nonatomic, copy) NSString* valueText;
// The value's colour, when it is a warning ("Not Set Up"); nil for the usual secondary text.
@property(nonatomic, retain) UIColor* valueColor;
@property(nonatomic, copy) NSString* focusKey;
@property(nonatomic, copy) NSString* hintText;
// A Controls action that does nothing unless a game is running.
@property(nonatomic, assign) BOOL needsRunningGame;
// Words search also finds the row by, for what is on its page ("Online Features" for Privacy).
@property(nonatomic, copy) NSString* keywords;
// A link row's address.
@property(nonatomic, copy) NSString* url;
+ (instancetype)itemWithKind:(XeniaSettingsHubCategoryKind)kind
                       title:(NSString*)title
                    subtitle:(NSString*)subtitle
                  symbolName:(NSString*)symbol_name
                   tintColor:(UIColor*)tint_color;
@end

@implementation XeniaSettingsHubItem

+ (instancetype)itemWithKind:(XeniaSettingsHubCategoryKind)kind
                       title:(NSString*)title
                    subtitle:(NSString*)subtitle
                  symbolName:(NSString*)symbol_name
                   tintColor:(UIColor*)tint_color {
  XeniaSettingsHubItem* item = [[[XeniaSettingsHubItem alloc] init] autorelease];
  item.kind = kind;
  item.title = title;
  item.subtitle = subtitle;
  item.symbolName = symbol_name;
  item.tintColor = tint_color;
  return item;
}

- (void)dealloc {
  [_title release];
  [_subtitle release];
  [_symbolName release];
  [_tintColor release];
  [_valueText release];
  [_valueColor release];
  [_focusKey release];
  [_hintText release];
  [_keywords release];
  [_url release];
  [super dealloc];
}

@end

@interface XeniaSettingsHubSection : NSObject
@property(nonatomic, copy) NSString* title;
@property(nonatomic, retain) NSArray* items;
+ (instancetype)sectionWithTitle:(NSString*)title items:(NSArray*)items;
@end

@implementation XeniaSettingsHubSection

+ (instancetype)sectionWithTitle:(NSString*)title items:(NSArray*)items {
  XeniaSettingsHubSection* section =
      [[[XeniaSettingsHubSection alloc] init] autorelease];
  section.title = title;
  section.items = items;
  return section;
}

- (void)dealloc {
  [_title release];
  [_items release];
  [super dealloc];
}

@end

static BOOL XeniaSettingsStringMatches(NSString* text, NSString* query) {
  if (!query.length) {
    return YES;
  }
  if (!text.length) {
    return NO;
  }
  return [text rangeOfString:query
                     options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch]
      .location != NSNotFound;
}

static BOOL XeniaSettingsHubItemMatches(XeniaSettingsHubItem* item,
                                        NSString* section_title,
                                        NSString* query) {
  return XeniaSettingsStringMatches(item.title, query) ||
         XeniaSettingsStringMatches(item.subtitle, query) ||
         XeniaSettingsStringMatches(item.keywords, query) ||
         XeniaSettingsStringMatches(section_title, query);
}

// Every word of the query somewhere in the setting's text, underscores read as spaces, so
// "protect zero" finds protect_zero and "metal shader" finds a Metal shader setting.
// Raw emulator options match by name only: their descriptions are long and technical, and would
// bury the curated settings under debug options for everyday words.
static BOOL XeniaSettingsSearchEntryMatches(const IOSConfigSearchEntry& entry,
                                            NSString* query) {
  std::string text = entry.title + "\n" + entry.key;
  if (!entry.advanced) {
    text += "\n" + entry.subtitle + "\n" + entry.section + "\n" + entry.page;
    if (entry.value_searchable) {
      text += "\n" + entry.value_text;
    }
  }
  NSString* haystack = [ToNSString(text) stringByReplacingOccurrencesOfString:@"_"
                                                                   withString:@" "];
  NSCharacterSet* separators = [NSCharacterSet characterSetWithCharactersInString:@" _\t"];
  for (NSString* word in [query componentsSeparatedByCharactersInSet:separators]) {
    if (word.length && !XeniaSettingsStringMatches(haystack, word)) {
      return NO;
    }
  }
  return YES;
}

// Lower ranks first: the title starts with the query, then a word in the title does, then the
// title contains it, then only the description or path matched.
static int XeniaSettingsSearchRank(const IOSConfigSearchEntry& entry, NSString* query) {
  NSString* title = ToNSString(entry.title);
  NSStringCompareOptions options = NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch;
  NSRange range = [title rangeOfString:query options:options];
  if (range.location == NSNotFound) {
    return 3;
  }
  if (range.location == 0) {
    return 0;
  }
  for (NSString* word in [title componentsSeparatedByString:@" "]) {
    if ([word rangeOfString:query options:options | NSAnchoredSearch].location != NSNotFound) {
      return 1;
    }
  }
  return 2;
}

static XeniaSettingsHubCategoryKind XeniaSettingsKindForCatalog(
    IOSConfigCatalogKind kind) {
  switch (kind) {
    case IOSConfigCatalogKind::kDisplay:
      return XeniaSettingsHubCategoryKindDisplay;
    case IOSConfigCatalogKind::kGraphics:
      return XeniaSettingsHubCategoryKindGraphics;
    case IOSConfigCatalogKind::kAudio:
      return XeniaSettingsHubCategoryKindAudio;
    case IOSConfigCatalogKind::kControls:
      return XeniaSettingsHubCategoryKindTouchBehavior;
    case IOSConfigCatalogKind::kPerformance:
      return XeniaSettingsHubCategoryKindPerformance;
    case IOSConfigCatalogKind::kCompatibility:
      return XeniaSettingsHubCategoryKindCompatibility;
    case IOSConfigCatalogKind::kDiagnostics:
      return XeniaSettingsHubCategoryKindDiagnostics;
    case IOSConfigCatalogKind::kDebugSettings:
      return XeniaSettingsHubCategoryKindAdvancedDebug;
    case IOSConfigCatalogKind::kSystem:
      return XeniaSettingsHubCategoryKindAutomation;
    case IOSConfigCatalogKind::kOnline:
      return XeniaSettingsHubCategoryKindOnline;
    case IOSConfigCatalogKind::kAllSettings:
      return XeniaSettingsHubCategoryKindAllSettings;
    default:
      return XeniaSettingsHubCategoryKindAllCvars;
  }
}

static NSString* XeniaSettingsSymbolForCatalog(IOSConfigCatalogKind kind) {
  switch (kind) {
    case IOSConfigCatalogKind::kDisplay:
      return @"display";
    case IOSConfigCatalogKind::kGraphics:
      return @"gearshape.2.fill";
    case IOSConfigCatalogKind::kAudio:
      return @"speaker.wave.2.fill";
    case IOSConfigCatalogKind::kControls:
      return @"gamecontroller.fill";
    case IOSConfigCatalogKind::kPerformance:
      return @"bolt.fill";
    case IOSConfigCatalogKind::kCompatibility:
      return @"wrench.and.screwdriver.fill";
    case IOSConfigCatalogKind::kDiagnostics:
      return @"waveform.path.ecg";
    case IOSConfigCatalogKind::kDebugSettings:
      return @"slider.horizontal.3";
    case IOSConfigCatalogKind::kSystem:
      return @"wand.and.stars";
    case IOSConfigCatalogKind::kOnline:
      return @"person.2.fill";
    case IOSConfigCatalogKind::kAllSettings:
      return @"list.bullet";
    default:
      return @"tablecells";
  }
}

// A search result for one setting: title, "Page › Section" path (plus a note when the setting can
// be set per game), the current value on the trailing edge, and the row to scroll to.
static XeniaSettingsHubItem* XeniaSettingsResultItem(const IOSConfigSearchEntry& entry) {
  NSString* page = ToNSString(entry.page);
  NSString* section = ToNSString(entry.section);
  // A section already named for its page ("Display", "Audio") is the whole path.
  NSString* path = page;
  if (section.length && [section hasPrefix:page]) {
    path = section;
  } else if (section.length) {
    path = [NSString stringWithFormat:@"%@ \u203A %@", page, section];
  }
  if (entry.per_game) {
    path = [path stringByAppendingFormat:@"\n%@",
                 NSLocalizedString(@"Can be overridden per game",
                                   @"settings hub view controller: search result note")];
  }
  XeniaSettingsHubItem* item =
      [XeniaSettingsHubItem itemWithKind:XeniaSettingsKindForCatalog(entry.kind)
                                   title:ToNSString(entry.title)
                                subtitle:path
                              symbolName:XeniaSettingsSymbolForCatalog(entry.kind)
                               tintColor:nil];
  item.valueText = entry.value_text.empty() ? nil : ToNSString(entry.value_text);
  item.focusKey = ToNSString(entry.key.empty() ? entry.title : entry.key);
  item.hintText = [NSString
      stringWithFormat:NSLocalizedString(@"Opens %1$@, %2$@.",
                                         @"settings hub view controller: hint of a search result "
                                         @"(page, then section)"),
                       page, section];
  return item;
}

@interface XeniaSettingsAppearanceViewController : XESheetTableViewController
- (instancetype)initWithStatusHandler:(XeniaSettingsStatusHandler)on_status;
@end

@implementation XeniaSettingsAppearanceViewController {
  XeniaSettingsStatusHandler on_status_;
}

- (instancetype)initWithStatusHandler:(XeniaSettingsStatusHandler)on_status {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = NSLocalizedString(@"Appearance", @"settings hub view controller: title");
    on_status_ = [on_status copy];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [on_status_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  XEUseArtWash(self);
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 64.0;
  // The Game Colours swatch follows the game the Library shows.
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(libraryBackgroundChanged:)
                                               name:XeniaLibraryBackgroundDidChangeNotification
                                             object:nil];
}

- (void)libraryBackgroundChanged:(NSNotification*)__unused notification {
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, 2)]
                withRowAnimation:UITableViewRowAnimationNone];
}

// Library Background, its motion, then the app icon.
enum : NSInteger { kAppearanceBackground, kAppearanceMotion, kAppearanceIcon, kAppearanceCount };

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return kAppearanceCount;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == kAppearanceMotion ? 2 : 3;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  switch (section) {
    case kAppearanceBackground:
      return NSLocalizedString(@"Library Background",
                               @"settings appearance: section of the Library's background choices");
    case kAppearanceMotion:
      return NSLocalizedString(@"Background Motion",
                               @"settings appearance: section of how the Library background moves");
    default:
      return NSLocalizedString(@"App Icon", @"settings hub view controller: user-visible text");
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  return section == kAppearanceMotion
             ? NSLocalizedString(@"It stays still when Reduce Motion or Low Power Mode is on.",
                                 @"settings appearance: footer under Background Motion")
             : nil;
}

// A row of Library Background or Background Motion: a title, a line under it and a check.
- (UITableViewCell*)backgroundCellForRowAtIndexPath:(NSIndexPath*)indexPath
                                          tableView:(UITableView*)tableView {
  static NSString* const kCellIdentifier = @"XeniaSettingsAppearanceBackgroundCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.textLabel.numberOfLines = 1;
    cell.detailTextLabel.numberOfLines = 2;
  }
  const NSInteger row = indexPath.row;
  NSString* title = nil;
  NSString* detail = nil;
  BOOL selected = NO;
  if (indexPath.section == kAppearanceBackground) {
    const XeniaLibraryBackground background = XeniaLibraryBackground(row);
    selected = XeniaLibraryBackgroundSetting() == background;
    switch (background) {
      case XeniaLibraryBackgroundGameColours:
        title = NSLocalizedString(@"Game Colours",
                                  @"settings appearance: Library background from the game's art");
        detail = NSLocalizedString(@"Matches the game you played last.",
                                   @"settings appearance: detail of Game Colours");
        break;
      case XeniaLibraryBackgroundGreen:
        title = NSLocalizedString(@"XeniOS Green",
                                  @"settings appearance: Library background in the app's greens");
        detail = NSLocalizedString(@"The same greens for every game.",
                                   @"settings appearance: detail of XeniOS Green");
        break;
      case XeniaLibraryBackgroundOff:
        title = NSLocalizedString(@"Off", @"settings appearance: no Library background");
        detail = NSLocalizedString(@"A plain background.", @"settings appearance: detail of Off");
        break;
    }
    cell.imageView.image = XeniaLibraryBackgroundSwatch(
        background == XeniaLibraryBackgroundOff ? nil : XeniaLibraryBackgroundColours(background),
        29.0);
  } else {
    const XeniaLibraryBackgroundMotion motion = XeniaLibraryBackgroundMotion(row);
    selected = XeniaLibraryBackgroundMotionSetting() == motion;
    if (motion == XeniaLibraryBackgroundMotionSubtle) {
      title = NSLocalizedString(@"Subtle", @"settings appearance: the background moves slowly");
      detail = NSLocalizedString(@"The colours drift slowly.",
                                 @"settings appearance: detail of Subtle motion");
    } else {
      title = NSLocalizedString(@"Still", @"settings appearance: the background doesn't move");
      detail = NSLocalizedString(@"The colours don't move.",
                                 @"settings appearance: detail of Still motion");
    }
    cell.imageView.image = nil;
  }
  cell.textLabel.text = title;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text = detail;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0, UIFontWeightSemibold);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  cell.accessoryType = selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  XEApplyAccessibility(cell, title, detail, nil,
                       UIAccessibilityTraitButton |
                           (selected ? UIAccessibilityTraitSelected : UIAccessibilityTraitNone));
  return cell;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section != kAppearanceIcon) {
    return [self backgroundCellForRowAtIndexPath:indexPath tableView:tableView];
  }
  static NSString* const kCellIdentifier = @"XeniaSettingsAppearanceIconCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.textLabel.numberOfLines = 1;
    cell.detailTextLabel.numberOfLines = 2;
  }

  const NSInteger row = indexPath.row;
  NSString* icon_name = XeniaAppIconNameForRow(row);
  NSString* current_name = [[UIApplication sharedApplication] alternateIconName];
  const BOOL selected = XeniaAppIconNamesEqual(current_name, icon_name);
  const BOOL supported = [[UIApplication sharedApplication] supportsAlternateIcons];

  cell.textLabel.text = XeniaAppIconTitleForRow(row);
  cell.textLabel.textColor = supported ? [XeniaTheme textPrimary] : [XeniaTheme textSecondary];
  cell.detailTextLabel.text = XeniaAppIconSubtitleForRow(row);
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0, UIFontWeightSemibold);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  cell.accessoryType = selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
  cell.selectionStyle =
      supported ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;

  cell.imageView.image = XeniaAppIconPreviewForRow(row)
                             ?: XERowSymbolImage(XeniaAppIconSymbolForRow(row), nil);
  XEApplyAccessibility(cell, cell.textLabel.text, cell.detailTextLabel.text,
                       supported ? NSLocalizedString(@"Selects this app icon.",
                                                     @"settings hub view controller: detail text")
                                 : nil,
                       supported ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == kAppearanceBackground) {
    XeniaSetLibraryBackgroundSetting(XeniaLibraryBackground(indexPath.row));
    return;
  }
  if (indexPath.section == kAppearanceMotion) {
    XeniaSetLibraryBackgroundMotionSetting(XeniaLibraryBackgroundMotion(indexPath.row));
    return;
  }
  UIApplication* application = [UIApplication sharedApplication];
  if (!application.supportsAlternateIcons) {
    if (on_status_) {
      on_status_(NSLocalizedString(@"This build does not include alternate app icons.",
                                   @"settings hub view controller: user-visible text"));
    }
    return;
  }

  NSString* icon_name = XeniaAppIconNameForRow(indexPath.row);
  if (XeniaAppIconNamesEqual(application.alternateIconName, icon_name)) {
    return;
  }

  NSString* title = XeniaAppIconTitleForRow(indexPath.row);
  [application setAlternateIconName:icon_name
                  completionHandler:^(NSError* error) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                      if (error) {
                        if (on_status_) {
                          on_status_(
                              [NSString stringWithFormat:
                                            NSLocalizedString(
                                                @"App icon: %@",
                                                @"settings hub view controller: user-visible text"),
                                            error.localizedDescription]);
                        }
                        return;
                      }
                      // Only the checkmarks move, so reload the three rows, not the table.
                      [self.tableView
                          reloadRowsAtIndexPaths:@[
                            [NSIndexPath indexPathForRow:0 inSection:kAppearanceIcon],
                            [NSIndexPath indexPathForRow:1 inSection:kAppearanceIcon],
                            [NSIndexPath indexPathForRow:2 inSection:kAppearanceIcon],
                          ]
                            withRowAnimation:UITableViewRowAnimationNone];
                      if (on_status_) {
                        on_status_(
                            [NSString stringWithFormat:
                                          NSLocalizedString(
                                              @"App icon set to %@.",
                                              @"settings hub view controller: user-visible text"),
                                          title]);
                      }
                    });
                  }];
}

@end

@interface XeniaSettingsActionListItem : NSObject
@property(nonatomic, assign) XeniaSettingsHubAction action;
@property(nonatomic, assign) BOOL selectable;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* subtitle;
@property(nonatomic, copy) NSString* symbolName;
@property(nonatomic, retain) UIColor* tintColor;
+ (instancetype)itemWithAction:(XeniaSettingsHubAction)action
                         title:(NSString*)title
                      subtitle:(NSString*)subtitle
                    symbolName:(NSString*)symbol_name
                     tintColor:(UIColor*)tint_color;
+ (instancetype)infoItemWithTitle:(NSString*)title
                         subtitle:(NSString*)subtitle
                       symbolName:(NSString*)symbol_name;
@end

@implementation XeniaSettingsActionListItem

+ (instancetype)itemWithAction:(XeniaSettingsHubAction)action
                         title:(NSString*)title
                      subtitle:(NSString*)subtitle
                    symbolName:(NSString*)symbol_name
                     tintColor:(UIColor*)tint_color {
  XeniaSettingsActionListItem* item =
      [[[XeniaSettingsActionListItem alloc] init] autorelease];
  item.action = action;
  item.selectable = YES;
  item.title = title;
  item.subtitle = subtitle;
  item.symbolName = symbol_name;
  item.tintColor = tint_color;
  return item;
}

+ (instancetype)infoItemWithTitle:(NSString*)title
                         subtitle:(NSString*)subtitle
                       symbolName:(NSString*)symbol_name {
  XeniaSettingsActionListItem* item =
      [[[XeniaSettingsActionListItem alloc] init] autorelease];
  item.action = XeniaSettingsHubActionNone;
  item.selectable = NO;
  item.title = title;
  item.subtitle = subtitle;
  item.symbolName = symbol_name;
  item.tintColor = nil;
  return item;
}

- (void)dealloc {
  [_title release];
  [_subtitle release];
  [_symbolName release];
  [_tintColor release];
  [super dealloc];
}

@end

@interface XeniaSettingsActionListViewController : XESheetTableViewController
- (instancetype)initWithTitle:(NSString*)title
                        items:(NSArray*)items
                actionHandler:(XeniaSettingsHubActionHandler)action_handler;
// Groups of items, each under its heading.
- (instancetype)initWithTitle:(NSString*)title
                     sections:(NSArray<NSArray*>*)sections
                sectionTitles:(NSArray<NSString*>*)section_titles
                actionHandler:(XeniaSettingsHubActionHandler)action_handler;
@end

@implementation XeniaSettingsActionListViewController {
  NSArray<NSArray*>* sections_;
  NSArray<NSString*>* section_titles_;
  XeniaSettingsHubActionHandler action_handler_;
}

- (instancetype)initWithTitle:(NSString*)title
                        items:(NSArray*)items
                actionHandler:(XeniaSettingsHubActionHandler)action_handler {
  return [self initWithTitle:title
                    sections:@[ items ]
               sectionTitles:nil
               actionHandler:action_handler];
}

- (instancetype)initWithTitle:(NSString*)title
                     sections:(NSArray<NSArray*>*)sections
                sectionTitles:(NSArray<NSString*>*)section_titles
                actionHandler:(XeniaSettingsHubActionHandler)action_handler {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = title;
    sections_ = [sections retain];
    section_titles_ = [section_titles retain];
    action_handler_ = [action_handler copy];
  }
  return self;
}

- (void)dealloc {
  [sections_ release];
  [section_titles_ release];
  [action_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  XEUseArtWash(self);
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 72.0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return static_cast<NSInteger>(sections_.count);
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)section {
  return static_cast<NSUInteger>(section) < section_titles_.count ? section_titles_[section] : nil;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  return static_cast<NSInteger>(sections_[section].count);
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaSettingsActionCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.textLabel.numberOfLines = 1;
    cell.detailTextLabel.numberOfLines = 3;
  }

  XeniaSettingsActionListItem* item = sections_[indexPath.section][indexPath.row];
  cell.textLabel.text = item.title;
  cell.textLabel.textColor = item.selectable ? [XeniaTheme textPrimary]
                                             : [XeniaTheme textSecondary];
  cell.detailTextLabel.text = item.subtitle;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0,
                      UIFontWeightSemibold);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleSubheadline, 15.0,
                      UIFontWeightRegular);
  cell.selectionStyle = item.selectable ? UITableViewCellSelectionStyleDefault
                                        : UITableViewCellSelectionStyleNone;
  cell.accessoryType = item.selectable ? UITableViewCellAccessoryDisclosureIndicator
                                       : UITableViewCellAccessoryNone;
  cell.imageView.image = XERowSymbolImage(item.symbolName, item.tintColor);
  XEApplyAccessibility(
      cell, item.title, item.subtitle,
      item.selectable ? NSLocalizedString(@"Performs this settings action.",
                                          @"settings hub view controller: title")
                      : nil,
      item.selectable ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  XeniaSettingsActionListItem* item = sections_[indexPath.section][indexPath.row];
  if (item.selectable && item.action == XeniaSettingsHubActionManageExternalFolders) {
    XeniaIOSExternalFoldersViewController* folders =
        [[[XeniaIOSExternalFoldersViewController alloc] init] autorelease];
    [self.navigationController pushViewController:folders animated:YES];
    return;
  }
  if (!item.selectable || !action_handler_) {
    return;
  }
  action_handler_(item.action);
}

@end

// The touch layout actions, in the order the Controls and Touch Layouts pages list them. The
// three that act on the running game are left out when no game is running.
static NSArray* XeniaTouchLayoutActionItems(BOOL no_game_running) {
  NSMutableArray* items = [NSMutableArray array];
  [items addObject:[XeniaSettingsActionListItem
                       itemWithAction:XeniaSettingsHubActionChooseGameTouchLayout
                                title:NSLocalizedString(@"Edit Game Touch Layout",
                                                        @"settings hub view controller: title")
                             subtitle:NSLocalizedString(
                                          @"Pick a game and arrange its touch controls.",
                                          @"settings hub view controller: title")
                           symbolName:@"gamecontroller.fill"
                            tintColor:nil]];
  [items addObject:[XeniaSettingsActionListItem
                       itemWithAction:XeniaSettingsHubActionOpenTouchLayoutLibrary
                                title:NSLocalizedString(@"Layout Library",
                                                        @"settings hub view controller: title")
                             subtitle:NSLocalizedString(
                                          @"All your touch layouts.",
                                          @"settings hub view controller: title")
                           symbolName:@"rectangle.grid.2x2.fill"
                            tintColor:nil]];
  if (!no_game_running) {
    [items addObject:[XeniaSettingsActionListItem
                         itemWithAction:XeniaSettingsHubActionEditTouchControls
                                  title:NSLocalizedString(@"Edit Running Touch Layout",
                                                          @"settings hub view controller: title")
                               subtitle:NSLocalizedString(
                                            @"Arrange the touch controls in the running game.",
                                            @"settings hub view controller: title")
                             symbolName:@"hand.draw.fill"
                              tintColor:nil]];
  }
  [items addObject:[XeniaSettingsActionListItem
                       itemWithAction:XeniaSettingsHubActionImportTouchLayout
                                title:NSLocalizedString(@"Import Layout",
                                                        @"settings hub view controller: title")
                             subtitle:NSLocalizedString(@"Add a layout from Files.",
                                                        @"settings hub view controller: title")
                           symbolName:@"square.and.arrow.down.fill"
                            tintColor:nil]];
  if (!no_game_running) {
    [items addObject:[XeniaSettingsActionListItem
                         itemWithAction:XeniaSettingsHubActionExportTouchLayout
                                  title:NSLocalizedString(@"Export Running Layout",
                                                          @"settings hub view controller: title")
                               subtitle:NSLocalizedString(
                                            @"Share the running game's layout.",
                                            @"settings hub view controller: title")
                             symbolName:@"square.and.arrow.up.fill"
                              tintColor:nil]];
    [items addObject:[XeniaSettingsActionListItem
                         itemWithAction:XeniaSettingsHubActionResetTouchLayout
                                  title:NSLocalizedString(@"Reset Running Layout",
                                                          @"settings hub view controller: title")
                               subtitle:NSLocalizedString(
                                            @"Go back to the standard layout.",
                                            @"settings hub view controller: title")
                             symbolName:@"arrow.counterclockwise"
                              tintColor:nil]];
  }
  return items;
}

@interface XeniaSettingsControlsViewController : XESheetTableViewController
- (instancetype)initWithActionHandler:(XeniaSettingsHubActionHandler)action_handler
                        noGameRunning:(BOOL)no_game_running;
@end

@implementation XeniaSettingsControlsViewController {
  NSArray* layout_items_;
  XeniaSettingsHubActionHandler action_handler_;
}

- (instancetype)initWithActionHandler:(XeniaSettingsHubActionHandler)action_handler
                        noGameRunning:(BOOL)no_game_running {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = NSLocalizedString(@"Controls", @"settings hub view controller: title");
    layout_items_ = [XeniaTouchLayoutActionItems(no_game_running) retain];
    action_handler_ = [action_handler copy];
  }
  return self;
}

- (void)dealloc {
  [layout_items_ release];
  [action_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  XEUseArtWash(self);
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 72.0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? static_cast<NSInteger>(layout_items_.count) : 1;
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)section {
  // The second section is one row named like it, so it needs no header.
  return section == 0 ? NSLocalizedString(@"Touch Layouts",
                                          @"settings hub view controller: user-visible text")
                      : nil;
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger)section {
  if (section != 0) {
    return nil;
  }
  return NSLocalizedString(@"Controllers work as soon as they connect. The touch controls show "
                           @"when none is connected.",
                           @"settings hub view controller: footer");
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaSettingsControlsCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.textLabel.numberOfLines = 1;
    cell.detailTextLabel.numberOfLines = 3;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  }

  NSString* title = nil;
  NSString* subtitle = nil;
  NSString* symbol_name = nil;
  UIColor* tint_color = nil;
  if (indexPath.section == 0) {
    XeniaSettingsActionListItem* item = [layout_items_ objectAtIndex:indexPath.row];
    title = item.title;
    subtitle = item.subtitle;
    symbol_name = item.symbolName;
    tint_color = item.tintColor;
  } else {
    title = NSLocalizedString(@"Touch Controls", @"settings hub view controller: title");
    subtitle = NSLocalizedString(@"Haptics and how the controls look.",
                                 @"settings hub view controller: title");
    symbol_name = @"hand.tap.fill";
    tint_color = nil;
  }

  cell.textLabel.text = title;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text = subtitle;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0,
                      UIFontWeightSemibold);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleSubheadline, 15.0,
                      UIFontWeightRegular);

  cell.imageView.image = XERowSymbolImage(symbol_name, tint_color);
  XEApplyAccessibility(cell, title, subtitle,
                       NSLocalizedString(@"Opens this controls option.",
                                         @"settings hub view controller: title"),
                       UIAccessibilityTraitButton);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 0) {
    XeniaSettingsActionListItem* item = [layout_items_ objectAtIndex:indexPath.row];
    if (action_handler_) {
      action_handler_(item.action);
    }
    return;
  }

  XeniaConfigViewController* config = [[XeniaConfigViewController alloc]
      initWithCatalogKind:IOSConfigCatalogKind::kControls
                    style:UITableViewStyleInsetGrouped];
  config.showsRootDismissButton = NO;
  [self.navigationController pushViewController:config animated:YES];
  [config release];
}

@end

@interface XeniaSettingsAboutViewController : XESheetTableViewController
@end

@interface XeniaSettingsPrivacyViewController : XESheetTableViewController
@end

namespace {

typedef NS_ENUM(NSInteger, XeniaAboutSection) {
  XeniaAboutSectionBuild = 0,
  XeniaAboutSectionUpdates,
  XeniaAboutSectionLinks,  // Acknowledgements; the project's links are on the hub.
  XeniaAboutSectionCount,
};

typedef NS_ENUM(NSInteger, XeniaPrivacySection) {
  XeniaPrivacySectionOnline = 0,
  XeniaPrivacySectionDiagnostics,  // With its own footer; empty without an endpoint.
  XeniaPrivacySectionCount,
};

constexpr NSInteger kPrivacyPendingReportsRow = 1;  // After Share Diagnostics.

}  // namespace

// Online Features and diagnostics: what the app sends, apart from the version and links in About.
@implementation XeniaSettingsPrivacyViewController

- (instancetype)init {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = NSLocalizedString(@"Privacy", @"settings hub view controller: title");
  }
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  XEUseArtWash(self);
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  // Back from Pending Reports, or Online Features turned on elsewhere.
  [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return XeniaPrivacySectionCount;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  if (section == XeniaPrivacySectionDiagnostics) {
    return xe_ios_diagnostics_available() ? 2 : 0;
  }
  return 1;
}

- (NSString*)tableView:(UITableView* __unused)tableView
titleForFooterInSection:(NSInteger)section {
  if (section == XeniaPrivacySectionOnline) {
    if (!xe_ios_diagnostics_available()) {
      return NSLocalizedString(
          @"Update checks, cover art, compatibility data, discussions and reports, System Link "
          @"over the internet, and which game you're playing (counted without your name or "
          @"device). Games still run with this off.",
          @"settings hub view controller: user-visible text");
    }
    return NSLocalizedString(
        @"Update checks, cover art, compatibility data, discussions and reports, System Link "
        @"over the internet, which game you're playing (counted without your name or device), "
        @"and diagnostics. Games still run with this off. To stop only diagnostics, turn off "
        @"Share Diagnostics.",
        @"settings hub view controller: footer of the Online Features switch");
  }
  if (section == XeniaPrivacySectionDiagnostics) {
    if (!xe_ios_diagnostics_available()) {
      return nil;
    }
    return NSLocalizedString(
        @"Sends crash reports and performance statistics: device model, iOS version, XeniOS "
        @"version, the game's title ID, frame rate, load time, shader compiles, GPU errors, "
        @"temperature, memory use and display settings. Never your files, file names, Apple "
        @"Account or location. Logs are only sent when you share them yourself.",
        @"settings hub view controller: footer of the Share Diagnostics switch");
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaSettingsPrivacyCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                   reuseIdentifier:kCellIdentifier] autorelease];
  }
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.accessoryView = nil;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.textLabel.numberOfLines = 0;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  [self configurePrivacyCell:cell indexPath:indexPath];
  if ([cell.accessoryView isKindOfClass:[UISwitch class]]) {
    // The switch carries the label and value; the cell stays a container.
    cell.isAccessibilityElement = NO;
    cell.textLabel.isAccessibilityElement = NO;
    cell.detailTextLabel.isAccessibilityElement = NO;
    return cell;
  }
  XEApplyAccessibility(cell, cell.textLabel.text, cell.detailTextLabel.text, nil,
                       UIAccessibilityTraitButton);
  cell.isAccessibilityElement = YES;
  return cell;
}

- (void)configurePrivacyCell:(UITableViewCell*)cell indexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == XeniaPrivacySectionDiagnostics &&
      indexPath.row == kPrivacyPendingReportsRow) {
    cell.textLabel.text =
        NSLocalizedString(@"Pending Reports", @"settings hub view controller: label text");
    const NSUInteger pending = xe_ios_diagnostics_pending_count();
    cell.detailTextLabel.text =
        pending ? [NSString localizedStringWithFormat:@"%lu", (unsigned long)pending]
                : NSLocalizedString(@"None", @"settings hub view controller: no pending reports");
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
    return;
  }

  if (indexPath.section == XeniaPrivacySectionDiagnostics) {
    cell.textLabel.text =
        NSLocalizedString(@"Share Diagnostics", @"settings hub view controller: label text");
    cell.detailTextLabel.text = @"";
    UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
    // Online Features off sends nothing, diagnostics included.
    toggle.on = xe_ios_online_features_enabled() && xe_ios_share_diagnostics_enabled();
    toggle.enabled = xe_ios_online_features_enabled();
    toggle.accessibilityLabel = cell.textLabel.text;
    [toggle addTarget:self
                  action:@selector(shareDiagnosticsSwitchChanged:)
        forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
    return;
  }

  if (indexPath.section == XeniaPrivacySectionOnline) {
    cell.textLabel.text =
        NSLocalizedString(@"Online Features", @"settings hub view controller: label text");
    cell.detailTextLabel.text = @"";
    UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
    toggle.on = xe_ios_online_features_enabled();
    toggle.accessibilityLabel = cell.textLabel.text;
    [toggle addTarget:self
                  action:@selector(onlineFeaturesSwitchChanged:)
        forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
  }
}

- (void)onlineFeaturesSwitchChanged:(UISwitch*)sender {
  xe_ios_set_online_features_enabled(sender.isOn);
  if (sender.isOn && xe_ios_diagnostics_available()) {
    // Share Diagnostics and its footer are right below: no separate notice later. A
    // build without an endpoint shows neither, so a later one still asks.
    xe_ios_set_diagnostics_notice_answered();
  }
  xe_ios_diagnostics_settings_changed();
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:XeniaPrivacySectionDiagnostics]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (void)shareDiagnosticsSwitchChanged:(UISwitch*)sender {
  xe_ios_set_share_diagnostics_enabled(sender.isOn);
  // Off deletes what is queued; the Pending Reports count follows.
  xe_ios_diagnostics_settings_changed();
  [self.tableView
      reloadRowsAtIndexPaths:@[ [NSIndexPath indexPathForRow:kPrivacyPendingReportsRow
                                                   inSection:XeniaPrivacySectionDiagnostics] ]
            withRowAnimation:UITableViewRowAnimationNone];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == XeniaPrivacySectionDiagnostics &&
      indexPath.row == kPrivacyPendingReportsRow) {
    [self.navigationController
        pushViewController:xe_ios_diagnostics_make_pending_reports_controller()
                  animated:YES];
  }
}

@end

@implementation XeniaSettingsAboutViewController {
  XeniaUpdateInfo* update_info_;
  BOOL update_checking_;
}

- (instancetype)init {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = NSLocalizedString(@"About", @"settings hub view controller: title");
  }
  return self;
}

- (void)dealloc {
  [update_info_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  XEUseArtWash(self);
  self.tableView.tableHeaderView = [self makeAboutHeaderView];
  [self refreshUpdateStatusForced:NO];
}

// The app icon, name and version, centered above the list.
- (UIView*)makeAboutHeaderView {
  UIImageView* icon =
      [[[UIImageView alloc] initWithImage:[UIImage imageNamed:@"AppIcon60x60"]] autorelease];
  icon.translatesAutoresizingMaskIntoConstraints = NO;
  icon.layer.cornerRadius = 18.0;
  icon.layer.cornerCurve = kCACornerCurveContinuous;
  icon.clipsToBounds = YES;

  UILabel* name = [[[UILabel alloc] init] autorelease];
  name.text = XeniaBundleValue((NSString*)kCFBundleNameKey) ?: @"XeniOS";
  name.font = xe_scaled_system_font(UIFontTextStyleTitle2, 22.0, UIFontWeightBold);
  name.adjustsFontForContentSizeCategory = YES;
  name.textColor = [XeniaTheme textPrimary];

  NSString* version = XeniaBundleValue(@"CFBundleShortVersionString") ?: @"";
  NSString* build = XeniaBundleValue((NSString*)kCFBundleVersionKey) ?: @"";
  UILabel* detail = [[[UILabel alloc] init] autorelease];
  detail.text =
      version.length
          ? [NSString stringWithFormat:NSLocalizedString(@"Version %@",
                                                         @"settings hub view controller: about "
                                                         @"header; %@ is the version"),
                                       build.length && ![build isEqualToString:version]
                                           ? [NSString stringWithFormat:@"%@ (%@)", version, build]
                                           : version]
          : NSLocalizedString(
                @"Development Build",
                @"settings hub view controller: build name shown when no version is available");
  detail.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  detail.adjustsFontForContentSizeCategory = YES;
  detail.textColor = [XeniaTheme textSecondary];

  UIStackView* stack =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ icon, name, detail ]] autorelease];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.alignment = UIStackViewAlignmentCenter;
  stack.spacing = 4.0;
  [stack setCustomSpacing:12.0 afterView:icon];
  stack.isAccessibilityElement = YES;
  stack.accessibilityLabel = [NSString stringWithFormat:@"%@, %@", name.text, detail.text];
  stack.accessibilityTraits = UIAccessibilityTraitHeader;
  stack.translatesAutoresizingMaskIntoConstraints = NO;

  UIView* header = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 320, 180)] autorelease];
  [header addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [icon.widthAnchor constraintEqualToConstant:80.0],
    [icon.heightAnchor constraintEqualToConstant:80.0],
    [stack.centerXAnchor constraintEqualToAnchor:header.centerXAnchor],
    [stack.topAnchor constraintEqualToAnchor:header.topAnchor constant:16.0],
  ]];
  return header;
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  // Size the header to its content, which grows with Dynamic Type.
  UIView* header = self.tableView.tableHeaderView;
  UIView* stack = header.subviews.firstObject;
  if (!stack) {
    return;
  }
  const CGFloat height =
      16.0 + [stack systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].height + 8.0;
  if (fabs(header.frame.size.height - height) > 0.5) {
    CGRect frame = header.frame;
    frame.size.height = height;
    header.frame = frame;
    self.tableView.tableHeaderView = header;
  }
}

- (void)refreshUpdateStatusForced:(BOOL)forced {
  if (update_checking_) {
    return;
  }
  update_checking_ = YES;
  [self reloadUpdatesSection];
  // The block retains self while the check is in flight, so the controller
  // stays alive long enough to render the result.
  xe_check_for_update(forced, ^(XeniaUpdateInfo* info) {
    update_checking_ = NO;
    [update_info_ release];
    update_info_ = [info retain];
    [self reloadUpdatesSection];
  });
}

- (void)reloadUpdatesSection {
  // Skip until the table is on screen; cellForRow renders the current state on
  // first appearance regardless.
  if (!self.isViewLoaded || !self.tableView.window) {
    return;
  }
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:XeniaAboutSectionUpdates]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return XeniaAboutSectionCount;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  switch (section) {
    case XeniaAboutSectionBuild:
      return 0;  // The header shows the name and version.
    default:
      return 1;  // The update status, or Acknowledgements.
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView
titleForHeaderInSection:(NSInteger)section {
  return nil;
}

- (NSString*)tableView:(UITableView* __unused)tableView
titleForFooterInSection:(NSInteger)section {
  if (section == XeniaAboutSectionLinks) {
    NSString* memorial = NSLocalizedString(
        @"XeniOS is one of several apps with dedication keeping the memory of \"Lily\" alive "
        @"11/03/2023",
        @"settings hub view controller: footer under the links");
    NSString* build = xe_user_facing_build_label(xe_current_compat_report_build_info());
    return build.length ? [NSString stringWithFormat:@"%@\n\n%@", build, memorial] : memorial;
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell = [self aboutCellForTableView:tableView indexPath:indexPath];
  if ([cell.accessoryView isKindOfClass:[UISwitch class]]) {
    // The switch carries the label and value; the cell stays a container, and its
    // labels are hidden so VoiceOver doesn't read the title twice.
    cell.isAccessibilityElement = NO;
    cell.textLabel.isAccessibilityElement = NO;
    cell.detailTextLabel.isAccessibilityElement = NO;
    return cell;
  }
  const BOOL actionable = cell.selectionStyle != UITableViewCellSelectionStyleNone;
  NSString* value = cell.detailTextLabel.text.length ? cell.detailTextLabel.text : nil;
  XEApplyAccessibility(cell, cell.textLabel.text, value, nil,
                       actionable ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText);
  cell.isAccessibilityElement = YES;
  return cell;
}

- (UITableViewCell*)aboutCellForTableView:(UITableView*)tableView
                                indexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaSettingsAboutCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                   reuseIdentifier:kCellIdentifier] autorelease];
  }
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.accessoryView = nil;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.imageView.image = nil;

  if (indexPath.section == XeniaAboutSectionBuild) {
    NSArray* titles = @[
      NSLocalizedString(@"Name", @"settings hub view controller: title"),
      NSLocalizedString(@"Version", @"settings hub view controller: title"),
      NSLocalizedString(@"Bundle", @"settings hub view controller: title")
    ];
    NSString* version = XeniaBundleValue(@"CFBundleShortVersionString") ?: @"";
    NSString* build = XeniaBundleValue((NSString*)kCFBundleVersionKey) ?: @"";
    NSString* version_text = version.length && build.length
                                 ? [NSString stringWithFormat:@"%@ (%@)", version, build]
                                 : (version.length ? version : build);
    NSArray* values = @[
      XeniaBundleValue((NSString*)kCFBundleNameKey) ?: @"XeniOS",
      version_text.length
          ? version_text
          : NSLocalizedString(
                @"Development Build",
                @"settings hub view controller: build name shown when no version is available"),
      [[NSBundle mainBundle] bundleIdentifier] ?: @""
    ];
    cell.textLabel.text = [titles objectAtIndex:indexPath.row];
    cell.detailTextLabel.text = [values objectAtIndex:indexPath.row];
    return cell;
  }

  if (indexPath.section == XeniaAboutSectionUpdates) {
    if (!xe_ios_online_features_enabled() && !update_checking_) {
      cell.textLabel.text =
          NSLocalizedString(@"Updates", @"settings hub view controller: label text");
      cell.detailTextLabel.text =
          NSLocalizedString(@"Online features off", @"settings hub view controller: detail text");
      cell.selectionStyle = UITableViewCellSelectionStyleDefault;
      return cell;
    }
    if (update_checking_) {
      cell.textLabel.text =
          NSLocalizedString(@"Checking for Updates…", @"settings hub view controller: label text");
      cell.detailTextLabel.text = @"";
      UIActivityIndicatorView* spinner = [[[UIActivityIndicatorView alloc]
          initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
      [spinner startAnimating];
      cell.accessoryView = spinner;
      return cell;
    }
    switch (update_info_.status) {
      case XeniaUpdateStatusUpdateAvailable:
        cell.textLabel.text =
            NSLocalizedString(@"Update Available", @"settings hub view controller: label text");
        cell.detailTextLabel.text = update_info_.latestVersionText ?: @"";
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        break;
      case XeniaUpdateStatusUpToDate:
        cell.textLabel.text =
            NSLocalizedString(@"Up to Date", @"settings hub view controller: label text");
        cell.detailTextLabel.text =
            NSLocalizedString(@"Check Now", @"settings hub view controller: detail text");
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        break;
      case XeniaUpdateStatusDevelopmentBuild:
        cell.textLabel.text = NSLocalizedString(
            @"Development Build",
            @"settings hub view controller: build name shown when no version is available");
        cell.detailTextLabel.text = @"";  // The header shows the version.
        break;
      case XeniaUpdateStatusUnknown:
      default:
        cell.textLabel.text =
            NSLocalizedString(@"Check for Updates", @"settings hub view controller: label text");
        cell.detailTextLabel.text = @"";
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        break;
    }
    return cell;
  }

  cell.textLabel.text =
      NSLocalizedString(@"Acknowledgements", @"settings hub view controller: title");
  cell.detailTextLabel.text = @"";
  cell.imageView.image = XERowSymbolImage(@"doc.text.fill", nil);
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == XeniaAboutSectionBuild) {
    return;
  }
  if (indexPath.section == XeniaAboutSectionUpdates) {
    if (!xe_ios_online_features_enabled()) {
      XEPresentOKAlert(self,
                       NSLocalizedString(@"Online Features Are Off",
                                         @"settings hub view controller: user-visible text"),
                       xe_ios_online_features_off_message());
      return;
    }
    if (update_checking_ || update_info_.status == XeniaUpdateStatusDevelopmentBuild) {
      return;
    }
    NSURL* download_url = update_info_.downloadURLString.length
                              ? [NSURL URLWithString:update_info_.downloadURLString]
                              : nil;
    if (update_info_.status == XeniaUpdateStatusUpdateAvailable &&
        xe_ios_is_trusted_release_url(download_url)) {
      XeniaOpenSettingsURL(update_info_.downloadURLString);
    } else {
      // Up to date / unknown: tap re-checks immediately.
      [self refreshUpdateStatusForced:YES];
    }
    return;
  }
  if (indexPath.section != XeniaAboutSectionLinks) {
    return;
  }
  XeniaAcknowledgementsViewController* acknowledgements =
      [[XeniaAcknowledgementsViewController alloc] init];
  [self.navigationController pushViewController:acknowledgements animated:YES];
  [acknowledgements release];
}

@end

@interface XeniaSettingsHubViewController () <UISearchResultsUpdating, UISearchControllerDelegate>
- (UIViewController*)makeDetailControllerForItem:(XeniaSettingsHubItem*)item;
- (UIViewController*)makeRawDetailControllerForItem:(XeniaSettingsHubItem*)item;
@end

@implementation XeniaSettingsHubViewController {
  xe::ui::IOSWindowedAppContext* app_context_;
  XeniaSettingsStatusHandler on_status_;
  XeniaSettingsHubActionHandler action_handler_;
  XeniaSettingsInitialSection initial_section_;
  NSArray* all_sections_;
  NSArray* sections_;
  UISearchController* search_controller_;
  void (^dismissal_handler_)(void);
  BOOL shows_close_button_;
  BOOL no_game_running_;
  // Every searchable setting with its current value, built once per search session.
  std::vector<IOSConfigSearchEntry> search_index_;
  BOOL search_index_valid_;
  // The category open beside this list on iPad, or -1.
  NSInteger sidebar_kind_;
}

@synthesize dismissalHandler = dismissal_handler_;
@synthesize actionHandler = action_handler_;
@synthesize showsCloseButton = shows_close_button_;
@synthesize noGameRunning = no_game_running_;

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(XeniaSettingsStatusHandler)on_status {
  return [self initWithAppContext:app_context
                         onStatus:on_status
                   initialSection:XeniaSettingsInitialSectionMain];
}

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(XeniaSettingsStatusHandler)on_status
                    initialSection:(XeniaSettingsInitialSection)initial_section {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    app_context_ = app_context;
    on_status_ = [on_status copy];
    initial_section_ = initial_section;
    sidebar_kind_ = -1;
    all_sections_ = [[self buildSections] retain];
    sections_ = [all_sections_ retain];
    self.title = NSLocalizedString(@"Settings", @"settings hub view controller: title");
  }
  return self;
}

- (void)dealloc {
  [on_status_ release];
  [action_handler_ release];
  [all_sections_ release];
  [sections_ release];
  [search_controller_ release];
  [dismissal_handler_ release];
  [super dealloc];
}

- (XeniaSettingsHubItem*)linkItemWithTitle:(NSString*)title
                                   subtitle:(NSString*)subtitle
                                   keywords:(NSString*)keywords
                                 symbolName:(NSString*)symbol_name
                                  tintColor:(UIColor*)tint_color
                                        url:(NSString*)url {
  XeniaSettingsHubItem* item = [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindLink
                                                            title:title
                                                         subtitle:subtitle
                                                       symbolName:symbol_name
                                                        tintColor:tint_color];
  item.keywords = keywords;
  item.url = url;
  return item;
}

- (NSArray*)buildSections {

  // What a player changes: their profile, then display, audio and controls. Everything that
  // only tunes the emulator is under Advanced, with All Settings holding every option.
  XeniaSettingsHubSection* account = [XeniaSettingsHubSection
      sectionWithTitle:@""
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindProfile
                              title:NSLocalizedString(@"Profile",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Gamertags and sign-in.",
                                                      @"settings hub view controller: title")
                         symbolName:@"person.crop.circle.fill"
                          tintColor:nil],
                 ]];

  XeniaSettingsHubSection* gameplay = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Play", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindDisplay
                              title:NSLocalizedString(@"Display",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Aspect ratio and frame rate.",
                                        @"settings hub view controller: title")
                         symbolName:@"display"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAudio
                              title:NSLocalizedString(@"Audio",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Spatial sound and head tracking.",
                                                      @"settings hub view controller: title")
                         symbolName:@"speaker.wave.2.fill"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindControls
                              title:NSLocalizedString(@"Controls",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Controllers, touch and haptics.",
                                   @"settings hub view controller: title")
                         symbolName:@"gamecontroller.fill"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindOnline
                              title:NSLocalizedString(@"Online",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Rooms and voice chat.",
                                        @"settings hub view controller: title")
                         symbolName:@"person.2.fill"
                          tintColor:nil],
                 ]];

  XeniaSettingsHubSection* games = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Games", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindLibrary
                              title:NSLocalizedString(@"Library & Storage",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Import games and free up space.",
                                   @"settings hub view controller: title")
                         symbolName:@"externaldrive.fill"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindSystemSoftware
                              title:NSLocalizedString(@"Xbox 360 System Software",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Needed for the Xbox 360 Dashboard.",
                                        @"settings hub view controller: title")
                         symbolName:@"square.stack.3d.up.fill"
                          tintColor:nil],
                 ]];

  XeniaSettingsHubItem* privacy = [XeniaSettingsHubItem
      itemWithKind:XeniaSettingsHubCategoryKindPrivacy
             title:NSLocalizedString(@"Privacy", @"settings hub view controller: title")
          subtitle:NSLocalizedString(@"Online features and diagnostics.",
                                     @"settings hub view controller: title")
        symbolName:@"hand.raised.fill"
         tintColor:nil];
  privacy.keywords = NSLocalizedString(
      @"Online Features, Share Diagnostics, Pending Reports, Crash Reports, Internet",
      @"settings hub view controller: search words for the Privacy page");

  XeniaSettingsHubSection* app = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"App", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAppearance
                              title:NSLocalizedString(@"Appearance",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Library background and app icon.",
                                        @"settings hub view controller: title")
                         symbolName:@"paintpalette.fill"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindDiagnostics
                              title:NSLocalizedString(@"Diagnostics",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Log detail and the log.",
                                                      @"settings hub view controller: title")
                         symbolName:@"waveform.path.ecg"
                          tintColor:nil],
                   privacy,
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAbout
                              title:NSLocalizedString(@"About",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Version, updates and licenses.",
                                                      @"settings hub view controller: title")
                         symbolName:@"info.circle.fill"
                          tintColor:nil],
                 ]];

  // The project's own places, on the hub itself rather than inside About, so they are easy to find.
  XeniaSettingsHubSection* community = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Community", @"settings hub view controller: title")
                 items:@[
                   [self linkItemWithTitle:NSLocalizedString(
                                               @"Support XeniOS",
                                               @"settings hub view controller: link to Ko-fi")
                                  subtitle:NSLocalizedString(
                                               @"Ko-fi", @"settings hub view controller: title")
                                  keywords:NSLocalizedString(
                                               @"Ko-fi, Kofi, Donate, Tip",
                                               @"settings hub view controller: search words for "
                                               @"the Support link")
                                symbolName:@"heart.fill"
                                 tintColor:nil
                                       url:@"https://ko-fi.com/xenios"],
                   [self linkItemWithTitle:NSLocalizedString(@"Discord",
                                                             @"settings hub view controller: title")
                                  subtitle:NSLocalizedString(
                                               @"Help and news.",
                                               @"settings hub view controller: title")
                                  keywords:NSLocalizedString(
                                               @"Chat, Server, Help",
                                               @"settings hub view controller: search words for "
                                               @"the Discord link")
                                symbolName:@"bubble.left.and.bubble.right.fill"
                                 tintColor:nil
                                       url:@"https://discord.gg/QwcTtNKTGf"],
                   [self linkItemWithTitle:@"GitHub"
                                  subtitle:NSLocalizedString(
                                               @"Source code and issues.",
                                               @"settings hub view controller: title")
                                  keywords:NSLocalizedString(
                                               @"Source, Code, Bug, Issue",
                                               @"settings hub view controller: search words for "
                                               @"the GitHub link")
                                symbolName:@"chevron.left.forwardslash.chevron.right"
                                 tintColor:nil
                                       url:@"https://github.com/xenios-jp/XeniOS"],
                   [self linkItemWithTitle:NSLocalizedString(@"Website",
                                                             @"settings hub view controller: title")
                                  subtitle:@"xenios.jp"
                                  keywords:nil
                                symbolName:@"globe"
                                 tintColor:nil
                                       url:@"https://xenios.jp"],
                 ]];

  XeniaSettingsHubSection* advanced = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Advanced", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAllSettings
                              title:NSLocalizedString(@"All Settings",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Every option, for troubleshooting.",
                                        @"settings hub view controller: title")
                         symbolName:@"list.bullet"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindGraphics
                              title:NSLocalizedString(@"Graphics",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Backend and anisotropic filtering.",
                                   @"settings hub view controller: title")
                         symbolName:@"gearshape.2.fill"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindPerformance
                              title:NSLocalizedString(@"Performance",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Shader compiles and threads.",
                                   @"settings hub view controller: title")
                         symbolName:@"bolt.fill"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindCompatibility
                              title:NSLocalizedString(@"Compatibility Workarounds",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Fixes for specific games.",
                                        @"settings hub view controller: title")
                         symbolName:@"wrench.and.screwdriver.fill"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAutomation
                              title:NSLocalizedString(@"Automation",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Open StikDebug on launch.",
                                                      @"settings hub view controller: title")
                         symbolName:@"wand.and.stars"
                          tintColor:nil],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAdvancedDebug
                              title:NSLocalizedString(@"Advanced Debug",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Debug overrides.",
                                        @"settings hub view controller: title")
                         symbolName:@"slider.horizontal.3"
                          tintColor:nil],
                 ]];

  // What a player changes first; help, news and the community close the page.
  return @[ account, gameplay, games, app, advanced, community ];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  XEUseArtWash(self);
  self.tableView.separatorInset = UIEdgeInsetsMake(0, 59.0, 0, 0);
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 52.0;
  self.tableView.sectionHeaderTopPadding = 0.0;
  // Untitled groups: only the header sets the gap, no grouped-table footer space on top of it.
  self.tableView.sectionFooterHeight = 0.0;
  self.tableView.estimatedSectionFooterHeight = 0.0;
  // Without a header view a grouped table opens with about 35 pt of its own space under the search
  // field; the first section's header alone sets the gap.
  self.tableView.tableHeaderView =
      [[[UIView alloc] initWithFrame:CGRectMake(0.0, 0.0, 0.0, 1.0)] autorelease];
  // A large title on the leading side, ✕ in the bar above it, as on the Friends card.
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeAlways;
  self.navigationController.navigationBar.prefersLargeTitles = YES;
  search_controller_ = [[UISearchController alloc] initWithSearchResultsController:nil];
  search_controller_.searchResultsUpdater = self;
  search_controller_.delegate = self;
  search_controller_.obscuresBackgroundDuringPresentation = NO;
  search_controller_.hidesNavigationBarDuringPresentation = NO;
  search_controller_.searchBar.placeholder =
      NSLocalizedString(@"Search Settings", @"settings hub view controller: placeholder");
  self.navigationItem.searchController = search_controller_;
  self.navigationItem.hidesSearchBarWhenScrolling = NO;
  // Under the title, not floating over the rows at the bottom of a phone in landscape.
  self.navigationItem.preferredSearchBarPlacement = UINavigationItemSearchBarPlacementStacked;
  self.definesPresentationContext = YES;
  if (shows_close_button_) {
    XESetSheetClose(self, self, @selector(closeTapped:));
  }
}

// What each page is set to now, on its row: the frame rate, the sound, the controller, whether
// the system software is in, the version.
- (void)refreshStatusValues {
  search_index_ = BuildIOSConfigSearchIndex();
  search_index_valid_ = YES;
  NSString* frame_rate = nil;
  NSString* sound = nil;
  for (const IOSConfigSearchEntry& entry : search_index_) {
    if (entry.key == "framerate_limit") {
      frame_rate = ToNSString(entry.value_text);
    } else if (entry.key == "apu") {
      sound = ToNSString(entry.value_text);
    }
  }
  GCController* controller = GCController.current ?: GCController.controllers.firstObject;
  NSString* controls = controller.vendorName.length
                           ? controller.vendorName
                           : NSLocalizedString(@"Touch", @"settings hub: Controls row value when "
                                                         @"no controller is connected");
  const bool system_software = xe::ui::IsIOSDashboardInstalled();
  NSString* version = XeniaBundleValue(@"CFBundleShortVersionString");
  for (XeniaSettingsHubSection* section in all_sections_) {
    for (XeniaSettingsHubItem* item in section.items) {
      switch (item.kind) {
        case XeniaSettingsHubCategoryKindDisplay:
          // "30 FPS" says what it is; "Unlimited" alone beside Display doesn't.
          item.valueText =
              !frame_rate.length ? nil
              : [frame_rate rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet]
                          .location != NSNotFound
                  ? frame_rate
                  : NSLocalizedString(@"No FPS Limit",
                                      @"settings hub: Display row value when the frame rate "
                                      @"isn't limited");
          break;
        case XeniaSettingsHubCategoryKindAudio:
          item.valueText = sound.length ? sound : nil;
          break;
        case XeniaSettingsHubCategoryKindControls:
          item.valueText = item.focusKey.length ? item.valueText : controls;
          break;
        case XeniaSettingsHubCategoryKindSystemSoftware:
          item.valueText =
              system_software
                  ? NSLocalizedString(@"Installed", @"settings hub: System Software row value")
                  : NSLocalizedString(@"Not Set Up", @"settings hub: System Software row value");
          item.valueColor = system_software ? nil : [XeniaTheme statusWarning];
          break;
        case XeniaSettingsHubCategoryKindAbout:
          item.valueText = version.length ? version : nil;
          break;
        default:
          break;
      }
    }
  }
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  // A page may have changed values while this list was out of sight.
  search_index_valid_ = NO;
  // Coming back from a page: the profile heading shows whoever is signed in now, and each row
  // its current value.
  if (![self showsSearchResults]) {
    [self refreshStatusValues];
    [self.tableView reloadData];
  }
  self.tableView.selectionFollowsFocus = [self showsSidebarSelection];
  [self updateSidebarSelection];
}

// On iPad the list sits beside the page it opened, and marks that page as Settings does.
- (BOOL)showsSidebarSelection {
  return self.selectionDelegate && self.splitViewController &&
         !self.splitViewController.collapsed;
}

- (void)updateSidebarSelection {
  NSIndexPath* selected = self.tableView.indexPathForSelectedRow;
  NSIndexPath* target = nil;
  if ([self showsSidebarSelection] && ![self showsSearchResults]) {
    for (NSUInteger section = 0; section < sections_.count && !target; ++section) {
      NSArray* items = [[sections_ objectAtIndex:section] items];
      for (NSUInteger row = 0; row < items.count; ++row) {
        XeniaSettingsHubItem* item = [items objectAtIndex:row];
        if (item.kind == sidebar_kind_ && item.focusKey.length == 0) {
          target = [NSIndexPath indexPathForRow:row inSection:section];
          break;
        }
      }
    }
  }
  if (target) {
    if (![selected isEqual:target]) {
      [self.tableView selectRowAtIndexPath:target
                                  animated:NO
                            scrollPosition:UITableViewScrollPositionNone];
    }
  } else if (selected) {
    [self.tableView deselectRowAtIndexPath:selected animated:NO];
  }
}

#pragma mark - UISearchControllerDelegate

- (void)willPresentSearchController:(UISearchController* __unused)searchController {
  search_index_valid_ = NO;
}

- (void)closeTapped:(id)sender {
  (void)sender;
  void (^dismissal_handler)(void) = [dismissal_handler_ copy];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             if (dismissal_handler) {
                               dismissal_handler();
                             }
                             [dismissal_handler release];
                           }];
}

- (void)replaceVisibleSections:(NSArray*)sections {
  [sections_ release];
  sections_ = [sections retain];
  [self.tableView reloadData];
  [self updateSidebarSelection];
}

- (void)appendTouchLayoutSearchResults:(NSMutableArray*)results query:(NSString*)query {
  NSArray* items = @[
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindControls
               title:NSLocalizedString(@"Hardware Controllers",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Use controllers as soon as they connect.",
                         @"settings hub view controller: title")
          symbolName:@"gamecontroller.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindControls
               title:NSLocalizedString(@"Touch Controls", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Choose a layout before launch or adjust it "
                                       @"while a game is running.",
                                       @"settings hub view controller: title")
          symbolName:@"hand.tap.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Edit Game Touch Layout",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Pick a game and arrange its touch controls.",
                                       @"settings hub view controller: title")
          symbolName:@"gamecontroller.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Layout Library", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · All your touch layouts.",
                                       @"settings hub view controller: title")
          symbolName:@"rectangle.grid.2x2.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Edit Running Touch Layout",
                                       @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(@"Controls · Arrange the touch controls in the running game.",
                    @"settings hub view controller: title")
          symbolName:@"hand.draw.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Import Layout", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Add a layout from Files.",
                                       @"settings hub view controller: title")
          symbolName:@"square.and.arrow.down.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Export Running Layout",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Share the running game's layout.",
                         @"settings hub view controller: title")
          symbolName:@"square.and.arrow.up.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Reset Running Layout",
                                       @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(@"Controls · Go back to the standard layout.",
                    @"settings hub view controller: title")
          symbolName:@"arrow.counterclockwise"
           tintColor:nil],
  ];
  // These act on the running game, so they only show while one can be running.
  // (Items 4, 6 and 7: Edit, Export and Reset Running Layout.)
  [items enumerateObjectsUsingBlock:^(XeniaSettingsHubItem* item, NSUInteger index,
                                      __unused BOOL* stop) {
    item.needsRunningGame = index == 4 || index == 6 || index == 7;
  }];
  for (XeniaSettingsHubItem* item in items) {
    if (no_game_running_ && item.needsRunningGame) {
      continue;
    }
    if (XeniaSettingsHubItemMatches(
            item,
            NSLocalizedString(@"Controls", @"settings hub view controller: user-visible text"),
            query)) {
      [results addObject:item];
    }
  }
}

- (void)appendLibrarySearchResults:(NSMutableArray*)results query:(NSString*)query {
  NSArray* items = @[
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindLibrary
               title:NSLocalizedString(@"Import Game", @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(
                    @"Games · Copy a game or add-on from Files.",
                    @"settings hub view controller: title")
          symbolName:@"plus"
           tintColor:[XeniaTheme accent]],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindLibrary
               title:NSLocalizedString(@"External Folders", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Games · Play games from Files, USB drives or network "
                                       @"shares without copying them.",
                                       @"settings hub view controller: title")
          symbolName:@"folder.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindLibrary
               title:NSLocalizedString(@"Refresh Library", @"settings hub view controller: title")
            subtitle:NSLocalizedString(
                         @"Games · Look again for added or removed games.",
                         @"settings hub view controller: title")
          symbolName:@"arrow.clockwise"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindLibrary
               title:NSLocalizedString(@"Clear This Game's Shaders",
                                       @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(@"Storage · Removes the running game's saved shaders.",
                    @"settings hub view controller: title")
          symbolName:@"trash"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindLibrary
               title:NSLocalizedString(@"Clear All Shaders",
                                       @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(@"Storage · Removes saved shaders for every game.",
                    @"settings hub view controller: title")
          symbolName:@"trash.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindSystemSoftware
               title:NSLocalizedString(@"Xbox 360 System Software",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Games · Needed for the Xbox 360 Dashboard.",
                         @"settings hub view controller: title")
          symbolName:@"square.stack.3d.up.fill"
           tintColor:nil],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindLibrary
               title:NSLocalizedString(@"Convert Library to ZAR",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Storage · Makes disc images smaller.",
                                       @"settings hub view controller: title")
          symbolName:@"archivebox.fill"
           tintColor:nil],
  ];
  // Item 3, Clear This Game's Shaders, only shows while a game is running.
  ((XeniaSettingsHubItem*)items[3]).needsRunningGame = YES;
  for (XeniaSettingsHubItem* item in items) {
    if (no_game_running_ && item.needsRunningGame) {
      continue;
    }
    if (XeniaSettingsHubItemMatches(
            item,
            NSLocalizedString(@"Library & Storage",
                              @"settings hub view controller: user-visible text"),
            query)) {
      [results addObject:item];
    }
  }
}

- (void)updateSearchResultsForSearchController:(UISearchController*)searchController {
  NSString* query =
      [searchController.searchBar.text stringByTrimmingCharactersInSet:
                                        [NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if (!query.length) {
    [self replaceVisibleSections:all_sections_];
    return;
  }

  NSMutableArray* filtered_sections = [NSMutableArray array];
  for (XeniaSettingsHubSection* section in all_sections_) {
    NSMutableArray* filtered_items = [NSMutableArray array];
    for (XeniaSettingsHubItem* item in section.items) {
      if (XeniaSettingsHubItemMatches(item, section.title, query)) {
        [filtered_items addObject:item];
      }
    }
    if (filtered_items.count) {
      [filtered_sections addObject:[XeniaSettingsHubSection sectionWithTitle:section.title
                                                                       items:filtered_items]];
    }
  }

  NSMutableArray* setting_results = [NSMutableArray array];
  [self appendTouchLayoutSearchResults:setting_results query:query];
  [self appendLibrarySearchResults:setting_results query:query];

  // Every curated setting, by title, description, key, page, section and current value, then the
  // settings that exist only as raw config.
  if (!search_index_valid_) {
    search_index_ = BuildIOSConfigSearchIndex();
    search_index_valid_ = YES;
  }
  NSMutableArray* advanced_results = [NSMutableArray array];
  std::vector<std::pair<int, const IOSConfigSearchEntry*>> curated_matches;
  for (const IOSConfigSearchEntry& entry : search_index_) {
    if (!XeniaSettingsSearchEntryMatches(entry, query)) {
      continue;
    }
    if (entry.advanced) {
      // Two letters match half the emulator's option names; wait for a real word.
      if (query.length >= 3) {
        [advanced_results addObject:XeniaSettingsResultItem(entry)];
      }
    } else {
      curated_matches.emplace_back(XeniaSettingsSearchRank(entry, query), &entry);
    }
  }
  std::stable_sort(curated_matches.begin(), curated_matches.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& match : curated_matches) {
    [setting_results addObject:XeniaSettingsResultItem(*match.second)];
  }
  if (setting_results.count) {
    [filtered_sections
        addObject:[XeniaSettingsHubSection
                      sectionWithTitle:NSLocalizedString(@"Settings",
                                                         @"settings hub view controller: title")
                                 items:setting_results]];
  }
  if (advanced_results.count) {
    [filtered_sections
        addObject:[XeniaSettingsHubSection
                      sectionWithTitle:NSLocalizedString(@"Emulator Options",
                                                         @"settings hub view controller: title")
                                 items:advanced_results]];
  }

  [self replaceVisibleSections:filtered_sections];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return static_cast<NSInteger>(sections_.count);
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  XeniaSettingsHubSection* hub_section = [self sectionAtIndex:section];
  return static_cast<NSInteger>(hub_section.items.count);
}

// Like Settings, the groups go untitled and the spacing separates them; search results keep the
// name of the page each group comes from.
- (BOOL)showsSearchResults {
  return search_controller_.active && search_controller_.searchBar.text.length > 0;
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)section {
  if (![self showsSearchResults]) {
    return nil;
  }
  NSString* title = [self sectionAtIndex:section].title;
  return title.length ? title : nil;
}

- (CGFloat)tableView:(UITableView* __unused)tableView heightForHeaderInSection:(NSInteger)section {
  if ([self showsSearchResults]) {
    return UITableViewAutomaticDimension;
  }
  return section == 0 ? 4.0 : 16.0;  // 16 between groups, as on the game page.
}

- (CGFloat)tableView:(UITableView* __unused)tableView
    heightForFooterInSection:(NSInteger __unused)section {
  return [self showsSearchResults] ? UITableViewAutomaticDimension : 0.01;
}

- (void)tableView:(UITableView* __unused)tableView
      willDisplayCell:(UITableViewCell*)cell
    forRowAtIndexPath:(NSIndexPath*)indexPath {
  // Every row, the profile included, is in a glass group on the wash.
  (void)indexPath;
  cell.backgroundColor = [XeniaTheme glassGroupFill];
}

// The account card at the top, as in Settings: avatar, gamertag, and what signing in is for.
- (void)configureAccountCell:(UITableViewCell*)cell {
  xe::ui::IOSProfileSummary signed_in_profile;
  bool any_profile = false;
  if (app_context_ && app_context_->ProfileServicesReady()) {
    for (const auto& profile : app_context_->ListProfiles()) {
      any_profile = true;
      if (profile.signed_in) {
        signed_in_profile = profile;
        break;
      }
    }
  }
  NSString* gamertag = signed_in_profile.signed_in ? ToNSString(signed_in_profile.gamertag) : nil;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  if (gamertag.length) {
    content.text = gamertag;
    NSString* where = NSLocalizedString(@"Profiles and account",
                                        @"settings hub view controller: account row");
    content.secondaryText =
        xe_ios_live_account_service().summary.online
            ? [NSString stringWithFormat:@"%@ \u00B7 %@",
                                         NSLocalizedString(@"Online",
                                                           @"settings hub view controller: account "
                                                           @"row, the profile is online"),
                                         where]
            : where;
    content.image = xe_profile_avatar_image(gamertag, YES, 36.0);
  } else {
    content.text = any_profile
                       ? NSLocalizedString(@"Sign In", @"settings hub view controller: account card")
                       : NSLocalizedString(@"Add a Profile",
                                           @"settings hub view controller: account card");
    content.secondaryText = NSLocalizedString(@"Games save progress and achievements to it.",
                                              @"settings hub view controller: account card");
    content.image = [UIImage systemImageNamed:@"person.crop.circle.fill"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithPointSize:34.0]];
    content.imageProperties.tintColor = [UIColor colorWithWhite:1.0 alpha:0.5];
  }
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.font =
      xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightSemibold);
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.secondaryTextProperties.font =
      xe_scaled_system_font(UIFontTextStyleSubheadline, 14.0, UIFontWeightRegular);
  content.textToSecondaryTextVerticalPadding = 2.0;
  // An avatar row: 36-pt circle, 64 high, text where every other row's text starts.
  content.imageProperties.reservedLayoutSize = CGSizeMake(36.0, 36.0);
  content.imageProperties.maximumSize = CGSizeMake(36.0, 36.0);
  content.imageToTextPadding = 14.0;
  content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(10.0, 16.0, 10.0, 16.0);
  cell.contentConfiguration = content;
  cell.accessoryView = nil;
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  cell.isAccessibilityElement = YES;
  XEApplyAccessibility(cell, content.text, content.secondaryText,
                       NSLocalizedString(@"Opens your profiles.",
                                         @"settings hub view controller: account card hint"),
                       UIAccessibilityTraitButton);
}

- (XeniaSettingsHubSection*)sectionAtIndex:(NSInteger)section {
  if (section < 0 || section >= static_cast<NSInteger>(sections_.count)) {
    return nil;
  }
  return [sections_ objectAtIndex:section];
}

- (XeniaSettingsHubItem*)itemAtIndexPath:(NSIndexPath*)indexPath {
  XeniaSettingsHubSection* section = [self sectionAtIndex:indexPath.section];
  if (!section || indexPath.row < 0 ||
      indexPath.row >= static_cast<NSInteger>(section.items.count)) {
    return nil;
  }
  return [section.items objectAtIndex:indexPath.row];
}

// Beside the page on iPad: no chevrons, and the open page filled in the accent with white text.
- (void)applySidebarStyleToCell:(UITableViewCell*)cell hasValue:(BOOL)has_value {
  if (![self showsSidebarSelection]) {
    cell.selectedBackgroundView = nil;
    cell.configurationUpdateHandler = nil;
    cell.focusEffect = [UIFocusHaloEffect effect];
    return;
  }
  if (!has_value) {
    cell.accessoryType = UITableViewCellAccessoryNone;
  }
  // A rounded fill of its own rather than the group's corners; with a keyboard it is also the
  // focus, as the selection follows it.
  UIView* background =
      [[[UIView alloc] initWithFrame:CGRectMake(0.0, 0.0, 100.0, 44.0)] autorelease];
  UIView* fill = [[[UIView alloc] initWithFrame:CGRectMake(0.0, 2.0, 100.0, 40.0)] autorelease];
  fill.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  fill.backgroundColor = [XeniaTheme accent];
  fill.layer.cornerRadius = 12.0;
  fill.layer.cornerCurve = kCACornerCurveContinuous;
  [background addSubview:fill];
  cell.selectedBackgroundView = background;
  cell.focusEffect = nil;
  cell.configurationUpdateHandler = ^(UITableViewCell* updated_cell,
                                      UICellConfigurationState* state) {
    if (![updated_cell.contentConfiguration isKindOfClass:[UIListContentConfiguration class]]) {
      return;
    }
    const BOOL filled = state.isSelected || state.isHighlighted;
    UIListContentConfiguration* content =
        [[(UIListContentConfiguration*)updated_cell.contentConfiguration copy] autorelease];
    content.textProperties.color = filled ? [UIColor whiteColor] : [XeniaTheme textPrimary];
    content.secondaryTextProperties.color =
        filled ? [UIColor colorWithWhite:1.0 alpha:0.8] : [XeniaTheme textSecondary];
    updated_cell.contentConfiguration = content;
  };
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaSettingsHubCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
  }

  XeniaSettingsHubItem* item = [self itemAtIndexPath:indexPath];
  if (!item) {
    cell.contentConfiguration = nil;
    cell.accessoryView = nil;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.isAccessibilityElement = NO;
    cell.selectedBackgroundView = nil;
    cell.configurationUpdateHandler = nil;
    return cell;
  }

  if (item.kind == XeniaSettingsHubCategoryKindProfile && item.focusKey.length == 0) {
    [self configureAccountCell:cell];
    [self applySidebarStyleToCell:cell hasValue:NO];
    return cell;
  }

  // List content configuration so titles and subtitles wrap at large text sizes (a search result
  // carries a path and a per-game note as well as a description).
  // A category is one line, as in Settings; a search result also says where it is.
  const BOOL search_result = item.focusKey.length > 0;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = item.title;
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.numberOfLines = 0;
  content.textProperties.font = xe_scaled_system_font(UIFontTextStyleBody, 17.0,
                                                      UIFontWeightRegular);
  // At the larger text sizes a value on the trailing edge leaves the title a word or two per
  // line; it goes under the path instead.
  const BOOL value_below =
      search_result && item.valueText.length &&
      UIContentSizeCategoryCompareToCategory(tableView.traitCollection.preferredContentSizeCategory,
                                             UIContentSizeCategoryExtraLarge) != NSOrderedAscending;
  content.secondaryText =
      !search_result ? nil
      : value_below  ? [NSString stringWithFormat:@"%@\n%@", item.subtitle ?: @"", item.valueText]
                     : item.subtitle;
  content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(
      search_result ? 10.0 : 11.5, 16.0, search_result ? 10.0 : 11.5, 16.0);
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.secondaryTextProperties.numberOfLines = 0;
  content.secondaryTextProperties.font = xe_scaled_system_font(UIFontTextStyleFootnote, 13.0,
                                                               UIFontWeightRegular);
  content.prefersSideBySideTextAndSecondaryText = NO;
  content.image = XERowSymbolImage(item.symbolName, item.tintColor);
  content.imageProperties.cornerRadius = 8.0;
  content.imageProperties.reservedLayoutSize = CGSizeMake(29.0, 29.0);
  content.imageProperties.maximumSize = CGSizeMake(29.0, 29.0);
  content.imageToTextPadding = 14.0;
  cell.contentConfiguration = content;

  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  if (item.valueText.length && !value_below) {
    // The current value, on the trailing edge like in Settings.
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.accessoryView = XeniaConfigMakeValueAccessoryView(
        item.valueText, CGRectGetWidth(tableView.bounds) * 0.4, YES);
    if (item.valueColor) {
      for (UIView* view in ((UIStackView*)cell.accessoryView).arrangedSubviews) {
        if ([view isKindOfClass:[UILabel class]]) {
          ((UILabel*)view).textColor = item.valueColor;
        }
      }
    }
  } else {
    cell.accessoryView = nil;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  }
  [self applySidebarStyleToCell:cell hasValue:item.valueText.length > 0];
  const BOOL link = item.kind == XeniaSettingsHubCategoryKindLink;
  if (link) {
    // Leaves the app, so an arrow out rather than a chevron in.
    UIImageView* arrow = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:@"arrow.up.right"
                              withConfiguration:[UIImageSymbolConfiguration
                                                    configurationWithTextStyle:
                                                        UIFontTextStyleFootnote
                                                                         scale:UIImageSymbolScaleMedium]]]
        autorelease];
    arrow.tintColor = [UIColor tertiaryLabelColor];
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.accessoryView = arrow;
  }

  // A result reads as: setting, current value, and where it opens.
  const BOOL is_result = item.focusKey.length > 0;
  NSString* value = is_result ? item.valueText : (item.subtitle.length ? item.subtitle : nil);
  NSString* hint = item.hintText.length ? item.hintText
                 : link ? NSLocalizedString(@"Opens in your browser.",
                                            @"settings hub view controller: hint of a link row")
                        : NSLocalizedString(@"Opens this settings section.",
                                            @"settings hub view controller: title");
  XEApplyAccessibility(cell, item.title, value, hint,
                       link ? UIAccessibilityTraitLink : UIAccessibilityTraitButton);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  const BOOL sidebar = [self showsSidebarSelection];
  if (!sidebar) {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
  }
  XeniaSettingsHubItem* item = [self itemAtIndexPath:indexPath];
  if (item.kind == XeniaSettingsHubCategoryKindLink) {
    // Opens outside the app; the sidebar keeps the page it was showing.
    XeniaOpenSettingsURL(item.url);
    if (sidebar) {
      [self updateSidebarSelection];
    }
    return;
  }
  UIViewController* detail = [self makeDetailControllerForItem:item];
  if (!detail) {
    [self updateSidebarSelection];
    return;
  }
  if (sidebar) {
    sidebar_kind_ = item.kind;
  }
  void (^show)(void) = ^{
    if ([self.selectionDelegate respondsToSelector:
            @selector(settingsHubViewController:didSelectController:)]) {
      [self.selectionDelegate settingsHubViewController:self didSelectController:detail];
      [self updateSidebarSelection];
      return;
    }
    [self.navigationController pushViewController:detail animated:YES];
  };
  if (search_controller_.active) {
    // A page shown while the search is still closing is dropped; show it once it has closed.
    search_controller_.active = NO;
    id<UIViewControllerTransitionCoordinator> coordinator =
        search_controller_.transitionCoordinator;
    if (coordinator) {
      [coordinator animateAlongsideTransition:nil
                                   completion:^(id<UIViewControllerTransitionCoordinatorContext>
                                                    __unused context) {
                                     show();
                                   }];
    } else {
      dispatch_async(dispatch_get_main_queue(), show);
    }
    return;
  }
  show();
}

- (UIViewController*)makeInitialDetailController {
  XeniaSettingsHubItem* item = nil;
  if (initial_section_ == XeniaSettingsInitialSectionProfile) {
    item = [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindProfile
               title:NSLocalizedString(@"Profile", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Gamertags and sign-in.",
                                       @"settings hub view controller: title")
          symbolName:@"person.crop.circle.fill"
           tintColor:[XeniaTheme accent]];
  } else {
    item = [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindDisplay
               title:NSLocalizedString(@"Display", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Presenter output, aspect, and frame pacing.",
                                       @"settings hub view controller: title")
          symbolName:@"display"
           tintColor:nil];
  }
  sidebar_kind_ = item.kind;
  return [self makeDetailControllerForItem:item];
}

- (UIViewController*)makeDetailControllerForItem:(XeniaSettingsHubItem*)item {
  UIViewController* detail = [self makeRawDetailControllerForItem:item];
  // Live setting changes pause the running game through the app context.
  if ([detail isKindOfClass:[XeniaConfigViewController class]]) {
    XeniaConfigViewController* config = (XeniaConfigViewController*)detail;
    config.appContext = app_context_;
    config.gameRunning = !no_game_running_;
    // A search result opens its page scrolled to the setting.
    config.focusKey = item.focusKey;
  }
  return detail;
}

- (UIViewController*)makeRawDetailControllerForItem:(XeniaSettingsHubItem*)item {
  if (!item) {
    return nil;
  }

  switch (item.kind) {
    case XeniaSettingsHubCategoryKindProfile: {
      XeniaProfileViewController* profile =
          [[[XeniaProfileViewController alloc] initWithAppContext:app_context_
                                                         onStatus:on_status_] autorelease];
      profile.showsDismissButton = NO;
      return profile;
    }
    case XeniaSettingsHubCategoryKindAppearance:
      return [[[XeniaSettingsAppearanceViewController alloc] initWithStatusHandler:on_status_]
          autorelease];
    case XeniaSettingsHubCategoryKindControls:
      return [[[XeniaSettingsControlsViewController alloc]
          initWithActionHandler:action_handler_
                  noGameRunning:no_game_running_] autorelease];
    case XeniaSettingsHubCategoryKindTouchLayouts:
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:NSLocalizedString(@"Touch Layouts", @"settings hub view controller: title")
                  items:XeniaTouchLayoutActionItems(no_game_running_)
          actionHandler:action_handler_] autorelease];
    case XeniaSettingsHubCategoryKindTouchBehavior: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kControls
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindDisplay: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kDisplay
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindGraphics: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kGraphics
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindAudio: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kAudio
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindLibrary: {
      NSArray* library = @[
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionImportGame
                     title:NSLocalizedString(@"Import Game", @"settings hub view controller: title")
                  subtitle:NSLocalizedString(@"Copy a game or add-on from Files.",
                                             @"settings hub view controller: title")
                symbolName:@"plus"
                 tintColor:[XeniaTheme accent]],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionManageExternalFolders
                     title:NSLocalizedString(@"External Folders",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(@"Play games from Files, USB drives or network "
                                             @"shares without copying them.",
                                             @"settings hub view controller: title")
                symbolName:@"folder.fill"
                 tintColor:nil],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionRefreshLibrary
                     title:NSLocalizedString(@"Refresh Library",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(@"Look again for added or removed games.",
                                             @"settings hub view controller: title")
                symbolName:@"arrow.clockwise"
                 tintColor:nil],
      ];
      NSMutableArray* storage = [NSMutableArray array];
      [storage addObject:[XeniaSettingsActionListItem
                             itemWithAction:XeniaSettingsHubActionConvertLibraryToZar
                                      title:NSLocalizedString(
                                                @"Convert Library to ZAR",
                                                @"settings hub view controller: title")
                                   subtitle:NSLocalizedString(
                                                @"Makes disc images smaller. Games play the same.",
                                                @"settings hub view controller: title")
                                 symbolName:@"archivebox.fill"
                                  tintColor:nil]];
      // Only a running game has shaders of its own to clear.
      if (!no_game_running_) {
        [storage addObject:[XeniaSettingsActionListItem
                               itemWithAction:XeniaSettingsHubActionClearCurrentGameShaderCache
                                        title:NSLocalizedString(
                                                  @"Clear This Game's Shaders",
                                                  @"settings hub view controller: title")
                                     subtitle:NSLocalizedString(
                                                  @"Removes the running game's saved shaders. "
                                                  @"They rebuild as you play.",
                                                  @"settings hub view controller: title")
                                   symbolName:@"trash"
                                    tintColor:nil]];
      }
      [storage addObject:[XeniaSettingsActionListItem
                             itemWithAction:XeniaSettingsHubActionClearAllShaderCaches
                                      title:NSLocalizedString(
                                                @"Clear All Shaders",
                                                @"settings hub view controller: title")
                                   subtitle:NSLocalizedString(
                                                @"Removes saved shaders for every game. Games "
                                                @"and saves stay.",
                                                @"settings hub view controller: title")
                                 symbolName:@"trash.fill"
                                  tintColor:[XeniaTheme statusError]]];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:NSLocalizedString(@"Library & Storage",
                                          @"settings hub view controller: title")
               sections:@[ library, storage ]
          sectionTitles:@[
            NSLocalizedString(@"Library", @"settings hub view controller: section"),
            NSLocalizedString(@"Storage", @"settings hub view controller: section"),
          ]
          actionHandler:action_handler_] autorelease];
    }
    case XeniaSettingsHubCategoryKindSystemSoftware:
      return [[[XeniaSystemSoftwareViewController alloc] initWithGameRunning:!no_game_running_]
          autorelease];
    case XeniaSettingsHubCategoryKindPerformance: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kPerformance
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindCompatibility: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kCompatibility
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindDiagnostics: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kDiagnostics
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindAdvancedDebug: {
      XeniaIOSDebugSettingsViewController* config =
          [[[XeniaIOSDebugSettingsViewController alloc]
              initWithCatalogKind:IOSConfigCatalogKind::kDebugSettings
                     liveOverride:NO] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindAutomation: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kSystem
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindOnline: {
      XeniaConfigViewController* config = [[[XeniaConfigViewController alloc]
          initWithCatalogKind:IOSConfigCatalogKind::kOnline
                        style:UITableViewStyleInsetGrouped] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindPrivacy:
      return [[[XeniaSettingsPrivacyViewController alloc] init] autorelease];
    case XeniaSettingsHubCategoryKindAbout:
      return [[[XeniaSettingsAboutViewController alloc] init] autorelease];
    case XeniaSettingsHubCategoryKindAllCvars: {
      XeniaIOSDebugSettingsViewController* config =
          [[[XeniaIOSDebugSettingsViewController alloc]
              initWithCatalogKind:IOSConfigCatalogKind::kAllCvars
                     liveOverride:NO] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    case XeniaSettingsHubCategoryKindAllSettings: {
      // Searchable and editable, like the developer pages (XeniaConfigViewController owns the
      // search); the debug subclass only sets the page up as a pushed, non-live page.
      XeniaIOSDebugSettingsViewController* config =
          [[[XeniaIOSDebugSettingsViewController alloc]
              initWithCatalogKind:IOSConfigCatalogKind::kAllSettings
                     liveOverride:NO] autorelease];
      config.showsRootDismissButton = NO;
      return config;
    }
    default:
      return nil;
  }
}

@end

@interface XeniaSettingsWorkspaceViewController () <
    XeniaSettingsHubViewControllerDelegate>
@end

@implementation XeniaSettingsWorkspaceViewController {
  XeniaSettingsHubViewController* hub_controller_;
  void (^dismissal_handler_)(void);
  XeniaSettingsHubActionHandler action_handler_;
}

@synthesize dismissalHandler = dismissal_handler_;
@synthesize actionHandler = action_handler_;

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(XeniaSettingsStatusHandler)on_status {
  return [self initWithAppContext:app_context
                         onStatus:on_status
                   initialSection:XeniaSettingsInitialSectionMain];
}

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(XeniaSettingsStatusHandler)on_status
                    initialSection:(XeniaSettingsInitialSection)initial_section {
  self = [super initWithStyle:UISplitViewControllerStyleDoubleColumn];
  if (self) {
    self.title = NSLocalizedString(@"Settings", @"settings hub view controller: title");
    self.preferredDisplayMode = UISplitViewControllerDisplayModeOneBesideSecondary;
    self.preferredSplitBehavior = UISplitViewControllerSplitBehaviorTile;
    // Wide enough for every category on one line at the default text size.
    self.minimumPrimaryColumnWidth = 340.0;
    self.preferredPrimaryColumnWidth = 340.0;

    XeniaSettingsHubViewController* hub =
        [[[XeniaSettingsHubViewController alloc] initWithAppContext:app_context
                                                           onStatus:on_status
                                                     initialSection:initial_section] autorelease];
    hub.selectionDelegate = self;
    hub.showsCloseButton = YES;
    hub.dismissalHandler = dismissal_handler_;
    hub_controller_ = [hub retain];

    UINavigationController* primary =
        [[[UINavigationController alloc] initWithRootViewController:hub] autorelease];
    primary.navigationBar.prefersLargeTitles = YES;

    UIViewController* detail = [hub makeInitialDetailController];
    UINavigationController* secondary =
        [[[UINavigationController alloc] initWithRootViewController:detail] autorelease];
    secondary.navigationBar.prefersLargeTitles = NO;

    [self setViewController:primary forColumn:UISplitViewControllerColumnPrimary];
    [self setViewController:secondary forColumn:UISplitViewControllerColumnSecondary];
    // Settings is the app's own chrome: dark glass whatever the system appearance, on the one
    // charcoal the other sheets use (see docs/ios_design_language.md).
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    primary.view.backgroundColor = [UIColor clearColor];  // The wash shows through.
    secondary.view.backgroundColor = [UIColor clearColor];  // The wash shows through.
  }
  return self;
}

- (void)dealloc {
  [hub_controller_ release];
  [dismissal_handler_ release];
  [action_handler_ release];
  [super dealloc];
}

- (void)setDismissalHandler:(void (^)(void))dismissalHandler {
  if (dismissal_handler_ == dismissalHandler) {
    return;
  }
  [dismissal_handler_ release];
  dismissal_handler_ = [dismissalHandler copy];
  hub_controller_.dismissalHandler = dismissal_handler_;
}

- (void)setActionHandler:(XeniaSettingsHubActionHandler)actionHandler {
  if (action_handler_ == actionHandler) {
    return;
  }
  [action_handler_ release];
  action_handler_ = [actionHandler copy];
  hub_controller_.actionHandler = action_handler_;
}

- (BOOL)noGameRunning {
  return hub_controller_.noGameRunning;
}

- (void)setNoGameRunning:(BOOL)noGameRunning {
  if (hub_controller_.noGameRunning == noGameRunning) {
    return;
  }
  hub_controller_.noGameRunning = noGameRunning;
  // The first detail page is made in init, before this is known, so make it again.
  if (!self.isViewLoaded) {
    UINavigationController* secondary = [[[UINavigationController alloc]
        initWithRootViewController:[hub_controller_ makeInitialDetailController]] autorelease];
    secondary.navigationBar.prefersLargeTitles = NO;
    [self setViewController:secondary forColumn:UISplitViewControllerColumnSecondary];
  }
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskAll;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
  return xe_current_interface_orientation(self.view);
}

- (BOOL)shouldAutorotate {
  return YES;
}

- (void)settingsHubViewController:(XeniaSettingsHubViewController* __unused)hub
               didSelectController:(UIViewController*)controller {
  UINavigationController* secondary =
      [[[UINavigationController alloc] initWithRootViewController:controller] autorelease];
  secondary.navigationBar.prefersLargeTitles = NO;
  [self setViewController:secondary forColumn:UISplitViewControllerColumnSecondary];
}

@end
