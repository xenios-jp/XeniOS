/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_settings_hub_view_controller.h"

#import "xenia/ui/ios/settings/ios_acknowledgements_view_controller.h"
#import "xenia/ui/ios/settings/ios_config_catalog.h"
#import "xenia/ui/ios/settings/ios_config_storage.h"
#import "xenia/ui/ios/settings/ios_config_view_controller.h"
#import "xenia/ui/ios/settings/ios_debug_settings_view_controller.h"
#import "xenia/ui/ios/settings/ios_profile_view_controller.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_status_toast.h"
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
  return NSLocalizedString(@"Use the adaptive primary icon.",
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
  XeniaSettingsHubCategoryKindCacheCleanup,
  XeniaSettingsHubCategoryKindLibrary,
  XeniaSettingsHubCategoryKindGameContent,
  XeniaSettingsHubCategoryKindPerGameSettings,
  XeniaSettingsHubCategoryKindAutomation,
  XeniaSettingsHubCategoryKindAbout,
};

@interface XeniaSettingsHubItem : NSObject
@property(nonatomic, assign) XeniaSettingsHubCategoryKind kind;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* subtitle;
@property(nonatomic, copy) NSString* symbolName;
@property(nonatomic, retain) UIColor* tintColor;
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
         XeniaSettingsStringMatches(section_title, query);
}

static NSString* XeniaSettingsCatalogResultSubtitle(IOSConfigCatalogKind kind,
                                                    const IOSConfigItem& item) {
  NSString* catalog_title =
      [NSString stringWithUTF8String:IOSConfigCatalogTitle(kind).c_str()];
  NSString* item_subtitle =
      item.subtitle.empty() ? nil : [NSString stringWithUTF8String:item.subtitle.c_str()];
  return item_subtitle.length
             ? [NSString stringWithFormat:@"%@ · %@", catalog_title, item_subtitle]
             : catalog_title;
}

static BOOL XeniaSettingsConfigItemMatches(const IOSConfigItem& item,
                                           const IOSConfigSection& section,
                                           NSString* query) {
  NSString* item_title = [NSString stringWithUTF8String:item.title.c_str()];
  NSString* item_subtitle = [NSString stringWithUTF8String:item.subtitle.c_str()];
  NSString* item_key = [NSString stringWithUTF8String:item.key.c_str()];
  NSString* section_title = [NSString stringWithUTF8String:section.title.c_str()];
  return XeniaSettingsStringMatches(item_title, query) ||
         XeniaSettingsStringMatches(item_subtitle, query) ||
         XeniaSettingsStringMatches(item_key, query) ||
         XeniaSettingsStringMatches(section_title, query);
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
    default:
      return @"tablecells";
  }
}

static UIColor* XeniaSettingsTintForCatalog(IOSConfigCatalogKind kind) {
  switch (kind) {
    case IOSConfigCatalogKind::kDisplay:
      return [XeniaTheme sectionIconIndigo];
    case IOSConfigCatalogKind::kGraphics:
    case IOSConfigCatalogKind::kDebugSettings:
      return [XeniaTheme sectionIconPurple];
    case IOSConfigCatalogKind::kAudio:
    case IOSConfigCatalogKind::kPerformance:
      return [XeniaTheme sectionIconOrange];
    case IOSConfigCatalogKind::kControls:
      return [XeniaTheme sectionIconBlue];
    case IOSConfigCatalogKind::kCompatibility:
    case IOSConfigCatalogKind::kDiagnostics:
      return [XeniaTheme sectionIconRed];
    case IOSConfigCatalogKind::kSystem:
      return [XeniaTheme accent];
    default:
      return [XeniaTheme sectionIconGray];
  }
}

static void XeniaAppendCatalogSearchResults(NSMutableArray* results,
                                            IOSConfigCatalogKind kind,
                                            NSString* query) {
  std::vector<IOSConfigSection> sections = BuildIOSConfigSectionsForKind(kind);
  for (const IOSConfigSection& section : sections) {
    for (const IOSConfigItem& item : section.items) {
      if (!XeniaSettingsConfigItemMatches(item, section, query)) {
        continue;
      }
      NSString* title = [NSString stringWithUTF8String:item.title.c_str()];
      [results addObject:[XeniaSettingsHubItem
                             itemWithKind:XeniaSettingsKindForCatalog(kind)
                                    title:title
                                 subtitle:XeniaSettingsCatalogResultSubtitle(kind, item)
                               symbolName:XeniaSettingsSymbolForCatalog(kind)
                                tintColor:XeniaSettingsTintForCatalog(kind)]];
    }
  }
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
  [on_status_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 64.0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
    numberOfRowsInSection:(NSInteger)__unused section {
  return 3;
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)__unused section {
  return NSLocalizedString(@"App Icon", @"settings hub view controller: user-visible text");
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
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

  UIImageSymbolConfiguration* icon_config =
      [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* image = [UIImage systemImageNamed:XeniaAppIconSymbolForRow(row)
                           withConfiguration:icon_config];
  cell.imageView.image = [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor =
      row == 2 ? [XeniaTheme sectionIconIndigo] : [XeniaTheme sectionIconOrange];
  XEApplyAccessibility(cell, cell.textLabel.text, cell.detailTextLabel.text,
                       supported ? NSLocalizedString(@"Selects this app icon.",
                                                     @"settings hub view controller: detail text")
                                 : nil,
                       supported ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
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
                      [self.tableView reloadData];
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
  item.tintColor = [XeniaTheme sectionIconGray];
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
@end

@implementation XeniaSettingsActionListViewController {
  NSArray* items_;
  XeniaSettingsHubActionHandler action_handler_;
}

- (instancetype)initWithTitle:(NSString*)title
                        items:(NSArray*)items
                actionHandler:(XeniaSettingsHubActionHandler)action_handler {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = title;
    items_ = [items retain];
    action_handler_ = [action_handler copy];
  }
  return self;
}

- (void)dealloc {
  [items_ release];
  [action_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 72.0;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)__unused section {
  return static_cast<NSInteger>(items_.count);
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

  XeniaSettingsActionListItem* item = [items_ objectAtIndex:indexPath.row];
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
  UIImageSymbolConfiguration* icon_config =
      [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* image = [UIImage systemImageNamed:item.symbolName
                           withConfiguration:icon_config];
  cell.imageView.image = [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor = item.tintColor;
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
  XeniaSettingsActionListItem* item = [items_ objectAtIndex:indexPath.row];
  if (!item.selectable || !action_handler_) {
    return;
  }
  action_handler_(item.action);
}

@end

@interface XeniaSettingsControlsViewController : XESheetTableViewController
- (instancetype)initWithActionHandler:(XeniaSettingsHubActionHandler)action_handler;
@end

@implementation XeniaSettingsControlsViewController {
  NSArray* setup_items_;
  NSArray* layout_items_;
  XeniaSettingsHubActionHandler action_handler_;
}

- (instancetype)initWithActionHandler:(XeniaSettingsHubActionHandler)action_handler {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = NSLocalizedString(@"Controls", @"settings hub view controller: title");
    UIColor* blue = [XeniaTheme sectionIconBlue];
    setup_items_ = [@[
      [XeniaSettingsActionListItem
          infoItemWithTitle:NSLocalizedString(@"Hardware Controllers",
                                              @"settings hub view controller: title")
                   subtitle:NSLocalizedString(@"Connect a controller before or during gameplay; "
                                              @"XeniOS binds it automatically.",
                                              @"settings hub view controller: title")
                 symbolName:@"gamecontroller.fill"],
      [XeniaSettingsActionListItem
          infoItemWithTitle:NSLocalizedString(@"Touch Controls",
                                              @"settings hub view controller: title")
                   subtitle:NSLocalizedString(@"Pick a game layout before launch, then use Edit "
                                              @"Running Touch Layout for final placement.",
                                              @"settings hub view controller: title")
                 symbolName:@"hand.tap.fill"],
    ] retain];
    layout_items_ = [@[
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionChooseGameTouchLayout
                   title:NSLocalizedString(@"Edit Game Touch Layout",
                                           @"settings hub view controller: title")
                subtitle:
                    NSLocalizedString(
                        @"Choose an installed title and edit its touch controls before launch.",
                        @"settings hub view controller: title")
              symbolName:@"gamecontroller.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionOpenTouchLayoutLibrary
                   title:NSLocalizedString(@"Layout Library",
                                           @"settings hub view controller: title")
                subtitle:NSLocalizedString(
                             @"Browse, rename, duplicate, import, export, and reset touch layouts.",
                             @"settings hub view controller: title")
              symbolName:@"rectangle.grid.2x2.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionEditTouchControls
                   title:NSLocalizedString(@"Edit Running Touch Layout",
                                           @"settings hub view controller: title")
                subtitle:NSLocalizedString(
                             @"Open the in-game touch editor for the currently running title.",
                             @"settings hub view controller: title")
              symbolName:@"hand.draw.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionImportTouchLayout
                   title:NSLocalizedString(@"Import Layout", @"settings hub view controller: title")
                subtitle:NSLocalizedString(@"Install a .toml layout from Files.",
                                           @"settings hub view controller: title")
              symbolName:@"square.and.arrow.down.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionExportTouchLayout
                   title:NSLocalizedString(@"Export Running Layout",
                                           @"settings hub view controller: title")
                subtitle:NSLocalizedString(
                             @"Share the layout currently loaded in the running game.",
                             @"settings hub view controller: title")
              symbolName:@"square.and.arrow.up.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionResetTouchLayout
                   title:NSLocalizedString(@"Reset Running Layout",
                                           @"settings hub view controller: title")
                subtitle:NSLocalizedString(
                             @"Restore the running game's layout to the built-in XeniOS preset.",
                             @"settings hub view controller: title")
              symbolName:@"arrow.counterclockwise"
               tintColor:[XeniaTheme sectionIconOrange]],
    ] retain];
    action_handler_ = [action_handler copy];
  }
  return self;
}

- (void)dealloc {
  [setup_items_ release];
  [layout_items_ release];
  [action_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 72.0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 3;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  if (section == 0) {
    return static_cast<NSInteger>(setup_items_.count);
  }
  return section == 1 ? static_cast<NSInteger>(layout_items_.count) : 1;
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)section {
  if (section == 0) {
    return NSLocalizedString(@"Setup", @"settings hub view controller: user-visible text");
  }
  return section == 1 ? NSLocalizedString(@"Touch Layouts",
                                          @"settings hub view controller: user-visible text")
                      : NSLocalizedString(@"Touch Behavior",
                                          @"settings hub view controller: user-visible text");
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
    XeniaSettingsActionListItem* item = [setup_items_ objectAtIndex:indexPath.row];
    title = item.title;
    subtitle = item.subtitle;
    symbol_name = item.symbolName;
    tint_color = item.tintColor;
  } else if (indexPath.section == 1) {
    XeniaSettingsActionListItem* item = [layout_items_ objectAtIndex:indexPath.row];
    title = item.title;
    subtitle = item.subtitle;
    symbol_name = item.symbolName;
    tint_color = item.tintColor;
  } else {
    title = NSLocalizedString(@"Touch Behavior", @"settings hub view controller: title");
    subtitle = NSLocalizedString(@"Haptics, overlay visibility, and touch-look tuning.",
                                 @"settings hub view controller: title");
    symbol_name = @"hand.tap.fill";
    tint_color = [XeniaTheme sectionIconBlue];
  }

  cell.textLabel.text = title;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text = subtitle;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0,
                      UIFontWeightSemibold);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleSubheadline, 15.0,
                      UIFontWeightRegular);

  UIImageSymbolConfiguration* icon_config =
      [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* image = [UIImage systemImageNamed:symbol_name
                           withConfiguration:icon_config];
  cell.imageView.image = [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor = tint_color;
  const BOOL selectable = indexPath.section != 0;
  cell.accessoryType = selectable ? UITableViewCellAccessoryDisclosureIndicator
                                  : UITableViewCellAccessoryNone;
  cell.selectionStyle = selectable ? UITableViewCellSelectionStyleDefault
                                   : UITableViewCellSelectionStyleNone;
  XEApplyAccessibility(cell, title, subtitle,
                       selectable ? NSLocalizedString(@"Opens this controls option.",
                                                      @"settings hub view controller: title")
                                  : nil,
                       selectable ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 0) {
    return;
  }
  if (indexPath.section == 1) {
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

namespace {

typedef NS_ENUM(NSInteger, XeniaAboutSection) {
  XeniaAboutSectionBuild = 0,
  XeniaAboutSectionUpdates,
  XeniaAboutSectionPrivacy,
  XeniaAboutSectionLinks,
  XeniaAboutSectionDeveloper,  // Only while developer mode is on.
};

constexpr NSInteger kAboutVersionRow = 1;
constexpr NSInteger kAboutAcknowledgementsRow = 4;  // After the four links.
constexpr NSInteger kDeveloperModeTapCount = 7;
constexpr NSTimeInterval kDeveloperModeTapWindowSeconds = 2.0;

}  // namespace

@implementation XeniaSettingsAboutViewController {
  XeniaUpdateInfo* update_info_;
  BOOL update_checking_;
  NSInteger version_tap_count_;
  NSTimeInterval last_version_tap_time_;
  XeniaIOSStatusToastPresenter* toast_presenter_;
}

- (instancetype)init {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = NSLocalizedString(@"About", @"settings hub view controller: title");
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [toast_presenter_ release];
  [update_info_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  toast_presenter_ = [[XeniaIOSStatusToastPresenter alloc] init];
  // Another path (the switch below, or Reset) can change developer mode.
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(developerModeChanged:)
                                               name:kXeniaDeveloperModeChangedNotification
                                             object:nil];
  [self refreshUpdateStatusForced:NO];
}

- (void)developerModeChanged:(NSNotification*)__unused notification {
  [self.tableView reloadData];
}

- (NSInteger)sectionCount {
  return IOSDeveloperModeEnabled() ? 5 : 4;
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
  return [self sectionCount];
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  switch (section) {
    case XeniaAboutSectionBuild:
      return 3;
    case XeniaAboutSectionUpdates:
    case XeniaAboutSectionPrivacy:
    case XeniaAboutSectionDeveloper:
      return 1;
    default:
      return 5;  // Four links and Acknowledgements.
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView
titleForHeaderInSection:(NSInteger)section {
  switch (section) {
    case XeniaAboutSectionBuild:
      return NSLocalizedString(@"Build", @"settings hub view controller: user-visible text");
    case XeniaAboutSectionUpdates:
      return NSLocalizedString(@"Updates", @"settings hub view controller: user-visible text");
    case XeniaAboutSectionPrivacy:
      return NSLocalizedString(@"Privacy", @"settings hub view controller: user-visible text");
    case XeniaAboutSectionDeveloper:
      return NSLocalizedString(@"Developer", @"settings hub view controller: user-visible text");
    default:
      return NSLocalizedString(@"Links", @"settings hub view controller: user-visible text");
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView
titleForFooterInSection:(NSInteger)section {
  if (section == XeniaAboutSectionPrivacy) {
    return NSLocalizedString(
        @"Update checks, compatibility data, cover art, and compatibility discussions and "
        @"reports. Turn this off and XeniOS does none of these. Games are not affected.",
        @"settings hub view controller: user-visible text");
  }
  if (section == XeniaAboutSectionDeveloper) {
    return NSLocalizedString(
        @"Shows Advanced Debug and All Config under Settings > Advanced. These change "
        @"emulator internals and can break games.",
        @"settings hub view controller: user-visible text");
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

  if (indexPath.section == XeniaAboutSectionPrivacy ||
      indexPath.section == XeniaAboutSectionDeveloper) {
    const BOOL privacy = indexPath.section == XeniaAboutSectionPrivacy;
    cell.textLabel.text =
        privacy ? NSLocalizedString(@"Online Features", @"settings hub view controller: label text")
                : NSLocalizedString(@"Developer Mode", @"settings hub view controller: label text");
    cell.detailTextLabel.text = @"";
    UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
    toggle.on = privacy ? xe_ios_online_features_enabled() : IOSDeveloperModeEnabled();
    toggle.accessibilityLabel = cell.textLabel.text;
    [toggle addTarget:self
                  action:privacy ? @selector(onlineFeaturesSwitchChanged:)
                                 : @selector(developerModeSwitchChanged:)
        forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
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
        cell.detailTextLabel.text = update_info_.currentVersionText ?: @"";
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

  NSArray* titles = @[
    NSLocalizedString(@"Website", @"settings hub view controller: title"), @"GitHub",
    NSLocalizedString(@"Discord", @"settings hub view controller: title"),
    NSLocalizedString(@"Support", @"settings hub view controller: title"),
    NSLocalizedString(@"Acknowledgements", @"settings hub view controller: title")
  ];
  cell.textLabel.text = [titles objectAtIndex:indexPath.row];
  cell.detailTextLabel.text = @"";
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  return cell;
}

- (void)onlineFeaturesSwitchChanged:(UISwitch*)sender {
  xe_ios_set_online_features_enabled(sender.isOn);
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:XeniaAboutSectionUpdates]
                withRowAnimation:UITableViewRowAnimationNone];
  if (sender.isOn) {
    [self refreshUpdateStatusForced:NO];
  }
}

- (void)developerModeSwitchChanged:(UISwitch*)sender {
  IOSSetDeveloperModeEnabled(sender.isOn);
}

// Seven quick taps on the Version row switch developer mode on.
- (void)registerVersionTap {
  const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
  if (now - last_version_tap_time_ > kDeveloperModeTapWindowSeconds) {
    version_tap_count_ = 0;
  }
  last_version_tap_time_ = now;
  ++version_tap_count_;
  if (version_tap_count_ < kDeveloperModeTapCount) {
    return;
  }
  version_tap_count_ = 0;
  if (IOSDeveloperModeEnabled()) {
    [toast_presenter_ presentMessage:NSLocalizedString(@"Developer mode is already on.",
                                                       @"settings hub view controller: message")
                               style:XeniaIOSStatusToastStyleInfo
                              inView:self.view];
    return;
  }
  IOSSetDeveloperModeEnabled(true);
  [toast_presenter_
      presentMessage:NSLocalizedString(
                         @"Developer mode is on. You can turn it off at the bottom of "
                         @"this page.",
                         @"settings hub view controller: message")
               style:XeniaIOSStatusToastStyleSuccess
              inView:self.view];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == XeniaAboutSectionBuild) {
    if (indexPath.row == kAboutVersionRow) {
      [self registerVersionTap];
    }
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
  if (indexPath.row == kAboutAcknowledgementsRow) {
    XeniaAcknowledgementsViewController* acknowledgements =
        [[XeniaAcknowledgementsViewController alloc] init];
    [self.navigationController pushViewController:acknowledgements animated:YES];
    [acknowledgements release];
    return;
  }
  NSArray* urls = @[
    @"https://xenios.jp",
    @"https://github.com/xenios-jp/XeniOS",
    @"https://discord.gg/QwcTtNKTGf",
    @"https://ko-fi.com/xenios",
  ];
  XeniaOpenSettingsURL([urls objectAtIndex:indexPath.row]);
}

@end

@interface XeniaSettingsHubViewController () <UISearchResultsUpdating>
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
}

@synthesize dismissalHandler = dismissal_handler_;
@synthesize actionHandler = action_handler_;
@synthesize showsCloseButton = shows_close_button_;

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
    all_sections_ = [[self buildSections] retain];
    sections_ = [all_sections_ retain];
    self.title = NSLocalizedString(@"Settings", @"settings hub view controller: title");
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(developerModeChanged:)
                                                 name:kXeniaDeveloperModeChangedNotification
                                               object:nil];
  }
  return self;
}

- (void)developerModeChanged:(NSNotification*)__unused notification {
  [all_sections_ release];
  all_sections_ = [[self buildSections] retain];
  if (!search_controller_.active) {
    [self replaceVisibleSections:all_sections_];
  }
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [on_status_ release];
  [action_handler_ release];
  [all_sections_ release];
  [sections_ release];
  [search_controller_ release];
  [dismissal_handler_ release];
  [super dealloc];
}

- (NSArray*)buildSections {
  UIColor* mint = [XeniaTheme accent];
  UIColor* blue = [XeniaTheme sectionIconBlue];
  UIColor* indigo = [XeniaTheme sectionIconIndigo];
  UIColor* orange = [XeniaTheme sectionIconOrange];
  UIColor* purple = [XeniaTheme sectionIconPurple];
  UIColor* red = [XeniaTheme sectionIconRed];
  UIColor* gray = [XeniaTheme sectionIconGray];

  XeniaSettingsHubSection* account = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Profile", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindProfile
                              title:NSLocalizedString(@"Profile",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Manage local profiles and sign-in slots.",
                                                      @"settings hub view controller: title")
                         symbolName:@"person.crop.circle.fill"
                          tintColor:mint],
                 ]];

  XeniaSettingsHubSection* gameplay = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Gameplay", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindDisplay
                              title:NSLocalizedString(@"Display",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Presenter output, aspect, and frame pacing.",
                                        @"settings hub view controller: title")
                         symbolName:@"display"
                          tintColor:indigo],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAudio
                              title:NSLocalizedString(@"Audio",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Output and decoder preferences.",
                                                      @"settings hub view controller: title")
                         symbolName:@"speaker.wave.2.fill"
                          tintColor:orange],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindControls
                              title:NSLocalizedString(@"Controls",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Controller setup, touch layouts, haptics, and look tuning.",
                                   @"settings hub view controller: title")
                         symbolName:@"gamecontroller.fill"
                          tintColor:blue],
                 ]];

  XeniaSettingsHubSection* graphics_performance = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Graphics & Performance",
                                         @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindGraphics
                              title:NSLocalizedString(@"Graphics",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Backend, quality, shader pipeline, and render target path.",
                                   @"settings hub view controller: title")
                         symbolName:@"gearshape.2.fill"
                          tintColor:purple],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindPerformance
                              title:NSLocalizedString(@"Performance",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Shader compilation behavior and experimental iOS thread QoS.",
                                   @"settings hub view controller: title")
                         symbolName:@"bolt.fill"
                          tintColor:orange],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindCacheCleanup
                              title:NSLocalizedString(@"Cache Cleanup",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Clear current-title or all-title persistent shader caches.",
                                   @"settings hub view controller: title")
                         symbolName:@"trash"
                          tintColor:orange],
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
                                   @"Import games, refresh the library, and manage local storage.",
                                   @"settings hub view controller: title")
                         symbolName:@"externaldrive.fill"
                          tintColor:gray],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindGameContent
                              title:NSLocalizedString(@"Game Content & Patches",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Title content, discs, packages, and patches from game tiles.",
                                   @"settings hub view controller: title")
                         symbolName:@"puzzlepiece.extension.fill"
                          tintColor:gray],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindPerGameSettings
                              title:NSLocalizedString(@"Per-Game Settings",
                                                      @"settings hub view controller: title")
                           subtitle:
                               NSLocalizedString(
                                   @"Title-specific overrides, search, reset, and batched saves.",
                                   @"settings hub view controller: title")
                         symbolName:@"gamecontroller.fill"
                          tintColor:indigo],
                 ]];

  XeniaSettingsHubSection* compatibility = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Compatibility", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindCompatibility
                              title:NSLocalizedString(@"Compatibility Workarounds",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"GPU, memory, boot, and JIT title workarounds.",
                                        @"settings hub view controller: title")
                         symbolName:@"wrench.and.screwdriver.fill"
                          tintColor:red],
                 ]];

  XeniaSettingsHubSection* app = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"App", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAppearance
                              title:NSLocalizedString(@"Appearance",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"App icon selection and interface presentation.",
                                        @"settings hub view controller: title")
                         symbolName:@"paintpalette.fill"
                          tintColor:mint],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAutomation
                              title:NSLocalizedString(@"Automation",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Local iOS automation options.",
                                                      @"settings hub view controller: title")
                         symbolName:@"wand.and.stars"
                          tintColor:mint],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindDiagnostics
                              title:NSLocalizedString(@"Diagnostics",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Log verbosity and live log viewer.",
                                                      @"settings hub view controller: title")
                         symbolName:@"waveform.path.ecg"
                          tintColor:red],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAbout
                              title:NSLocalizedString(@"About",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Build information and project links.",
                                                      @"settings hub view controller: title")
                         symbolName:@"info.circle.fill"
                          tintColor:gray],
                 ]];

  // Advanced Debug and All Config edit emulator internals; they appear only in developer
  // mode (About > Version, seven taps).
  if (!IOSDeveloperModeEnabled()) {
    return @[ account, gameplay, graphics_performance, games, compatibility, app ];
  }

  XeniaSettingsHubSection* advanced = [XeniaSettingsHubSection
      sectionWithTitle:NSLocalizedString(@"Advanced", @"settings hub view controller: title")
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAdvancedDebug
                              title:NSLocalizedString(@"Advanced Debug",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(
                                        @"Curated F8-style debug and compatibility overrides.",
                                        @"settings hub view controller: title")
                         symbolName:@"slider.horizontal.3"
                          tintColor:purple],
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAllCvars
                              title:NSLocalizedString(@"All Config",
                                                      @"settings hub view controller: title")
                           subtitle:NSLocalizedString(@"Search and edit every registered cvar.",
                                                      @"settings hub view controller: title")
                         symbolName:@"tablecells"
                          tintColor:purple],
                 ]];

  return @[
    account, gameplay, graphics_performance, games, compatibility, app, advanced
  ];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.separatorInset = UIEdgeInsetsMake(0, 16.0, 0, 16.0);
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 74.0;
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeAlways;
  search_controller_ = [[UISearchController alloc] initWithSearchResultsController:nil];
  search_controller_.searchResultsUpdater = self;
  search_controller_.obscuresBackgroundDuringPresentation = NO;
  search_controller_.hidesNavigationBarDuringPresentation = NO;
  search_controller_.searchBar.placeholder =
      NSLocalizedString(@"Search Settings", @"settings hub view controller: placeholder");
  self.navigationItem.searchController = search_controller_;
  self.navigationItem.hidesSearchBarWhenScrolling = NO;
  self.definesPresentationContext = YES;
  if (shows_close_button_) {
    self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
        initWithTitle:NSLocalizedString(@"Close", @"settings hub view controller: title")
                style:UIBarButtonItemStylePlain
               target:self
               action:@selector(closeTapped:)] autorelease];
  }
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
}

- (void)appendTouchLayoutSearchResults:(NSMutableArray*)results query:(NSString*)query {
  UIColor* blue = [XeniaTheme sectionIconBlue];
  NSArray* items = @[
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindControls
               title:NSLocalizedString(@"Hardware Controllers",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(
                         @"Controls · Auto-bind controllers connected before or during gameplay.",
                         @"settings hub view controller: title")
          symbolName:@"gamecontroller.fill"
           tintColor:blue],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindControls
               title:NSLocalizedString(@"Touch Controls", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Choose a layout before launch or adjust it "
                                       @"while a game is running.",
                                       @"settings hub view controller: title")
          symbolName:@"hand.tap.fill"
           tintColor:blue],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Edit Game Touch Layout",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Choose an installed title and edit its touch "
                                       @"controls before launch.",
                                       @"settings hub view controller: title")
          symbolName:@"gamecontroller.fill"
           tintColor:blue],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Layout Library", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Browse, rename, duplicate, import, export, and "
                                       @"reset touch layouts.",
                                       @"settings hub view controller: title")
          symbolName:@"rectangle.grid.2x2.fill"
           tintColor:blue],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Edit Running Touch Layout",
                                       @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(
                    @"Controls · Open the in-game touch editor for the currently running title.",
                    @"settings hub view controller: title")
          symbolName:@"hand.draw.fill"
           tintColor:blue],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Import Layout", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Controls · Install a .toml layout from Files.",
                                       @"settings hub view controller: title")
          symbolName:@"square.and.arrow.down.fill"
           tintColor:blue],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Export Running Layout",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(
                         @"Controls · Share the layout currently loaded in the running game.",
                         @"settings hub view controller: title")
          symbolName:@"square.and.arrow.up.fill"
           tintColor:blue],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
               title:NSLocalizedString(@"Reset Running Layout",
                                       @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(
                    @"Controls · Restore the running game's layout to the built-in XeniOS preset.",
                    @"settings hub view controller: title")
          symbolName:@"arrow.counterclockwise"
           tintColor:[XeniaTheme sectionIconOrange]],
  ];
  for (XeniaSettingsHubItem* item in items) {
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
                    @"Games · Open Files and copy a game or content package into XeniOS storage.",
                    @"settings hub view controller: title")
          symbolName:@"plus.app.fill"
           tintColor:[XeniaTheme accent]],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindLibrary
               title:NSLocalizedString(@"Refresh Library", @"settings hub view controller: title")
            subtitle:NSLocalizedString(
                         @"Games · Rescan XeniOS storage and update the launcher grid.",
                         @"settings hub view controller: title")
          symbolName:@"arrow.clockwise"
           tintColor:[XeniaTheme sectionIconGray]],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindCacheCleanup
               title:NSLocalizedString(@"Clear Current Game Shader Cache",
                                       @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(
                    @"Graphics & Performance · Delete persistent shaders for the running title.",
                    @"settings hub view controller: title")
          symbolName:@"trash"
           tintColor:[XeniaTheme sectionIconOrange]],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindCacheCleanup
               title:NSLocalizedString(@"Clear All Shader Caches",
                                       @"settings hub view controller: title")
            subtitle:
                NSLocalizedString(
                    @"Graphics & Performance · Delete persistent shader storage for every title.",
                    @"settings hub view controller: title")
          symbolName:@"trash.fill"
           tintColor:[XeniaTheme sectionIconRed]],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindGameContent
               title:NSLocalizedString(@"Game Content and Patches",
                                       @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Games · Manage title-specific content from a game tile.",
                                       @"settings hub view controller: title")
          symbolName:@"puzzlepiece.extension.fill"
           tintColor:[XeniaTheme sectionIconGray]],
    [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindPerGameSettings
               title:NSLocalizedString(@"Per-Game Settings", @"settings hub view controller: title")
            subtitle:NSLocalizedString(
                         @"Games · Title-specific overrides, search, reset, and batched saves.",
                         @"settings hub view controller: title")
          symbolName:@"gamecontroller.fill"
           tintColor:[XeniaTheme sectionIconIndigo]],
  ];
  for (XeniaSettingsHubItem* item in items) {
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
  const IOSConfigCatalogKind kCatalogs[] = {
      IOSConfigCatalogKind::kDisplay,       IOSConfigCatalogKind::kGraphics,
      IOSConfigCatalogKind::kAudio,         IOSConfigCatalogKind::kControls,
      IOSConfigCatalogKind::kPerformance,   IOSConfigCatalogKind::kCompatibility,
      IOSConfigCatalogKind::kDiagnostics,   IOSConfigCatalogKind::kDebugSettings,
      IOSConfigCatalogKind::kSystem,
  };
  for (IOSConfigCatalogKind kind : kCatalogs) {
    if (kind == IOSConfigCatalogKind::kDebugSettings && !IOSDeveloperModeEnabled()) {
      continue;
    }
    XeniaAppendCatalogSearchResults(setting_results, kind, query);
  }
  if (setting_results.count) {
    [filtered_sections
        addObject:[XeniaSettingsHubSection
                      sectionWithTitle:NSLocalizedString(@"Settings",
                                                         @"settings hub view controller: title")
                                 items:setting_results]];
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

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)section {
  return [self sectionAtIndex:section].title;
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

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaSettingsHubCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    cell.textLabel.numberOfLines = 1;
    cell.detailTextLabel.numberOfLines = 2;
  }

  XeniaSettingsHubItem* item = [self itemAtIndexPath:indexPath];
  if (!item) {
    cell.textLabel.text = @"";
    cell.detailTextLabel.text = @"";
    cell.imageView.image = nil;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.isAccessibilityElement = NO;
    return cell;
  }

  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  cell.textLabel.text = item.title;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text = item.subtitle;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(cell.textLabel, UIFontTextStyleBody, 17.0,
                      UIFontWeightSemibold);
  xe_apply_label_font(cell.detailTextLabel, UIFontTextStyleSubheadline, 15.0,
                      UIFontWeightRegular);

  UIImageSymbolConfiguration* image_config =
      [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* image = [UIImage systemImageNamed:item.symbolName
                           withConfiguration:image_config];
  cell.imageView.image = [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor = item.tintColor;

  NSString* value = item.subtitle.length ? item.subtitle : nil;
  XEApplyAccessibility(
      cell, item.title, value,
      NSLocalizedString(@"Opens this settings section.", @"settings hub view controller: title"),
      UIAccessibilityTraitButton);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  XeniaSettingsHubItem* item = [self itemAtIndexPath:indexPath];
  UIViewController* detail = [self makeDetailControllerForItem:item];
  if (!detail) {
    return;
  }
  if (search_controller_.active) {
    search_controller_.active = NO;
  }

  if ([self.selectionDelegate respondsToSelector:
          @selector(settingsHubViewController:didSelectController:)]) {
    [self.selectionDelegate settingsHubViewController:self didSelectController:detail];
    return;
  }
  [self.navigationController pushViewController:detail animated:YES];
}

- (UIViewController*)makeInitialDetailController {
  XeniaSettingsHubItem* item = nil;
  if (initial_section_ == XeniaSettingsInitialSectionProfile) {
    item = [XeniaSettingsHubItem
        itemWithKind:XeniaSettingsHubCategoryKindProfile
               title:NSLocalizedString(@"Profile", @"settings hub view controller: title")
            subtitle:NSLocalizedString(@"Manage local profiles and sign-in slots.",
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
           tintColor:[XeniaTheme sectionIconIndigo]];
  }
  return [self makeDetailControllerForItem:item];
}

- (UIViewController*)makeDetailControllerForItem:(XeniaSettingsHubItem*)item {
  UIViewController* detail = [self makeRawDetailControllerForItem:item];
  // Live setting changes pause the running game through the app context.
  if ([detail isKindOfClass:[XeniaConfigViewController class]]) {
    ((XeniaConfigViewController*)detail).appContext = app_context_;
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
          initWithActionHandler:action_handler_] autorelease];
    case XeniaSettingsHubCategoryKindTouchLayouts: {
      UIColor* blue = [XeniaTheme sectionIconBlue];
      NSArray* items = @[
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionChooseGameTouchLayout
                     title:NSLocalizedString(@"Edit Game Touch Layout",
                                             @"settings hub view controller: title")
                  subtitle:
                      NSLocalizedString(
                          @"Choose an installed title and edit its touch controls before launch.",
                          @"settings hub view controller: title")
                symbolName:@"gamecontroller.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionOpenTouchLayoutLibrary
                     title:NSLocalizedString(@"Layout Library",
                                             @"settings hub view controller: title")
                  subtitle:
                      NSLocalizedString(
                          @"Browse, rename, duplicate, import, export, and reset touch layouts.",
                          @"settings hub view controller: title")
                symbolName:@"rectangle.grid.2x2.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionEditTouchControls
                     title:NSLocalizedString(@"Edit Running Touch Layout",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(
                               @"Open the in-game touch editor for the currently running title.",
                               @"settings hub view controller: title")
                symbolName:@"hand.draw.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionImportTouchLayout
                     title:NSLocalizedString(@"Import Layout",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(@"Install a .toml layout from Files.",
                                             @"settings hub view controller: title")
                symbolName:@"square.and.arrow.down.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionExportTouchLayout
                     title:NSLocalizedString(@"Export Running Layout",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(
                               @"Share the layout currently loaded in the running game.",
                               @"settings hub view controller: title")
                symbolName:@"square.and.arrow.up.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionResetTouchLayout
                     title:NSLocalizedString(@"Reset Running Layout",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(
                               @"Restore the running game's layout to the built-in XeniOS preset.",
                               @"settings hub view controller: title")
                symbolName:@"arrow.counterclockwise"
                 tintColor:[XeniaTheme sectionIconOrange]],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:NSLocalizedString(@"Touch Layouts", @"settings hub view controller: title")
                  items:items
          actionHandler:action_handler_] autorelease];
    }
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
      UIColor* gray = [XeniaTheme sectionIconGray];
      NSArray* items = @[
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionImportGame
                     title:NSLocalizedString(@"Import Game", @"settings hub view controller: title")
                  subtitle:
                      NSLocalizedString(
                          @"Open Files and copy a game or content package into XeniOS storage.",
                          @"settings hub view controller: title")
                symbolName:@"plus.app.fill"
                 tintColor:[XeniaTheme accent]],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionRefreshLibrary
                     title:NSLocalizedString(@"Refresh Library",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(@"Rescan XeniOS storage and update the launcher grid.",
                                             @"settings hub view controller: title")
                symbolName:@"arrow.clockwise"
                 tintColor:gray],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:NSLocalizedString(@"Library & Storage",
                                          @"settings hub view controller: title")
                  items:items
          actionHandler:action_handler_] autorelease];
    }
    case XeniaSettingsHubCategoryKindCacheCleanup: {
      NSArray* items = @[
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionClearCurrentGameShaderCache
                     title:NSLocalizedString(@"Clear Current Game Shader Cache",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(@"Delete persistent shaders for the running title. "
                                             @"Relaunch the game before testing.",
                                             @"settings hub view controller: title")
                symbolName:@"trash"
                 tintColor:[XeniaTheme sectionIconOrange]],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionClearAllShaderCaches
                     title:NSLocalizedString(@"Clear All Shader Caches",
                                             @"settings hub view controller: title")
                  subtitle:NSLocalizedString(@"Delete persistent shader storage for every title. "
                                             @"This does not remove games or saves.",
                                             @"settings hub view controller: title")
                symbolName:@"trash.fill"
                 tintColor:[XeniaTheme sectionIconRed]],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:NSLocalizedString(@"Cache Cleanup", @"settings hub view controller: title")
                  items:items
          actionHandler:action_handler_] autorelease];
    }
    case XeniaSettingsHubCategoryKindGameContent: {
      NSArray* items = @[
        [XeniaSettingsActionListItem
            infoItemWithTitle:NSLocalizedString(@"Game Content and Patches",
                                                @"settings hub view controller: title")
                     subtitle:NSLocalizedString(@"Manage title-specific installed content, discs, "
                                                @"and patches from a game tile's context menu.",
                                                @"settings hub view controller: title")
                   symbolName:@"puzzlepiece.extension.fill"],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:NSLocalizedString(@"Game Content & Patches",
                                          @"settings hub view controller: title")
                  items:items
          actionHandler:action_handler_] autorelease];
    }
    case XeniaSettingsHubCategoryKindPerGameSettings: {
      NSMutableArray* items = [NSMutableArray
          arrayWithObject:[XeniaSettingsActionListItem
                              infoItemWithTitle:NSLocalizedString(
                                                    @"Title Overrides",
                                                    @"settings hub view controller: title")
                                       subtitle:NSLocalizedString(
                                                    @"Open Game Settings from a game tile to edit "
                                                    @"settings that apply only to that title.",
                                                    @"settings hub view controller: title")
                                     symbolName:@"slider.horizontal.3"]];
      if (IOSDeveloperModeEnabled()) {
        [items addObject:[XeniaSettingsActionListItem
                             infoItemWithTitle:NSLocalizedString(
                                                   @"Search Overrides",
                                                   @"settings hub view controller: title")
                                      subtitle:NSLocalizedString(
                                                   @"Search All Config appears at the top of each "
                                                   @"title's settings sheet.",
                                                   @"settings hub view controller: title")
                                    symbolName:@"magnifyingglass"]];
      }
      [items addObjectsFromArray:@[
        [XeniaSettingsActionListItem
            infoItemWithTitle:NSLocalizedString(@"Reset Game Settings",
                                                @"settings hub view controller: title")
                     subtitle:NSLocalizedString(@"Reset appears in the same title settings sheet "
                                                @"and deletes saved overrides for that title.",
                                                @"settings hub view controller: title")
                   symbolName:@"arrow.counterclockwise"],
      ]];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:NSLocalizedString(@"Per-Game Settings",
                                          @"settings hub view controller: title")
                  items:items
          actionHandler:action_handler_] autorelease];
    }
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
