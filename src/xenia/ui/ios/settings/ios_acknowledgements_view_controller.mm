/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_acknowledgements_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

NSString* const kAcknowledgementsResourceName = @"Acknowledgements";
NSString* const kAcknowledgementsFallbackText =
    @"The license notices could not be loaded from this build.\n\nXeniOS is based on Xenia "
    @"(BSD license) and includes third-party libraries. Their licenses are in the source "
    @"repository: https://github.com/xenios-jp/XeniOS";

}  // namespace

// One notice: shows the library's note (if any) above its license text.
@interface XeniaAcknowledgementDetailViewController : XESheetViewController
- (instancetype)initWithEntry:(NSDictionary*)entry;
@end

@implementation XeniaAcknowledgementDetailViewController {
  NSDictionary* entry_;
}

- (instancetype)initWithEntry:(NSDictionary*)entry {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    entry_ = [entry retain];
    NSString* name = entry[@"name"];
    self.title = [name isKindOfClass:[NSString class]] ? name : @"License";
  }
  return self;
}

- (void)dealloc {
  [entry_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];

  NSMutableString* text = [NSMutableString string];
  NSString* note = entry_[@"note"];
  if ([note isKindOfClass:[NSString class]] && note.length) {
    [text appendFormat:@"%@\n\n", note];
  }
  NSString* url = entry_[@"url"];
  if ([url isKindOfClass:[NSString class]] && url.length) {
    [text appendFormat:@"%@\n\n", url];
  }
  NSString* license = entry_[@"text"];
  if ([license isKindOfClass:[NSString class]]) {
    [text appendString:license];
  }

  UITextView* text_view = [[[UITextView alloc] init] autorelease];
  text_view.translatesAutoresizingMaskIntoConstraints = NO;
  text_view.editable = NO;
  text_view.alwaysBounceVertical = YES;
  text_view.backgroundColor = [UIColor clearColor];
  text_view.textColor = [XeniaTheme textPrimary];
  text_view.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
  text_view.adjustsFontForContentSizeCategory = YES;
  text_view.textContainerInset = UIEdgeInsetsMake(16.0, 12.0, 24.0, 12.0);
  text_view.dataDetectorTypes = UIDataDetectorTypeLink;
  text_view.text = text;
  text_view.accessibilityLabel = [NSString stringWithFormat:@"%@ license", self.title];
  [self.view addSubview:text_view];
  [NSLayoutConstraint activateConstraints:@[
    [text_view.topAnchor constraintEqualToAnchor:self.view.topAnchor],
    [text_view.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [text_view.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
    [text_view.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
  ]];
}

@end

@implementation XeniaAcknowledgementsViewController {
  NSArray<NSDictionary*>* entries_;
  BOOL loading_;
  BOOL failed_;
}

- (instancetype)init {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = @"Acknowledgements";
    loading_ = YES;
  }
  return self;
}

- (void)dealloc {
  [entries_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  // The file is a few hundred KB; parse it off the main thread.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSString* path = [[NSBundle mainBundle] pathForResource:kAcknowledgementsResourceName
                                                     ofType:@"json"];
    NSData* data = path ? [NSData dataWithContentsOfFile:path] : nil;
    id parsed = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSArray* raw = [parsed isKindOfClass:[NSDictionary class]] ? parsed[@"entries"] : nil;
    NSMutableArray* valid = [NSMutableArray array];
    if ([raw isKindOfClass:[NSArray class]]) {
      for (id item in raw) {
        if ([item isKindOfClass:[NSDictionary class]] &&
            [item[@"name"] isKindOfClass:[NSString class]]) {
          [valid addObject:item];
        }
      }
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      [entries_ release];
      entries_ = valid.count ? [valid copy] : nil;
      loading_ = NO;
      failed_ = valid.count == 0;
      [self.tableView reloadData];
    });
  });
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  return static_cast<NSInteger>(entries_.count);
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger)section {
  if (loading_) {
    return @"Loading…";
  }
  return failed_ ? kAcknowledgementsFallbackText
                 : @"XeniOS is based on Xenia and uses the open-source software listed above.";
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaAcknowledgementCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
  }
  NSDictionary* entry = entries_[indexPath.row];
  NSString* usage = [entry[@"usage"] isKindOfClass:[NSString class]] ? entry[@"usage"] : @"";
  cell.textLabel.text = entry[@"name"];
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.textLabel.numberOfLines = 0;
  cell.detailTextLabel.text = usage;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.detailTextLabel.numberOfLines = 0;
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  cell.accessibilityLabel = entry[@"name"];
  cell.accessibilityValue = usage;
  cell.accessibilityHint = @"Shows the license.";
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.row < 0 || indexPath.row >= static_cast<NSInteger>(entries_.count)) {
    return;
  }
  XeniaAcknowledgementDetailViewController* detail =
      [[XeniaAcknowledgementDetailViewController alloc] initWithEntry:entries_[indexPath.row]];
  [self.navigationController pushViewController:detail animated:YES];
  [detail release];
}

@end
