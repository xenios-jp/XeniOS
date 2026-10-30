/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_settings_hub_view_controller.h"

#import "xenia/ui/ios/settings/ios_config_catalog.h"
#import "xenia/ui/ios/settings/ios_config_view_controller.h"
#import "xenia/ui/ios/settings/ios_debug_settings_view_controller.h"
#import "xenia/ui/ios/settings/ios_profile_view_controller.h"
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
    return @"Light";
  }
  if (row == 2) {
    return @"Dark";
  }
  return @"Automatic";
}

NSString* XeniaAppIconSubtitleForRow(NSInteger row) {
  if (row == 1) {
    return @"Use the light icon.";
  }
  if (row == 2) {
    return @"Use the dark icon.";
  }
  return @"Use the adaptive primary icon.";
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
    self.title = @"Appearance";
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
  return @"App Icon";
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
                       supported ? @"Selects this app icon." : nil,
                       supported ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  UIApplication* application = [UIApplication sharedApplication];
  if (!application.supportsAlternateIcons) {
    if (on_status_) {
      on_status_(@"This build does not include alternate app icons.");
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
                          on_status_([NSString
                              stringWithFormat:@"App icon: %@", error.localizedDescription]);
                        }
                        return;
                      }
                      [self.tableView reloadData];
                      if (on_status_) {
                        on_status_([NSString stringWithFormat:@"App icon set to %@.", title]);
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
  XEApplyAccessibility(cell, item.title, item.subtitle,
                       item.selectable ? @"Performs this settings action." : nil,
                       item.selectable ? UIAccessibilityTraitButton
                                       : UIAccessibilityTraitStaticText);
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
    self.title = @"Controls";
    UIColor* blue = [XeniaTheme sectionIconBlue];
    setup_items_ = [@[
      [XeniaSettingsActionListItem
          infoItemWithTitle:@"Hardware Controllers"
                   subtitle:@"Connect a controller before or during gameplay; XeniOS binds it automatically."
                 symbolName:@"gamecontroller.fill"],
      [XeniaSettingsActionListItem
          infoItemWithTitle:@"Touch Controls"
                   subtitle:@"Pick a game layout before launch, then use Edit Running Touch Layout for final placement."
                 symbolName:@"hand.tap.fill"],
    ] retain];
    layout_items_ = [@[
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionChooseGameTouchLayout
                   title:@"Edit Game Touch Layout"
                subtitle:@"Choose an installed title and edit its touch controls before launch."
              symbolName:@"gamecontroller.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionOpenTouchLayoutLibrary
                   title:@"Layout Library"
                subtitle:@"Browse, rename, duplicate, import, export, and reset touch layouts."
              symbolName:@"rectangle.grid.2x2.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionEditTouchControls
                   title:@"Edit Running Touch Layout"
                subtitle:@"Open the in-game touch editor for the currently running title."
              symbolName:@"hand.draw.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionImportTouchLayout
                   title:@"Import Layout"
                subtitle:@"Install a .toml layout from Files."
              symbolName:@"square.and.arrow.down.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionExportTouchLayout
                   title:@"Export Running Layout"
                subtitle:@"Share the layout currently loaded in the running game."
              symbolName:@"square.and.arrow.up.fill"
               tintColor:blue],
      [XeniaSettingsActionListItem
          itemWithAction:XeniaSettingsHubActionResetTouchLayout
                   title:@"Reset Running Layout"
                subtitle:@"Restore the running game's layout to the built-in XeniOS preset."
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
    return @"Setup";
  }
  return section == 1 ? @"Touch Layouts" : @"Touch Behavior";
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
    title = @"Touch Behavior";
    subtitle = @"Haptics, overlay visibility, and touch-look tuning.";
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
                       selectable ? @"Opens this controls option." : nil,
                       selectable ? UIAccessibilityTraitButton
                                  : UIAccessibilityTraitStaticText);
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

@implementation XeniaSettingsAboutViewController {
  XeniaUpdateInfo* update_info_;
  BOOL update_checking_;
}

- (instancetype)init {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = @"About";
  }
  return self;
}

- (void)dealloc {
  [update_info_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  [self refreshUpdateStatusForced:NO];
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
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:1]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 3;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  switch (section) {
    case 0:
      return 3;  // Build
    case 1:
      return 1;  // Updates
    default:
      return 4;  // Links
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView
titleForHeaderInSection:(NSInteger)section {
  switch (section) {
    case 0:
      return @"Build";
    case 1:
      return @"Updates";
    default:
      return @"Links";
  }
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
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

  if (indexPath.section == 0) {
    NSArray* titles = @[ @"Name", @"Version", @"Bundle" ];
    NSString* version = XeniaBundleValue(@"CFBundleShortVersionString") ?: @"";
    NSString* build = XeniaBundleValue((NSString*)kCFBundleVersionKey) ?: @"";
    NSString* version_text = version.length && build.length
                                 ? [NSString stringWithFormat:@"%@ (%@)", version, build]
                                 : (version.length ? version : build);
    NSArray* values = @[
      XeniaBundleValue((NSString*)kCFBundleNameKey) ?: @"XeniOS",
      version_text.length ? version_text : @"Development Build",
      [[NSBundle mainBundle] bundleIdentifier] ?: @""
    ];
    cell.textLabel.text = [titles objectAtIndex:indexPath.row];
    cell.detailTextLabel.text = [values objectAtIndex:indexPath.row];
    return cell;
  }

  if (indexPath.section == 1) {
    if (update_checking_) {
      cell.textLabel.text = @"Checking for Updates…";
      cell.detailTextLabel.text = @"";
      UIActivityIndicatorView* spinner = [[[UIActivityIndicatorView alloc]
          initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
      [spinner startAnimating];
      cell.accessoryView = spinner;
      return cell;
    }
    switch (update_info_.status) {
      case XeniaUpdateStatusUpdateAvailable:
        cell.textLabel.text = @"Update Available";
        cell.detailTextLabel.text = update_info_.latestVersionText ?: @"";
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        break;
      case XeniaUpdateStatusUpToDate:
        cell.textLabel.text = @"Up to Date";
        cell.detailTextLabel.text = @"Check Now";
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        break;
      case XeniaUpdateStatusDevelopmentBuild:
        cell.textLabel.text = @"Development Build";
        cell.detailTextLabel.text = update_info_.currentVersionText ?: @"";
        break;
      case XeniaUpdateStatusUnknown:
      default:
        cell.textLabel.text = @"Check for Updates";
        cell.detailTextLabel.text = @"";
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        break;
    }
    return cell;
  }

  NSArray* titles = @[ @"Website", @"GitHub", @"Discord", @"Support" ];
  cell.textLabel.text = [titles objectAtIndex:indexPath.row];
  cell.detailTextLabel.text = @"";
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 1) {
    if (update_checking_ || update_info_.status == XeniaUpdateStatusDevelopmentBuild) {
      return;
    }
    if (update_info_.status == XeniaUpdateStatusUpdateAvailable &&
        update_info_.downloadURLString.length) {
      XeniaOpenSettingsURL(update_info_.downloadURLString);
    } else {
      // Up to date / unknown: tap re-checks immediately.
      [self refreshUpdateStatusForced:YES];
    }
    return;
  }
  if (indexPath.section != 2) {
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
    self.title = @"Settings";
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

- (NSArray*)buildSections {
  UIColor* mint = [XeniaTheme accent];
  UIColor* blue = [XeniaTheme sectionIconBlue];
  UIColor* indigo = [XeniaTheme sectionIconIndigo];
  UIColor* orange = [XeniaTheme sectionIconOrange];
  UIColor* purple = [XeniaTheme sectionIconPurple];
  UIColor* red = [XeniaTheme sectionIconRed];
  UIColor* gray = [XeniaTheme sectionIconGray];

  XeniaSettingsHubSection* account =
      [XeniaSettingsHubSection sectionWithTitle:@"Profile"
                                          items:@[
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindProfile
                                                       title:@"Profile"
                                                    subtitle:@"Manage local profiles and sign-in slots."
                                                  symbolName:@"person.crop.circle.fill"
                                                   tintColor:mint],
                                          ]];

  XeniaSettingsHubSection* gameplay =
      [XeniaSettingsHubSection sectionWithTitle:@"Gameplay"
                                          items:@[
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindDisplay
                                                       title:@"Display"
                                                    subtitle:@"Presenter output, aspect, and frame pacing."
                                                  symbolName:@"display"
                                                   tintColor:indigo],
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindAudio
                                                       title:@"Audio"
                                                    subtitle:@"Output and decoder preferences."
                                                  symbolName:@"speaker.wave.2.fill"
                                                   tintColor:orange],
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindControls
                                                       title:@"Controls"
                                                    subtitle:@"Controller setup, touch layouts, haptics, and look tuning."
                                                  symbolName:@"gamecontroller.fill"
                                                   tintColor:blue],
                                          ]];

  XeniaSettingsHubSection* graphics_performance =
      [XeniaSettingsHubSection sectionWithTitle:@"Graphics & Performance"
                                          items:@[
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindGraphics
                                                       title:@"Graphics"
                                                    subtitle:@"Backend, quality, shader pipeline, and render target path."
                                                  symbolName:@"gearshape.2.fill"
                                                   tintColor:purple],
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindPerformance
                                                       title:@"Performance"
                                                    subtitle:@"Shader compilation behavior and experimental iOS thread QoS."
                                                  symbolName:@"bolt.fill"
                                                   tintColor:orange],
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindCacheCleanup
                                                       title:@"Cache Cleanup"
                                                    subtitle:@"Clear current-title or all-title persistent shader caches."
                                                  symbolName:@"trash"
                                                   tintColor:orange],
                                          ]];

  XeniaSettingsHubSection* games =
      [XeniaSettingsHubSection sectionWithTitle:@"Games"
                                          items:@[
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindLibrary
                                                       title:@"Library & Storage"
                                                    subtitle:@"Import games, refresh the library, and manage local storage."
                                                  symbolName:@"externaldrive.fill"
                                                   tintColor:gray],
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindGameContent
                                                       title:@"Game Content & Patches"
                                                    subtitle:@"Title content, discs, packages, and patches from game tiles."
                                                  symbolName:@"puzzlepiece.extension.fill"
                                                   tintColor:gray],
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindPerGameSettings
                                                       title:@"Per-Game Settings"
                                                    subtitle:@"Title-specific overrides, search, reset, and batched saves."
                                                  symbolName:@"gamecontroller.fill"
                                                   tintColor:indigo],
                                          ]];

  XeniaSettingsHubSection* compatibility =
      [XeniaSettingsHubSection sectionWithTitle:@"Compatibility"
                                          items:@[
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindCompatibility
                                                       title:@"Compatibility Workarounds"
                                                    subtitle:@"GPU, memory, boot, and JIT title workarounds."
                                                  symbolName:@"wrench.and.screwdriver.fill"
                                                   tintColor:red],
                                          ]];

  XeniaSettingsHubSection* app = [XeniaSettingsHubSection
      sectionWithTitle:@"App"
                 items:@[
                   [XeniaSettingsHubItem
                       itemWithKind:XeniaSettingsHubCategoryKindAppearance
                              title:@"Appearance"
                           subtitle:@"App icon selection and interface presentation."
                         symbolName:@"paintpalette.fill"
                          tintColor:mint],
                   [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindAutomation
                                                title:@"Automation"
                                             subtitle:@"Local iOS automation options."
                                           symbolName:@"wand.and.stars"
                                            tintColor:mint],
                   [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindDiagnostics
                                                title:@"Diagnostics"
                                             subtitle:@"Log verbosity and live log viewer."
                                           symbolName:@"waveform.path.ecg"
                                            tintColor:red],
                   [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindAbout
                                                title:@"About"
                                             subtitle:@"Build information and project links."
                                           symbolName:@"info.circle.fill"
                                            tintColor:gray],
                 ]];

  XeniaSettingsHubSection* advanced =
      [XeniaSettingsHubSection sectionWithTitle:@"Advanced"
                                          items:@[
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindAdvancedDebug
                                                       title:@"Advanced Debug"
                                                    subtitle:@"Curated F8-style debug and compatibility overrides."
                                                  symbolName:@"slider.horizontal.3"
                                                   tintColor:purple],
                                            [XeniaSettingsHubItem
                                                itemWithKind:XeniaSettingsHubCategoryKindAllCvars
                                                       title:@"All Config"
                                                    subtitle:@"Search and edit every registered cvar."
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
  search_controller_.searchBar.placeholder = @"Search Settings";
  self.navigationItem.searchController = search_controller_;
  self.navigationItem.hidesSearchBarWhenScrolling = NO;
  self.definesPresentationContext = YES;
  if (shows_close_button_) {
    self.navigationItem.leftBarButtonItem =
        [[[UIBarButtonItem alloc] initWithTitle:@"Close"
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
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindControls
                                 title:@"Hardware Controllers"
                              subtitle:@"Controls · Auto-bind controllers connected before or during gameplay."
                            symbolName:@"gamecontroller.fill"
                             tintColor:blue],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindControls
                                 title:@"Touch Controls"
                              subtitle:@"Controls · Choose a layout before launch or adjust it while a game is running."
                            symbolName:@"hand.tap.fill"
                             tintColor:blue],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
                                 title:@"Edit Game Touch Layout"
                              subtitle:@"Controls · Choose an installed title and edit its touch controls before launch."
                            symbolName:@"gamecontroller.fill"
                             tintColor:blue],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
                                 title:@"Layout Library"
                              subtitle:@"Controls · Browse, rename, duplicate, import, export, and reset touch layouts."
                            symbolName:@"rectangle.grid.2x2.fill"
                             tintColor:blue],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
                                 title:@"Edit Running Touch Layout"
                              subtitle:@"Controls · Open the in-game touch editor for the currently running title."
                            symbolName:@"hand.draw.fill"
                             tintColor:blue],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
                                 title:@"Import Layout"
                              subtitle:@"Controls · Install a .toml layout from Files."
                            symbolName:@"square.and.arrow.down.fill"
                             tintColor:blue],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
                                 title:@"Export Running Layout"
                              subtitle:@"Controls · Share the layout currently loaded in the running game."
                            symbolName:@"square.and.arrow.up.fill"
                             tintColor:blue],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindTouchLayouts
                                 title:@"Reset Running Layout"
                              subtitle:@"Controls · Restore the running game's layout to the built-in XeniOS preset."
                            symbolName:@"arrow.counterclockwise"
                             tintColor:[XeniaTheme sectionIconOrange]],
  ];
  for (XeniaSettingsHubItem* item in items) {
    if (XeniaSettingsHubItemMatches(item, @"Controls", query)) {
      [results addObject:item];
    }
  }
}

- (void)appendLibrarySearchResults:(NSMutableArray*)results query:(NSString*)query {
  NSArray* items = @[
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindLibrary
                                 title:@"Import Game"
                              subtitle:@"Games · Open Files and copy a game or content package into XeniOS storage."
                            symbolName:@"plus.app.fill"
                             tintColor:[XeniaTheme accent]],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindLibrary
                                 title:@"Refresh Library"
                              subtitle:@"Games · Rescan XeniOS storage and update the launcher grid."
                            symbolName:@"arrow.clockwise"
                             tintColor:[XeniaTheme sectionIconGray]],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindCacheCleanup
                                 title:@"Clear Current Game Shader Cache"
                              subtitle:@"Graphics & Performance · Delete persistent shaders for the running title."
                            symbolName:@"trash"
                             tintColor:[XeniaTheme sectionIconOrange]],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindCacheCleanup
                                 title:@"Clear All Shader Caches"
                              subtitle:@"Graphics & Performance · Delete persistent shader storage for every title."
                            symbolName:@"trash.fill"
                             tintColor:[XeniaTheme sectionIconRed]],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindGameContent
                                 title:@"Game Content and Patches"
                              subtitle:@"Games · Manage title-specific content from a game tile."
                            symbolName:@"puzzlepiece.extension.fill"
                             tintColor:[XeniaTheme sectionIconGray]],
    [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindPerGameSettings
                                 title:@"Per-Game Settings"
                              subtitle:@"Games · Title-specific overrides, search, reset, and batched saves."
                            symbolName:@"gamecontroller.fill"
                             tintColor:[XeniaTheme sectionIconIndigo]],
  ];
  for (XeniaSettingsHubItem* item in items) {
    if (XeniaSettingsHubItemMatches(item, @"Library & Storage", query)) {
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
    XeniaAppendCatalogSearchResults(setting_results, kind, query);
  }
  if (setting_results.count) {
    [filtered_sections addObject:[XeniaSettingsHubSection sectionWithTitle:@"Settings"
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
  XEApplyAccessibility(cell, item.title, value, @"Opens this settings section.",
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
    item = [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindProfile
                                       title:@"Profile"
                                    subtitle:@"Manage local profiles and sign-in slots."
                                  symbolName:@"person.crop.circle.fill"
                                   tintColor:[XeniaTheme accent]];
  } else {
    item = [XeniaSettingsHubItem itemWithKind:XeniaSettingsHubCategoryKindDisplay
                                       title:@"Display"
                                    subtitle:@"Presenter output, aspect, and frame pacing."
                                  symbolName:@"display"
                                   tintColor:[XeniaTheme sectionIconIndigo]];
  }
  return [self makeDetailControllerForItem:item];
}

- (UIViewController*)makeDetailControllerForItem:(XeniaSettingsHubItem*)item {
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
                     title:@"Edit Game Touch Layout"
                  subtitle:@"Choose an installed title and edit its touch controls before launch."
                symbolName:@"gamecontroller.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionOpenTouchLayoutLibrary
                     title:@"Layout Library"
                  subtitle:@"Browse, rename, duplicate, import, export, and reset touch layouts."
                symbolName:@"rectangle.grid.2x2.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionEditTouchControls
                     title:@"Edit Running Touch Layout"
                  subtitle:@"Open the in-game touch editor for the currently running title."
                symbolName:@"hand.draw.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionImportTouchLayout
                     title:@"Import Layout"
                  subtitle:@"Install a .toml layout from Files."
                symbolName:@"square.and.arrow.down.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionExportTouchLayout
                     title:@"Export Running Layout"
                  subtitle:@"Share the layout currently loaded in the running game."
                symbolName:@"square.and.arrow.up.fill"
                 tintColor:blue],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionResetTouchLayout
                     title:@"Reset Running Layout"
                  subtitle:@"Restore the running game's layout to the built-in XeniOS preset."
                symbolName:@"arrow.counterclockwise"
                 tintColor:[XeniaTheme sectionIconOrange]],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:@"Touch Layouts"
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
        [XeniaSettingsActionListItem itemWithAction:XeniaSettingsHubActionImportGame
                                             title:@"Import Game"
                                          subtitle:@"Open Files and copy a game or content package into XeniOS storage."
                                        symbolName:@"plus.app.fill"
                                         tintColor:[XeniaTheme accent]],
        [XeniaSettingsActionListItem itemWithAction:XeniaSettingsHubActionRefreshLibrary
                                             title:@"Refresh Library"
                                          subtitle:@"Rescan XeniOS storage and update the launcher grid."
                                        symbolName:@"arrow.clockwise"
                                         tintColor:gray],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:@"Library & Storage"
                  items:items
          actionHandler:action_handler_] autorelease];
    }
    case XeniaSettingsHubCategoryKindCacheCleanup: {
      NSArray* items = @[
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionClearCurrentGameShaderCache
                     title:@"Clear Current Game Shader Cache"
                  subtitle:@"Delete persistent shaders for the running title. Relaunch the game before testing."
                symbolName:@"trash"
                 tintColor:[XeniaTheme sectionIconOrange]],
        [XeniaSettingsActionListItem
            itemWithAction:XeniaSettingsHubActionClearAllShaderCaches
                     title:@"Clear All Shader Caches"
                  subtitle:@"Delete persistent shader storage for every title. This does not remove games or saves."
                symbolName:@"trash.fill"
                 tintColor:[XeniaTheme sectionIconRed]],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:@"Cache Cleanup"
                  items:items
          actionHandler:action_handler_] autorelease];
    }
    case XeniaSettingsHubCategoryKindGameContent: {
      NSArray* items = @[
        [XeniaSettingsActionListItem
            infoItemWithTitle:@"Game Content and Patches"
                     subtitle:@"Manage title-specific installed content, discs, and patches from a game tile's context menu."
                   symbolName:@"puzzlepiece.extension.fill"],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:@"Game Content & Patches"
                  items:items
          actionHandler:action_handler_] autorelease];
    }
    case XeniaSettingsHubCategoryKindPerGameSettings: {
      NSArray* items = @[
        [XeniaSettingsActionListItem
            infoItemWithTitle:@"Title Overrides"
                     subtitle:@"Open Game Settings from a game tile to edit settings that apply only to that title."
                   symbolName:@"slider.horizontal.3"],
        [XeniaSettingsActionListItem
            infoItemWithTitle:@"Search Overrides"
                     subtitle:@"Search All Config appears at the top of each title's settings sheet."
                   symbolName:@"magnifyingglass"],
        [XeniaSettingsActionListItem
            infoItemWithTitle:@"Reset Game Settings"
                     subtitle:@"Reset appears in the same title settings sheet and deletes saved overrides for that title."
                   symbolName:@"arrow.counterclockwise"],
      ];
      return [[[XeniaSettingsActionListViewController alloc]
          initWithTitle:@"Per-Game Settings"
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
    self.title = @"Settings";
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
