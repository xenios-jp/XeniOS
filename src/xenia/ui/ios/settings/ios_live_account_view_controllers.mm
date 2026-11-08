/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_live_account_view_controllers.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <vector>

#import "xenia/ui/ios/launcher/ios_system_link_view_controller.h"
#import "xenia/ui/ios/shared/ios_live_account.h"
#import "xenia/ui/ios/shared/ios_live_social_bridge.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

// The account screens are drawn like the Profile sheet they open from: inset grouped lists
// that follow the system appearance, ✕ and ✓ in the bar, text under each group, and rows
// tinted with the app's accent. Nothing here has a look of its own.

namespace {

NSString* DateText(NSDate* date) {
  static NSDateFormatter* formatter = nil;
  if (!formatter) {
    formatter = [[NSDateFormatter alloc] init];
    formatter.dateStyle = NSDateFormatterLongStyle;
    formatter.timeStyle = NSDateFormatterNoStyle;
  }
  return [formatter stringFromDate:date];
}

NSString* SizeText(uint64_t bytes) {
  return [NSByteCountFormatter stringFromByteCount:(long long)bytes
                                        countStyle:NSByteCountFormatterCountStyleFile];
}

UIViewController* TopOf(UIViewController* controller) {
  while (controller.presentedViewController) {
    controller = controller.presentedViewController;
  }
  return controller;
}

void ShowAlert(UIViewController* host, NSString* title, NSString* message) {
  UIAlertController* alert = [UIAlertController alertControllerWithTitle:title
                                                                 message:message
                                                          preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Account: alert button")
                                            style:UIAlertActionStyleDefault
                                          handler:nil]];
  [TopOf(host) presentViewController:alert animated:YES completion:nil];
}

NSString* LinkMessage(XeniaLiveLinkStatus status) {
  switch (status) {
    case XeniaLiveLinkStatusBadCode:
      return NSLocalizedString(@"That code isn’t right. Check it and try again.",
                               @"Account: code entry error");
    case XeniaLiveLinkStatusExpired:
      return NSLocalizedString(@"That code has expired. Make a new one on your other device.",
                               @"Account: code entry error");
    case XeniaLiveLinkStatusLimit:
      return NSLocalizedString(@"Too many tries. Wait a minute and try again.",
                               @"Account: code entry error");
    case XeniaLiveLinkStatusUnreachable:
      return NSLocalizedString(@"Can’t reach XeniOS. Check your connection.",
                               @"Account: code entry error");
    case XeniaLiveLinkStatusLinked:
      break;
  }
  return nil;
}

// "K7Q2-M9XD" from whatever was typed: letters and digits, upper case, hyphen every four.
NSString* FormattedCode(NSString* text, NSUInteger limit) {
  NSMutableString* out = [NSMutableString string];
  NSUInteger count = 0;
  NSString* upper = [text uppercaseString];
  for (NSUInteger i = 0; i < upper.length && count < limit; ++i) {
    const unichar c = [upper characterAtIndex:i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
      continue;
    }
    if (count && count % 4 == 0) {
      [out appendString:@"-"];
    }
    [out appendFormat:@"%C", c];
    ++count;
  }
  return out;
}

NSUInteger CodeLength(NSString* formatted) {
  return [formatted stringByReplacingOccurrencesOfString:@"-" withString:@""].length;
}

}  // namespace

// ---- Text above and below a group, which can change while a field keeps its focus ---------

@interface XEFormText : UIView
@property(nonatomic, readonly) UILabel* label;
- (CGFloat)heightForWidth:(CGFloat)width;
@end

@implementation XEFormText {
  UILabel* _label;
}
- (UILabel*)label {
  return _label;
}
- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _label = [[UILabel alloc] init];
    _label.translatesAutoresizingMaskIntoConstraints = NO;
    _label.numberOfLines = 0;
    xe_apply_label_font(_label, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular);
    _label.textColor = [XeniaTheme textSecondary];
    [self addSubview:_label];
    [NSLayoutConstraint activateConstraints:@[
      [_label.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:20],
      [_label.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-20],
      [_label.topAnchor constraintEqualToAnchor:self.topAnchor constant:6],
      [_label.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-6],
    ]];
  }
  return self;
}
- (void)dealloc {
  [_label release];
  [super dealloc];
}
- (CGFloat)heightForWidth:(CGFloat)width {
  return ceil([_label sizeThatFits:CGSizeMake(MAX(width - 40, 100), CGFLOAT_MAX)].height) + 14;
}
@end

// A grouped sheet list with the text above and below its first group as views of its own.
@interface XEForm : XESheetTableViewController
- (void)addCloseButton;
// The form's white commit button at the bottom; it calls doneTapped.
- (UIButton*)addDoneButtonWithTitle:(NSString*)title;
- (void)closeTapped;
- (void)doneTapped;
// Text above and below the first group; nil takes the space back.
- (void)setHeaderText:(nullable NSString*)text;
- (void)setFooterText:(nullable NSString*)text color:(nullable UIColor*)color;
@end

// Header and footer views sit inside the inset group's margins, so text is measured to that width.
static CGFloat TextWidth(UITableView* table) {
  const UIEdgeInsets margins = table.layoutMargins;
  return table.bounds.size.width - margins.left - margins.right;
}

@implementation XEForm {
  XEFormText* _header;
  XEFormText* _footer;
  NSString* _headerText;
  NSString* _footerText;
}

- (instancetype)init {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
    _header = [[XEFormText alloc] init];
    _footer = [[XEFormText alloc] init];
  }
  return self;
}

- (void)dealloc {
  [_header release];
  [_footer release];
  [_headerText release];
  [_footerText release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor clearColor];
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
}

- (void)addCloseButton {
  if (self.navigationController && self.navigationController.viewControllers.firstObject != self) {
    return;  // Pushed: the page picks up the root's ✕.
  }
  XESetSheetClose(self, self, @selector(closeTapped));
}

- (UIButton*)addDoneButtonWithTitle:(NSString*)title {
  return XESetFormCommit(self, title, self, @selector(doneTapped));
}

- (void)closeTapped {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)doneTapped {
}

- (void)setHeaderText:(NSString*)text {
  if ((!text.length && !_headerText.length) || [text isEqualToString:_headerText]) {
    return;
  }
  [_headerText release];
  _headerText = [text copy];
  _header.label.text = text;
  if (self.isViewLoaded) {
    [self.tableView reloadData];
  }
}

- (void)setFooterText:(NSString*)text color:(UIColor*)color {
  _footer.label.textColor = color ?: [XeniaTheme textSecondary];
  if ((!text.length && !_footerText.length) || [text isEqualToString:_footerText]) {
    return;
  }
  [_footerText release];
  _footerText = [text copy];
  _footer.label.text = text;
  if (self.isViewLoaded) {
    // Later, so the rows a caller is about to change have changed (an empty update checks them).
    XEForm* unsafe_self = [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
      [unsafe_self.tableView beginUpdates];
      [unsafe_self.tableView endUpdates];
      [unsafe_self release];
    });
  }
}

- (UIView*)tableView:(UITableView* __unused)tableView viewForHeaderInSection:(NSInteger)section {
  return section == 0 && _headerText.length ? _header : nil;
}

- (UIView*)tableView:(UITableView* __unused)tableView viewForFooterInSection:(NSInteger)section {
  return section == 0 && _footerText.length ? _footer : nil;
}

- (CGFloat)tableView:(UITableView*)tableView heightForHeaderInSection:(NSInteger)section {
  if (section == 0) {
    return _headerText.length ? [_header heightForWidth:TextWidth(tableView)] : 12;
  }
  return UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView*)tableView heightForFooterInSection:(NSInteger)section {
  if (section == 0) {
    return _footerText.length ? [_footer heightForWidth:TextWidth(tableView)] : 12;
  }
  return UITableViewAutomaticDimension;
}

@end

namespace {

// A form is a page on the wash (docs/ios_design_language.md, rule 2): full screen on iPhone,
// the page size on iPad, in a navigation controller. |modal| blocks swiping it away.
void PresentForm(UIViewController* presenter, XEForm* form, BOOL modal) {
  UINavigationController* navigation =
      [[[UINavigationController alloc] initWithRootViewController:form] autorelease];
  navigation.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  navigation.view.backgroundColor = [XeniaTheme bgPrimary];
  XEConfigureDestinationPresentation(navigation, TopOf(presenter).view,
                                     CGSizeMake(620.0, 720.0), modal);
  navigation.navigationBar.prefersLargeTitles = NO;
  [TopOf(presenter) presentViewController:navigation animated:YES completion:nil];
}

// A row that does something: the accent colour, like Add Profile….
UITableViewCell* ActionCell(NSString* title) {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
  content.text = title;
  content.textProperties.color = [XeniaTheme accent];
  cell.contentConfiguration = content;
  return cell;
}

// A cell that holds a text field the form keeps (so it keeps its focus when the list reloads).
UITableViewCell* FieldCell(UITextField* field) {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  field.translatesAutoresizingMaskIntoConstraints = NO;
  [cell.contentView addSubview:field];
  [NSLayoutConstraint activateConstraints:@[
    [field.leadingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.leadingAnchor],
    [field.trailingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.trailingAnchor],
    [field.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor],
    [field.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor],
    [field.heightAnchor constraintGreaterThanOrEqualToConstant:52.0],
  ]];
  return cell;
}

// A big monospaced code in a row of its own.
UITableViewCell* CodeCell(NSString* text, CGFloat size, UIColor* color, CGFloat padding) {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.text = text;
  label.textAlignment = NSTextAlignmentCenter;
  label.textColor = color;
  label.font = [UIFont monospacedSystemFontOfSize:size weight:UIFontWeightBold];
  label.adjustsFontSizeToFitWidth = YES;
  label.accessibilityLabel = [[text componentsSeparatedByString:@"-"] componentsJoinedByString:@" "];
  [cell.contentView addSubview:label];
  [NSLayoutConstraint activateConstraints:@[
    [label.leadingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.leadingAnchor],
    [label.trailingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.trailingAnchor],
    [label.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:padding],
    [label.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-padding],
  ]];
  return cell;
}

// A plain symbol for a row, as on every other list.
UIImage* Glyph(NSString* symbol) { return XERowSymbolImage(symbol, nil); }

}  // namespace

// ---- The gamertag form: rules on the device, then the lobby, with suggestions -----------------

typedef NS_ENUM(NSInteger, XEGamertagMode) {
  XEGamertagModeGoOnline = 0,
  XEGamertagModeRename,
  XEGamertagModeFlagged,
};

typedef NS_ENUM(NSInteger, XEGamertagState) {
  XEGamertagStateIdle = 0,  // Nothing typed.
  XEGamertagStateSame,      // Rename: it's the current name.
  XEGamertagStateLocal,     // Fails the 360 rules; the message says which.
  XEGamertagStateChecking,
  XEGamertagStateAvailable,
  XEGamertagStateTaken,
  XEGamertagStateUnusable,  // Reserved or not allowed: one wording for both.
  XEGamertagStateUnreachable,
  XEGamertagStateLimit,
  XEGamertagStateLocked,    // Rename too soon.
  XEGamertagStateLost,      // Free a moment ago, taken at claim time.
};

@interface XEGamertagForm : XEForm <UITextFieldDelegate>
- (instancetype)initWithMode:(XEGamertagMode)mode
                 currentName:(NSString*)current
                        done:(void (^)(XeniaLiveNameResult* result))done;
@end

@implementation XEGamertagForm {
  XEGamertagMode _mode;
  NSString* _current;
  void (^_done)(XeniaLiveNameResult*);
  UITextField* _field;
  UIImageView* _icon;
  UIActivityIndicatorView* _spinner;
  UILabel* _counter;
  UIButton* _doneItem;
  XEGamertagState _state;
  NSUInteger _generation;
  NSArray<NSString*>* _suggestions;
  NSString* _message;
  NSDate* _lockedUntil;
  BOOL _claiming;
}

- (instancetype)initWithMode:(XEGamertagMode)mode
                 currentName:(NSString*)current
                        done:(void (^)(XeniaLiveNameResult*))done {
  if ((self = [super init])) {
    _mode = mode;
    _current = [current copy];
    _done = [done copy];
    _suggestions = [@[] retain];
  }
  return self;
}

- (void)dealloc {
  [NSObject cancelPreviousPerformRequestsWithTarget:self];
  [_current release];
  [_done release];
  [_field release];
  [_icon release];
  [_spinner release];
  [_counter release];
  [_doneItem release];
  [_suggestions release];
  [_message release];
  [_lockedUntil release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  switch (_mode) {
    case XEGamertagModeGoOnline:
      self.title = NSLocalizedString(@"Choose Your Gamertag", @"Gamertag sheet title");
      [self setHeaderText:[NSString stringWithFormat:NSLocalizedString(
                                                         @"“%@” becomes this gamertag. Friends find "
                                                         @"you by it, and your saves and "
                                                         @"achievements stay.",
                                                         @"Gamertag sheet: text above the field; %@ "
                                                         @"is the profile's name"),
                                                     _current]];
      break;
    case XEGamertagModeRename:
      self.title = NSLocalizedString(@"Change Gamertag", @"Gamertag sheet title");
      [self setHeaderText:NSLocalizedString(@"Friends see the new name at once. You can change it "
                                            @"once every 30 days.",
                                            @"Gamertag sheet: text above the field")];
      break;
    case XEGamertagModeFlagged:
      self.title = NSLocalizedString(@"Pick a New Gamertag", @"Gamertag sheet title");
      [self setHeaderText:
                [NSString stringWithFormat:NSLocalizedString(
                                               @"Another player had “%@” first, and gamertags are "
                                               @"unique now. You keep it until you pick a new one, "
                                               @"with no wait.",
                                               @"Gamertag sheet: text above the field; %@ is the "
                                               @"player's current gamertag"),
                                           _current]];
      break;
  }
  [self addCloseButton];
  _doneItem = [[self addDoneButtonWithTitle:_mode == XEGamertagModeGoOnline
                                                ? NSLocalizedString(@"Get Gamertag",
                                                                    @"Gamertag sheet: button")
                                                : NSLocalizedString(@"Save Gamertag",
                                                                    @"Gamertag sheet: button")]
      retain];

  _field = [[UITextField alloc] init];
  _field.delegate = self;
  _field.placeholder = NSLocalizedString(@"Gamertag", @"Gamertag sheet: field placeholder");
  _field.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  _field.adjustsFontForContentSizeCategory = YES;
  _field.textColor = [XeniaTheme textPrimary];
  _field.autocapitalizationType = UITextAutocapitalizationTypeWords;
  _field.autocorrectionType = UITextAutocorrectionTypeNo;
  _field.spellCheckingType = UITextSpellCheckingTypeNo;
  _field.returnKeyType = UIReturnKeyDone;
  _field.accessibilityLabel = NSLocalizedString(@"Gamertag", @"Gamertag sheet: field placeholder");
  [_field addTarget:self action:@selector(textChanged) forControlEvents:UIControlEventEditingChanged];

  // What the check found, at the end of the field.
  _icon = [[UIImageView alloc] init];
  _icon.preferredSymbolConfiguration =
      [UIImageSymbolConfiguration configurationWithPointSize:20 weight:UIImageSymbolWeightRegular];
  _icon.contentMode = UIViewContentModeCenter;
  _icon.isAccessibilityElement = NO;
  _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
  _counter = [[UILabel alloc] init];
  _counter.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightRegular];
  _counter.textAlignment = NSTextAlignmentRight;
  UIView* end = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 48, 28)] autorelease];
  for (UIView* view in @[ _icon, _spinner, _counter ]) {
    view.frame = CGRectMake(0, 0, 48, 28);
    [end addSubview:view];
  }
  _field.rightView = end;
  _field.rightViewMode = UITextFieldViewModeAlways;

  if (_mode != XEGamertagModeFlagged) {
    _field.text = _current;
  }
  [self textChanged];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [_field becomeFirstResponder];
}

- (BOOL)textFieldShouldReturn:(UITextField* __unused)field {
  [self doneTapped];
  return NO;
}

- (void)setState:(XEGamertagState)state {
  _state = state;
  [self render];
}

- (void)setMessage:(NSString*)message {
  [_message release];
  _message = [message copy];
}

- (void)setLockedUntil:(NSDate*)date {
  [_lockedUntil release];
  _lockedUntil = [date retain];
}

- (void)setSuggestionList:(NSArray<NSString*>*)list {
  [_suggestions release];
  _suggestions = [list copy] ?: [@[] retain];
}

// Rules on the device first, instantly; then, after a pause, the lobby.
- (void)textChanged {
  [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(runCheck) object:nil];
  ++_generation;
  [self setSuggestionList:@[]];
  [self setMessage:nil];
  [self setLockedUntil:nil];
  NSString* text = _field.text ?: @"";
  if (!text.length) {
    [self setState:XEGamertagStateIdle];
    return;
  }
  NSString* local = xe_ios_gamertag_local_error(text);
  if (local.length) {
    [self setMessage:local];
    [self setState:XEGamertagStateLocal];
    return;
  }
  if (_mode == XEGamertagModeRename) {
    if ([text isEqualToString:_current]) {
      [self setState:XEGamertagStateSame];
      return;
    }
    XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
    const BOOL capitals_only = [text.lowercaseString isEqualToString:_current.lowercaseString];
    if (summary.nextRename.timeIntervalSinceNow > 0 && !capitals_only && !summary.renameFlagged) {
      [self setLockedUntil:summary.nextRename];
      [self setState:XEGamertagStateLocked];
      return;
    }
  }
  [self setState:XEGamertagStateChecking];
  [self performSelector:@selector(runCheck) withObject:nil afterDelay:0.3];
}

- (void)runCheck {
  const NSUInteger generation = _generation;
  NSString* text = [[_field.text copy] autorelease];
  // The answer can come after the sheet is closed, so the block keeps the form.
  [xe_ios_live_account_service()
      checkGamertag:text
         completion:^(XeniaLiveNameResult* result) {
           if (generation != self->_generation) {
             return;
           }
           [self applyCheck:result forName:text];
         }];
}

- (void)applyCheck:(XeniaLiveNameResult*)result forName:(NSString*)name {
  [self setSuggestionList:result.suggestions];
  switch (result.status) {
    case XeniaLiveNameStatusAvailable:
      [self setState:XEGamertagStateAvailable];
      break;
    case XeniaLiveNameStatusTaken:
      [self setMessage:[NSString stringWithFormat:NSLocalizedString(@"%@ is taken",
                                                                    @"Gamertag field: message; %@ "
                                                                    @"is the name"),
                                                  name]];
      [self setState:XEGamertagStateTaken];
      break;
    case XeniaLiveNameStatusReserved:
    case XeniaLiveNameStatusNotAllowed:
      [self setState:XEGamertagStateUnusable];
      break;
    case XeniaLiveNameStatusInvalid:
      [self setMessage:xe_ios_gamertag_local_error(name)
                           ?: NSLocalizedString(@"That name can’t be used",
                                                @"Gamertag field: message")];
      [self setState:XEGamertagStateLocal];
      break;
    case XeniaLiveNameStatusLimit:
      [self setState:XEGamertagStateLimit];
      [self performSelector:@selector(runCheck) withObject:nil afterDelay:6.0];
      break;
    case XeniaLiveNameStatusTooSoon:
      [self setLockedUntil:result.nextRename];
      [self setState:XEGamertagStateLocked];
      break;
    case XeniaLiveNameStatusBanned:
      [self setMessage:NSLocalizedString(@"This account can’t change its gamertag.",
                                         @"Gamertag field: message")];
      [self setState:XEGamertagStateLocal];
      break;
    case XeniaLiveNameStatusUnreachable:
      [self setState:XEGamertagStateUnreachable];
      break;
  }
}

- (void)doneTapped {
  if (_claiming || !_doneItem.enabled) {
    return;
  }
  NSString* name = [[_field.text copy] autorelease];
  _claiming = YES;
  [_field resignFirstResponder];
  XESetFormCommitBusy(self, YES);
  // Going online can finish after the sheet is closed, so the block keeps the form.
  void (^finish)(XeniaLiveNameResult*) = ^(XeniaLiveNameResult* result) {
    [self claimFinished:result name:name];
  };
  if (_mode == XEGamertagModeGoOnline) {
    [xe_ios_live_account_service() goOnlineWithGamertag:name completion:finish];
  } else {
    [xe_ios_live_account_service() renameToGamertag:name completion:finish];
  }
}

- (void)claimFinished:(XeniaLiveNameResult*)result name:(NSString*)name {
  _claiming = NO;
  XESetFormCommitBusy(self, NO);
  if (result.status == XeniaLiveNameStatusAvailable) {
    void (^done)(XeniaLiveNameResult*) = [[_done copy] autorelease];
    XeniaLiveNameResult* held = [[result retain] autorelease];
    [self dismissViewControllerAnimated:YES
                             completion:^{
                               if (done) {
                                 done(held);
                               }
                             }];
    return;
  }
  [self setSuggestionList:result.suggestions];
  switch (result.status) {
    case XeniaLiveNameStatusTaken:
    case XeniaLiveNameStatusReserved:
    case XeniaLiveNameStatusNotAllowed:
      // Free a moment ago: say so, with fresh suggestions.
      [self setMessage:[NSString stringWithFormat:NSLocalizedString(
                                                      @"%@ was just taken. Pick another.",
                                                      @"Gamertag sheet: after a lost race; %@ is "
                                                      @"the name"),
                                                  name]];
      [self setState:XEGamertagStateLost];
      break;
    case XeniaLiveNameStatusTooSoon:
      [self setLockedUntil:result.nextRename];
      [self setState:XEGamertagStateLocked];
      break;
    case XeniaLiveNameStatusLimit:
      [self setMessage:NSLocalizedString(@"Too many tries. Wait a moment and try again.",
                                         @"Gamertag sheet: message")];
      [self setState:XEGamertagStateLocal];
      break;
    case XeniaLiveNameStatusBanned:
      [self setMessage:NSLocalizedString(@"This account can’t change its gamertag.",
                                         @"Gamertag field: message")];
      [self setState:XEGamertagStateLocal];
      break;
    case XeniaLiveNameStatusUnreachable:
      [self setMessage:NSLocalizedString(@"Can’t reach XeniOS. Check your connection and try again.",
                                         @"Gamertag sheet: message")];
      [self setState:XEGamertagStateLocal];
      break;
    default:
      [self setMessage:xe_ios_gamertag_local_error(name)
                           ?: NSLocalizedString(@"That name can’t be used",
                                                @"Gamertag field: message")];
      [self setState:XEGamertagStateLocal];
      break;
  }
}

- (void)render {
  NSString* text = _field.text ?: @"";
  NSString* symbol = nil;
  UIColor* symbol_color = nil;
  NSString* message = nil;
  UIColor* message_color = [XeniaTheme textSecondary];
  BOOL can_claim = NO;
  BOOL spin = NO;
  BOOL show_suggestions = NO;
  switch (_state) {
    case XEGamertagStateIdle:
      break;
    case XEGamertagStateSame:
      message = NSLocalizedString(@"That’s your current gamertag", @"Gamertag field: message");
      break;
    case XEGamertagStateLocal:
      symbol = @"xmark.circle.fill";
      symbol_color = [UIColor systemRedColor];
      message = _message;
      message_color = [UIColor systemRedColor];
      break;
    case XEGamertagStateChecking:
      spin = YES;
      message = NSLocalizedString(@"Checking…", @"Gamertag field: message");
      break;
    case XEGamertagStateAvailable:
      symbol = @"checkmark.circle.fill";
      symbol_color = [UIColor systemGreenColor];
      message = NSLocalizedString(@"Available", @"Gamertag field: message");
      message_color = [UIColor systemGreenColor];
      can_claim = YES;
      break;
    case XEGamertagStateTaken:
      symbol = @"xmark.circle.fill";
      symbol_color = [UIColor systemRedColor];
      message = _message;
      message_color = [UIColor systemRedColor];
      show_suggestions = YES;
      break;
    case XEGamertagStateUnusable:
      symbol = @"xmark.circle.fill";
      symbol_color = [UIColor systemRedColor];
      message = NSLocalizedString(@"That name can’t be used", @"Gamertag field: message");
      message_color = [UIColor systemRedColor];
      show_suggestions = YES;
      break;
    case XEGamertagStateUnreachable:
      symbol = @"wifi.slash";
      symbol_color = [XeniaTheme textSecondary];
      message = NSLocalizedString(@"Can’t check right now. You can still claim it; the name is "
                                  @"checked then.",
                                  @"Gamertag field: message");
      can_claim = YES;  // The lobby decides when it claims.
      break;
    case XEGamertagStateLimit:
      spin = YES;
      message = NSLocalizedString(@"Checking too fast. One moment…", @"Gamertag field: message");
      break;
    case XEGamertagStateLocked:
      symbol = @"lock.fill";
      symbol_color = [UIColor systemOrangeColor];
      message = [NSString
          stringWithFormat:NSLocalizedString(@"You can change your gamertag again on %@. Changing "
                                             @"only capitals is always free.",
                                             @"Gamertag field: message; %@ is a date"),
                           _lockedUntil ? DateText(_lockedUntil) : @""];
      message_color = [UIColor systemOrangeColor];
      break;
    case XEGamertagStateLost:
      symbol = @"xmark.circle.fill";
      symbol_color = [UIColor systemRedColor];
      message = _message;
      message_color = [UIColor systemOrangeColor];
      show_suggestions = YES;
      break;
  }
  _icon.image = symbol ? [UIImage systemImageNamed:symbol] : nil;
  _icon.tintColor = symbol_color;
  _icon.hidden = symbol == nil;
  _counter.text = [NSString stringWithFormat:@"%lu/15", (unsigned long)text.length];
  _counter.textColor = text.length > 15 ? [UIColor systemRedColor] : [XeniaTheme textSecondary];
  _counter.hidden = !(text.length >= 12 && symbol == nil && !spin);
  if (spin) {
    [_spinner startAnimating];
  } else {
    [_spinner stopAnimating];
  }
  [self setFooterText:message color:message_color];
  _doneItem.enabled = can_claim && !_claiming;
  if (!show_suggestions) {
    [self setSuggestionList:@[]];
  }
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:1]
                withRowAnimation:UITableViewRowAnimationNone];
  if (UIAccessibilityIsVoiceOverRunning() && message.length &&
      (_state == XEGamertagStateAvailable || _state == XEGamertagStateTaken ||
       _state == XEGamertagStateUnusable)) {
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, message);
  }
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? 1 : (NSInteger)_suggestions.count;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return section == 1 && _suggestions.count
             ? NSLocalizedString(@"Try", @"Gamertag sheet: header over the suggestions")
             : nil;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 0) {
    return FieldCell(_field);
  }
  UITableViewCell* cell = ActionCell(_suggestions[(NSUInteger)indexPath.row]);
  cell.accessibilityHint = NSLocalizedString(@"Uses this gamertag.", @"Gamertag sheet: row hint");
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 1) {
    _field.text = _suggestions[(NSUInteger)indexPath.row];
    [self textChanged];
  }
}

@end

// ---- The recovery code, once -----------------------------------------------------------------

@interface XERecoveryForm : XEForm
- (instancetype)initWithCode:(NSString*)code firstTime:(BOOL)first done:(void (^)(void))done;
@end

@implementation XERecoveryForm {
  NSString* _code;
  BOOL _first;
  void (^_done)(void);
  UIButton* _doneItem;
  BOOL _saved;
  BOOL _copied;
}

- (instancetype)initWithCode:(NSString*)code firstTime:(BOOL)first done:(void (^)(void))done {
  if ((self = [super init])) {
    _code = [code copy];
    _first = first;
    _done = [done copy];
  }
  return self;
}

- (void)dealloc {
  [_code release];
  [_done release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Recovery Code", @"Recovery code sheet title");
  [self setHeaderText:_first
                          ? NSLocalizedString(@"Keep this code somewhere safe. It’s the only way "
                                              @"back into your account if you lose every device. "
                                              @"It’s shown once.",
                                              @"Recovery code sheet: text above the code")
                          : NSLocalizedString(@"This is your new code. The old one no longer works. "
                                              @"It’s shown once.",
                                              @"Recovery code sheet: text above the code")];
  // No ✕: it stays until the code is saved.
  _doneItem = [self addDoneButtonWithTitle:NSLocalizedString(@"Done",
                                                             @"Recovery code sheet: button")];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 3;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 1 ? 2 : 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 0) {
    return CodeCell(_code, 26, [XeniaTheme textPrimary], 22);
  }
  if (indexPath.section == 1) {
    return ActionCell(indexPath.row == 0
                          ? (_copied ? NSLocalizedString(@"Copied", @"Recovery code sheet: row after copying")
                                     : NSLocalizedString(@"Copy", @"Recovery code sheet: row"))
                          : NSLocalizedString(@"Save to Files…", @"Recovery code sheet: row"));
  }
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
  content.text = NSLocalizedString(@"I’ve kept it somewhere safe", @"Recovery code sheet: switch");
  cell.contentConfiguration = content;
  UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
  toggle.on = _saved;
  toggle.onTintColor = [XeniaTheme accent];
  toggle.accessibilityLabel = content.text;
  [toggle addTarget:self action:@selector(savedChanged:) forControlEvents:UIControlEventValueChanged];
  cell.accessoryView = toggle;
  return cell;
}

- (void)savedChanged:(UISwitch*)toggle {
  _saved = toggle.on;
  _doneItem.enabled = _saved;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section != 1) {
    return;
  }
  if (indexPath.row == 0) {
    // Local to this device and gone in two minutes: it's a key to the account.
    [[UIPasteboard generalPasteboard]
        setItems:@[ @{@"public.utf8-plain-text" : _code} ]
         options:@{
           UIPasteboardOptionLocalOnly : @YES,
           UIPasteboardOptionExpirationDate : [NSDate dateWithTimeIntervalSinceNow:120]
         }];
    _copied = YES;
    [tableView reloadRowsAtIndexPaths:@[ indexPath ] withRowAnimation:UITableViewRowAnimationNone];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification,
                                    NSLocalizedString(@"Copied", @"Recovery code sheet: row after copying"));
    return;
  }
  NSURL* url = [NSURL fileURLWithPath:[NSTemporaryDirectory()
                                          stringByAppendingPathComponent:@"XeniOS Recovery Code.txt"]];
  NSString* text = [NSString
      stringWithFormat:NSLocalizedString(@"XeniOS recovery code: %@\nKeep it private. Anyone with "
                                         @"it can take over your account.\n",
                                         @"Recovery code file text; %@ is the code"),
                       _code];
  [text writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:nil];
  UIActivityViewController* share =
      [[[UIActivityViewController alloc] initWithActivityItems:@[ url ] applicationActivities:nil]
          autorelease];
  share.popoverPresentationController.sourceView = tableView;
  share.popoverPresentationController.sourceRect = [tableView rectForRowAtIndexPath:indexPath];
  share.completionWithItemsHandler = ^(UIActivityType __unused type, BOOL __unused completed,
                                       NSArray* __unused items, NSError* __unused error) {
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
  };
  [self presentViewController:share animated:YES completion:nil];
}

- (void)doneTapped {
  if (!_saved) {
    return;
  }
  void (^done)(void) = [[_done copy] autorelease];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             if (done) {
                               done();
                             }
                           }];
}

@end

// ---- Saves between profiles on this device ----------------------------------------------

@interface XEShareProfileViewController : XESheetTableViewController
- (instancetype)initWithProfile:(XeniaLiveLocalProfile*)profile;
@end

@implementation XEShareProfileViewController {
  XeniaLiveLocalProfile* _profile;
  NSArray<XeniaLiveSaveGame*>* _games;
}

- (instancetype)initWithProfile:(XeniaLiveLocalProfile*)profile {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
    _profile = [profile retain];
  }
  return self;
}

- (void)dealloc {
  [_profile release];
  [_games release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = _profile.gamertag;
  [self reload];
}

- (void)reload {
  __unsafe_unretained XEShareProfileViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      saveGamesSharedWithProfile:_profile.profileId
                      completion:^(NSArray<XeniaLiveSaveGame*>* games) {
                        [unsafe_self->_games release];
                        unsafe_self->_games = [games copy];
                        [unsafe_self.tableView reloadData];
                      }];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  return (NSInteger)_games.count;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger __unused)section {
  return _games.count ? [NSString stringWithFormat:NSLocalizedString(
                                                       @"A shared game uses one set of saves for both "
                                                       @"profiles. %@’s own saves for it are kept as a "
                                                       @"backup.",
                                                       @"Share saves: footer; %@ is a gamertag"),
                                                   _profile.gamertag]
                      : NSLocalizedString(@"No games have saves yet.", @"Share saves: empty");
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaLiveSaveGame* game = _games[(NSUInteger)indexPath.row];
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = game.name;
  content.secondaryText = SizeText(game.bytes);
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  cell.contentConfiguration = content;
  UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
  toggle.on = game.shared;
  toggle.onTintColor = [XeniaTheme accent];
  toggle.tag = indexPath.row;
  toggle.accessibilityLabel = game.name;
  [toggle addTarget:self action:@selector(toggled:) forControlEvents:UIControlEventValueChanged];
  cell.accessoryView = toggle;
  return cell;
}

- (void)toggled:(UISwitch*)toggle {
  XeniaLiveSaveGame* game = _games[(NSUInteger)toggle.tag];
  __unsafe_unretained XEShareProfileViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      setSharing:toggle.on
        forTitle:game.titleId
     withProfile:_profile.profileId
      completion:^(NSError* error) {
        if (error) {
          ShowAlert(unsafe_self, NSLocalizedString(@"Couldn’t Change Sharing", @"Share saves: alert title"),
                    error.localizedDescription);
        }
        [unsafe_self reload];
      }];
}

@end

@interface XEShareHubViewController : XESheetTableViewController
@end

@implementation XEShareHubViewController {
  NSArray<XeniaLiveLocalProfile*>* _others;
  NSMutableDictionary<NSString*, NSNumber*>* _sharedCounts;  // Games shared, by profile ID.
  BOOL _busy;
}

- (instancetype)init {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
  }
  return self;
}

- (void)dealloc {
  [_others release];
  [_sharedCounts release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Shared Saves", @"Shared saves title");
  [self reload];
}

- (void)reload {
  __unsafe_unretained XEShareHubViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      localProfilesWithCompletion:^(NSArray<XeniaLiveLocalProfile*>* profiles) {
        NSMutableArray* others = [NSMutableArray array];
        for (XeniaLiveLocalProfile* profile in profiles) {
          if (!profile.thisProfile) {
            [others addObject:profile];
          }
        }
        [unsafe_self->_others release];
        unsafe_self->_others = [others copy];
        [unsafe_self.tableView reloadData];
        for (XeniaLiveLocalProfile* other in others) {
          [xe_ios_live_account_service()
              saveGamesSharedWithProfile:other.profileId
                              completion:^(NSArray<XeniaLiveSaveGame*>* games) {
                                NSUInteger shared = 0;
                                for (XeniaLiveSaveGame* game in games) {
                                  shared += game.shared ? 1 : 0;
                                }
                                if (!unsafe_self->_sharedCounts) {
                                  unsafe_self->_sharedCounts = [[NSMutableDictionary alloc] init];
                                }
                                unsafe_self->_sharedCounts[other.profileId] = @(shared);
                                [unsafe_self.tableView reloadData];
                              }];
        }
      }];
}

- (BOOL)all {
  return xe_ios_live_account_service().savesSharedAcrossProfiles;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return [self all] ? 1 : 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? 1 : (NSInteger)_others.count;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return section == 1 ? NSLocalizedString(@"Or Just Some Profiles", @"Share saves: header") : nil;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (section == 0) {
    return [self all] ? NSLocalizedString(@"Every profile on this device uses one set of saves. Turn it off to give each its own copy again.",
                                          @"Share saves: footer")
                      : NSLocalizedString(@"Every profile on this device uses one set of saves. They’re merged, "
                                          @"keeping the newest, and the originals are backed up first.",
                                          @"Share saves: footer");
  }
  return _others.count ? nil : NSLocalizedString(@"There are no other profiles on this device.",
                                                 @"Share saves: empty");
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
  if (indexPath.section == 0) {
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    content.text = NSLocalizedString(@"Share With Every Profile", @"Share saves: switch");
    cell.contentConfiguration = content;
    UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
    toggle.on = [self all];
    toggle.onTintColor = [XeniaTheme accent];
    toggle.accessibilityLabel = content.text;
    [toggle addTarget:self action:@selector(allToggled:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
    return cell;
  }
  XeniaLiveLocalProfile* profile = _others[(NSUInteger)indexPath.row];
  content = [UIListContentConfiguration valueCellConfiguration];
  content.text = profile.gamertag;
  content.image = xe_profile_avatar_image(profile.gamertag, NO, 32.0);
  NSNumber* count = _sharedCounts[profile.profileId];
  if (count) {
    content.secondaryText =
        count.unsignedIntegerValue == 0
            ? NSLocalizedString(@"Off", @"Share saves: row detail")
            : count.unsignedIntegerValue == 1
                  ? NSLocalizedString(@"1 Game", @"Share saves: row detail")
                  : [NSString stringWithFormat:NSLocalizedString(@"%lu Games", @"Share saves: row detail; a count"),
                                               (unsigned long)count.unsignedIntegerValue];
  }
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  cell.contentConfiguration = content;
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  return cell;
}

- (void)allToggled:(UISwitch*)toggle {
  if (_busy) {
    return;
  }
  const BOOL on = toggle.on;
  __unsafe_unretained XEShareHubViewController* unsafe_self = self;
  void (^apply)(void) = ^{
    unsafe_self->_busy = YES;
    [xe_ios_live_account_service()
        setSavesSharedAcrossProfiles:on
                          completion:^(NSError* error) {
                            unsafe_self->_busy = NO;
                            if (error) {
                              ShowAlert(unsafe_self,
                                        NSLocalizedString(@"Couldn’t Change Sharing", @"Share saves: alert title"),
                                        error.localizedDescription);
                            }
                            [unsafe_self.tableView reloadData];
                          }];
  };
  if (!on) {
    apply();
    return;
  }
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Share Saves With Every Profile?",
                                                 @"Share saves: alert title")
                       message:NSLocalizedString(@"Saves from every profile are merged, keeping the newest "
                                                 @"of each. The originals are backed up first. Don’t play "
                                                 @"while this is running.",
                                                 @"Share saves: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Share saves: alert button")
                                            style:UIAlertActionStyleCancel
                                          handler:^(UIAlertAction* __unused a) {
                                            [unsafe_self.tableView reloadData];
                                          }]];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Share", @"Share saves: alert button")
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction* __unused a) {
                                            apply();
                                          }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 1) {
    [self.navigationController
        pushViewController:[[[XEShareProfileViewController alloc]
                               initWithProfile:_others[(NSUInteger)indexPath.row]] autorelease]
                  animated:YES];
  }
}

@end

// ---- QR code, drawn with round dots ------------------------------------------------------

namespace {

// The QR code for |text| as an image of |side| points: round dots, rounded finder squares and a
// small mint mark in the middle (error correction Q leaves room for it). Black on clear, so it
// scans on a white tile in light and dark.
UIImage* QRImage(NSString* text, CGFloat side) {
  CIFilter* filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
  [filter setValue:[text dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"];
  [filter setValue:@"Q" forKey:@"inputCorrectionLevel"];
  CIImage* ci = filter.outputImage;
  if (!ci) {
    return nil;
  }
  const size_t n = (size_t)ci.extent.size.width;
  CIContext* context = [CIContext contextWithOptions:nil];
  CGImageRef cg = [context createCGImage:ci fromRect:ci.extent];
  if (!cg || !n) {
    return nil;
  }
  std::vector<uint8_t> pixels(n * n * 4);
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGContextRef bitmap = CGBitmapContextCreate(pixels.data(), n, n, 8, n * 4, space,
                                              kCGImageAlphaPremultipliedLast);
  CGContextDrawImage(bitmap, CGRectMake(0, 0, n, n), cg);
  CGContextRelease(bitmap);
  CGColorSpaceRelease(space);
  CGImageRelease(cg);
  auto dark = [&](size_t x, size_t y) { return pixels[(y * n + x) * 4] < 128; };
  // The generator's output has a one-module quiet border: the code itself starts at 1.
  auto in_eye = [&](size_t x, size_t y) {
    return (x < 9 && y < 9) || (x >= n - 9 && y < 9) || (x < 9 && y >= n - 9);
  };
  const CGFloat unit = side / (CGFloat)n;
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(side, side)] autorelease];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* __unused ctx) {
    [[UIColor blackColor] setFill];
    const size_t mid = n / 2;
    for (size_t y = 0; y < n; ++y) {
      for (size_t x = 0; x < n; ++x) {
        if (!dark(x, y) || in_eye(x, y)) {
          continue;
        }
        if (x + 2 >= mid && x <= mid + 2 && y + 2 >= mid && y <= mid + 2) {
          continue;  // Under the mark.
        }
        CGRect dot = CGRectMake((CGFloat)x * unit + unit * 0.06, (CGFloat)y * unit + unit * 0.06,
                                unit * 0.88, unit * 0.88);
        [[UIBezierPath bezierPathWithRoundedRect:dot cornerRadius:unit * 0.44] fill];
      }
    }
    const size_t eyes[3][2] = {{1, 1}, {n - 8, 1}, {1, n - 8}};
    for (const auto& eye : eyes) {
      CGRect ring = CGRectMake((CGFloat)eye[0] * unit + unit * 0.5, (CGFloat)eye[1] * unit + unit * 0.5,
                               unit * 6, unit * 6);
      UIBezierPath* path = [UIBezierPath bezierPathWithRoundedRect:ring cornerRadius:unit * 1.8];
      path.lineWidth = unit;
      [[UIColor blackColor] setStroke];
      [path stroke];
      CGRect core = CGRectMake((CGFloat)eye[0] * unit + unit * 2, (CGFloat)eye[1] * unit + unit * 2,
                               unit * 3, unit * 3);
      [[UIBezierPath bezierPathWithRoundedRect:core cornerRadius:unit] fill];
    }
    // Over exactly the modules left out above (mid - 2 to mid + 2).
    CGRect mark = CGRectMake(((CGFloat)mid - 2) * unit, ((CGFloat)mid - 2) * unit, unit * 5, unit * 5);
    [[UIColor whiteColor] setFill];
    [[UIBezierPath bezierPathWithRoundedRect:CGRectInset(mark, -unit * 0.15, -unit * 0.15)
                                cornerRadius:unit * 1.6] fill];
    [[XeniaTheme accent] setFill];
    [[UIBezierPath bezierPathWithRoundedRect:mark cornerRadius:unit * 1.3] fill];
  }];
}

NSString* CountdownText(NSDate* expires) {
  const NSInteger left = MAX((NSInteger)ceil(expires.timeIntervalSinceNow), 0);
  return [NSString stringWithFormat:@"%ld:%02ld", (long)(left / 60), (long)(left % 60)];
}

NSString* PlatformName(NSString* platform) {
  if ([platform isEqualToString:@"ipad"]) {
    return @"iPad";
  }
  if ([platform isEqualToString:@"mac"]) {
    return @"Mac";
  }
  return @"iPhone";
}

NSString* PlatformSymbol(NSString* platform) {
  return [platform isEqualToString:@"ipad"]  ? @"ipad"
         : [platform isEqualToString:@"mac"] ? @"laptopcomputer"
                                             : @"iphone";
}

}  // namespace

// ---- The QR code on a white tile, with text under it ----------------------------------------

@interface XEQRHero : UIView
@property(nonatomic, readonly) UIImageView* code;
@property(nonatomic, readonly) UILabel* title;
@property(nonatomic, readonly) UILabel* subtitle;
@property(nonatomic, readonly) UILabel* refresh;
@end

@implementation XEQRHero {
  UIImageView* _code;
  UILabel* _title;
  UILabel* _subtitle;
  UILabel* _refresh;
  CAGradientLayer* _glow;
  UIView* _tile;
}
- (UIImageView*)code { return _code; }
- (UILabel*)title { return _title; }
- (UILabel*)subtitle { return _subtitle; }
- (UILabel*)refresh { return _refresh; }

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _glow = [[CAGradientLayer alloc] init];
    _glow.type = kCAGradientLayerRadial;
    _glow.startPoint = CGPointMake(0.5, 0.5);
    _glow.endPoint = CGPointMake(1.0, 1.0);
    _glow.colors = @[ (id)[[XeniaTheme accent] colorWithAlphaComponent:0.36].CGColor,
                      (id)[[XeniaTheme accent] colorWithAlphaComponent:0.0].CGColor ];
    [self.layer addSublayer:_glow];
    _tile = [[UIView alloc] init];
    _tile.backgroundColor = [UIColor whiteColor];
    _tile.layer.cornerRadius = 26;
    _tile.layer.cornerCurve = kCACornerCurveContinuous;
    _tile.layer.borderWidth = 0.5;
    _tile.layer.borderColor = [UIColor colorWithWhite:0 alpha:0.08].CGColor;
    _tile.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_tile];
    _code = [[UIImageView alloc] init];
    _code.translatesAutoresizingMaskIntoConstraints = NO;
    _code.isAccessibilityElement = YES;
    _code.accessibilityLabel = NSLocalizedString(@"QR code", @"Link device: accessibility label");
    [_tile addSubview:_code];
    _title = [[UILabel alloc] init];
    _title.font = xe_scaled_system_font(UIFontTextStyleTitle3, 20.0, UIFontWeightSemibold);
    _title.adjustsFontForContentSizeCategory = YES;
    _title.textColor = [XeniaTheme textPrimary];
    _title.textAlignment = NSTextAlignmentCenter;
    _title.numberOfLines = 0;
    _subtitle = [[UILabel alloc] init];
    xe_apply_label_font(_subtitle, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular);
    _subtitle.textColor = [XeniaTheme textSecondary];
    _subtitle.textAlignment = NSTextAlignmentCenter;
    _subtitle.numberOfLines = 0;
    _refresh = [[UILabel alloc] init];
    _refresh.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightRegular];
    _refresh.textColor = [XeniaTheme textSecondary];
    _refresh.textAlignment = NSTextAlignmentCenter;
    UIStackView* text = [[[UIStackView alloc] initWithArrangedSubviews:@[ _title, _subtitle, _refresh ]] autorelease];
    text.axis = UILayoutConstraintAxisVertical;
    text.spacing = 6;
    [text setCustomSpacing:14 afterView:_subtitle];
    text.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:text];
    [NSLayoutConstraint activateConstraints:@[
      [_tile.topAnchor constraintEqualToAnchor:self.topAnchor constant:28],
      [_tile.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
      [_tile.widthAnchor constraintEqualToConstant:208],
      [_tile.heightAnchor constraintEqualToConstant:208],
      [_code.centerXAnchor constraintEqualToAnchor:_tile.centerXAnchor],
      [_code.centerYAnchor constraintEqualToAnchor:_tile.centerYAnchor],
      [_code.widthAnchor constraintEqualToConstant:176],
      [_code.heightAnchor constraintEqualToConstant:176],
      [text.topAnchor constraintEqualToAnchor:_tile.bottomAnchor constant:22],
      [text.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:28],
      [text.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-28],
      [text.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-8],
    ]];
  }
  return self;
}

- (void)dealloc {
  [_glow release];
  [_tile release];
  [_code release];
  [_title release];
  [_subtitle release];
  [_refresh release];
  [super dealloc];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat d = 280;
  _glow.frame = CGRectMake(self.bounds.size.width / 2 - d / 2, 28 + 104 - d / 2, d, d);
}
@end

// ---- The signed-in device: someone is signing in -----------------------------------------

namespace {

NSString* ApproveTitle() {
  LAContext* context = [[[LAContext alloc] init] autorelease];
  NSError* error = nil;
  [context canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&error];
  switch (context.biometryType) {
    case LABiometryTypeFaceID:
      return NSLocalizedString(@"Approve with Face ID", @"Approval: button");
    case LABiometryTypeTouchID:
      return NSLocalizedString(@"Approve with Touch ID", @"Approval: button");
    case LABiometryTypeOpticID:
      return NSLocalizedString(@"Approve with Optic ID", @"Approval: button");
    default:
      return NSLocalizedString(@"Approve with Passcode", @"Approval: button");
  }
}

}  // namespace

@interface XEIncomingForm : XEForm
- (instancetype)initWithApproval:(XeniaLiveIncomingApproval*)approval;
@end

@implementation XEIncomingForm {
  XeniaLiveIncomingApproval* _approval;
  NSString* _chosen;
  NSTimer* _timer;
  BOOL _busy;
  BOOL _answered;
}

- (instancetype)initWithApproval:(XeniaLiveIncomingApproval*)approval {
  if ((self = [super init])) {
    _approval = [approval retain];
    // A request that was scanned has no number to pick: the scan was the check.
    if (!approval.choices.count) {
      _chosen = [@"" copy];
    }
  }
  return self;
}

- (void)dealloc {
  [_timer invalidate];
  [_approval release];
  [_chosen release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = [NSString stringWithFormat:NSLocalizedString(@"Sign in on a new %@?",
                                                            @"Incoming approval title; %@ is iPhone, "
                                                            @"iPad or Mac"),
                                          PlatformName(_approval.platform)];
  [self setHeaderText:NSLocalizedString(@"Only approve if you’re signing in yourself.",
                                        @"Incoming approval: text above the device")];
  _timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                            target:self
                                          selector:@selector(tick)
                                          userInfo:nil
                                           repeats:YES];
  [self tick];
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  [_timer invalidate];
  _timer = nil;
}

- (void)tick {
  if (_approval.expires.timeIntervalSinceNow <= 0) {
    [_timer invalidate];
    _timer = nil;
    if (!_answered) {
      _answered = YES;
      [xe_ios_live_account_service() denyIncoming:_approval];
      [self dismissViewControllerAnimated:YES completion:nil];
    }
    return;
  }
  [self setFooterText:[NSString stringWithFormat:NSLocalizedString(@"Expires in %@",
                                                                   @"Incoming approval: countdown; %@ is m:ss"),
                                                 CountdownText(_approval.expires)]
                color:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 4;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 1 && !_approval.choices.count ? 0 : 1;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return section == 1 && _approval.choices.count
             ? [NSString
                   stringWithFormat:NSLocalizedString(@"Pick the number on %@",
                                                      @"Incoming approval: header over the "
                                                      @"numbers; %@ is the new device’s name"),
                                    _approval.deviceName]
             : nil;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  switch (indexPath.section) {
    case 0: {
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
      UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
      content.text = _approval.deviceName;
      content.secondaryText = _approval.platformText;
      content.image = Glyph(PlatformSymbol(_approval.platform));
      cell.contentConfiguration = content;
      break;
    }
    case 1: {
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
      NSMutableArray* buttons = [NSMutableArray array];
      for (NSString* choice in _approval.choices) {
        UIButton* button = [UIButton buttonWithType:UIButtonTypeSystem];
        UIButtonConfiguration* config = [UIButtonConfiguration plainButtonConfiguration];
        config.title = choice;
        config.baseForegroundColor = [XeniaTheme textPrimary];
        config.background.cornerRadius = 14;
        const BOOL picked = [choice isEqualToString:_chosen ?: @""];
        config.background.backgroundColor = picked ? [[XeniaTheme accent] colorWithAlphaComponent:0.22]
                                                   : [UIColor tertiarySystemFillColor];
        config.background.strokeColor = picked ? [XeniaTheme accent] : [UIColor clearColor];
        config.background.strokeWidth = 2;
        config.titleTextAttributesTransformer =
            ^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* attributes) {
              NSMutableDictionary* out = [[attributes mutableCopy] autorelease];
              out[NSFontAttributeName] = [UIFont monospacedDigitSystemFontOfSize:26 weight:UIFontWeightSemibold];
              return out;
            };
        button.configuration = config;
        button.accessibilityLabel = choice;
        button.accessibilityTraits |= picked ? UIAccessibilityTraitSelected : 0;
        [button.heightAnchor constraintEqualToConstant:60].active = YES;
        [button addTarget:self action:@selector(picked:) forControlEvents:UIControlEventTouchUpInside];
        [buttons addObject:button];
      }
      UIStackView* row = [[[UIStackView alloc] initWithArrangedSubviews:buttons] autorelease];
      row.distribution = UIStackViewDistributionFillEqually;
      row.spacing = 10;
      row.translatesAutoresizingMaskIntoConstraints = NO;
      [cell.contentView addSubview:row];
      [NSLayoutConstraint activateConstraints:@[
        [row.leadingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.leadingAnchor],
        [row.trailingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.trailingAnchor],
        [row.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:12],
        [row.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-12],
      ]];
      break;
    }
    case 2: {
      UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
      content.text = _busy ? NSLocalizedString(@"Approving…", @"Incoming approval: while waiting")
                           : ApproveTitle();
      content.textProperties.color = _chosen ? [XeniaTheme accent] : [XeniaTheme textMuted];
      content.textProperties.alignment = UIListContentTextAlignmentCenter;
      content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
      cell.contentConfiguration = content;
      break;
    }
    default: {
      UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
      content.text = NSLocalizedString(@"Deny", @"Incoming approval: button");
      content.textProperties.color = [UIColor systemRedColor];
      content.textProperties.alignment = UIListContentTextAlignmentCenter;
      cell.contentConfiguration = content;
      break;
    }
  }
  return cell;
}

- (void)picked:(UIButton*)button {
  if (_busy) {
    return;
  }
  [_chosen release];
  _chosen = [button.accessibilityLabel copy];
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(1, 2)]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (_busy) {
    return;
  }
  if (indexPath.section == 3) {
    _answered = YES;
    [xe_ios_live_account_service() denyIncoming:_approval];
    [self dismissViewControllerAnimated:YES completion:nil];
  } else if (indexPath.section == 2 && _chosen) {
    [self authenticateAndApprove];
  }
}

- (void)authenticateAndApprove {
  _busy = YES;
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:2] withRowAnimation:UITableViewRowAnimationNone];
  LAContext* context = [[[LAContext alloc] init] autorelease];
  NSError* error = nil;
  // Face ID or Touch ID, then the device passcode (the system password on a Mac).
  const BOOL can = [context canEvaluatePolicy:LAPolicyDeviceOwnerAuthentication error:&error];
  __unsafe_unretained XEIncomingForm* unsafe_self = self;
  [self retain];
  void (^go)(void) = ^{
    [xe_ios_live_account_service()
        approveIncoming:unsafe_self->_approval
                 chosen:unsafe_self->_chosen
             completion:^(XeniaLiveApprovalStatus status) {
               [unsafe_self answered:status];
               [unsafe_self release];
             }];
  };
  if (!can) {
    [self releaseAfterFailure:NSLocalizedString(@"Set a passcode on this device to approve sign-ins.",
                                                @"Incoming approval: no passcode")];
    return;
  }
  [context evaluatePolicy:LAPolicyDeviceOwnerAuthentication
          localizedReason:NSLocalizedString(@"Approve signing in to your XeniOS account on another device.",
                                            @"Face ID prompt reason")
                    reply:^(BOOL success, NSError* __unused e) {
                      dispatch_async(dispatch_get_main_queue(), ^{
                        if (success) {
                          go();
                        } else {
                          [unsafe_self releaseAfterFailure:nil];
                        }
                      });
                    }];
}

// Authentication was cancelled or failed: back to picking, nothing was sent.
- (void)releaseAfterFailure:(NSString*)message {
  _busy = NO;
  [self setFooterText:message color:[UIColor systemRedColor]];
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:2] withRowAnimation:UITableViewRowAnimationNone];
  [self release];
}

- (void)answered:(XeniaLiveApprovalStatus)status {
  _busy = NO;
  _answered = YES;
  if (status == XeniaLiveApprovalStatusApproved) {
    [self dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  __unsafe_unretained XEIncomingForm* unsafe_self = self;
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             (void)unsafe_self;
                           }];
}

@end

// ---- Shared bits for the v3 pages ------------------------------------------------------------

namespace {

NSString* const kRecoverySavedKey = @"XeniaLiveRecoverySaved";

BOOL RecoverySaved() { return [[NSUserDefaults standardUserDefaults] boolForKey:kRecoverySavedKey]; }

void SetRecoverySaved(BOOL saved) {
  [[NSUserDefaults standardUserDefaults] setBool:saved forKey:kRecoverySavedKey];
}

// "xenios://link?c=K7Q2M9XD": what Add a Device's QR code holds. The system Camera opens it in
// XeniOS on the new device; the sign-in sheet's own camera reads it too.
NSString* LinkURL(NSString* code) {
  NSString* squeezed = [code stringByReplacingOccurrencesOfString:@"-" withString:@""];
  return [@"xenios://link?c=" stringByAppendingString:squeezed];
}

NSString* CodeFromLinkURL(NSString* text) {
  NSURLComponents* components = [NSURLComponents componentsWithString:text];
  if (![components.scheme.lowercaseString isEqualToString:@"xenios"] ||
      ![components.host.lowercaseString isEqualToString:@"link"]) {
    return nil;
  }
  for (NSURLQueryItem* item in components.queryItems) {
    if ([item.name isEqualToString:@"c"] && item.value.length) {
      return item.value;
    }
  }
  return nil;
}

// A view sized by its constraints, set as |table|'s header at the table's width.
void FitTableHeader(UIView* header, UITableView* table) {
  const CGFloat width = CGRectGetWidth(table.bounds);
  if (width <= 0) {
    return;
  }
  const CGFloat height =
      [header systemLayoutSizeFittingSize:CGSizeMake(width, UILayoutFittingCompressedSize.height)
            withHorizontalFittingPriority:UILayoutPriorityRequired
                  verticalFittingPriority:UILayoutPriorityFittingSizeLevel]
          .height;
  if (table.tableHeaderView != header || std::fabs(CGRectGetHeight(header.frame) - height) >= 0.5 ||
      std::fabs(CGRectGetWidth(header.frame) - width) >= 0.5) {
    header.frame = CGRectMake(0, 0, width, height);
    table.tableHeaderView = header;
  }
}

// The page's one white button, as in the Games and TV apps.
UIButton* PrimaryButton(NSString* title, id target, SEL action) {
  UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
  config.title = title;
  config.baseBackgroundColor = [XeniaTheme textPrimary];
  config.baseForegroundColor = [XeniaTheme bgPrimary];
  config.buttonSize = UIButtonConfigurationSizeLarge;
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.titleTextAttributesTransformer =
      ^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
        NSMutableDictionary* out = [[in mutableCopy] autorelease];
        out[NSFontAttributeName] = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        return out;
      };
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
  return button;
}

// A dot that says "this wants doing", before the row's chevron.
UIView* AttentionAccessory() {
  UIView* dot = [[[UIView alloc] init] autorelease];
  dot.backgroundColor = [UIColor systemOrangeColor];
  dot.layer.cornerRadius = 4.5;
  dot.translatesAutoresizingMaskIntoConstraints = NO;
  UIImageView* chevron = [[[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"chevron.forward"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithTextStyle:UIFontTextStyleFootnote
                                                                       scale:UIImageSymbolScaleMedium]]]
      autorelease];
  chevron.tintColor = [XeniaTheme textMuted];
  UIStackView* stack = [[[UIStackView alloc] initWithArrangedSubviews:@[ dot, chevron ]] autorelease];
  stack.alignment = UIStackViewAlignmentCenter;
  stack.spacing = 10;
  [NSLayoutConstraint activateConstraints:@[
    [dot.widthAnchor constraintEqualToConstant:9], [dot.heightAnchor constraintEqualToConstant:9]
  ]];
  stack.frame = CGRectMake(0, 0, 32, 20);
  return stack;
}

}  // namespace

// ---- The profile page's top: avatar, name, a status line, and its one action -----------------

@interface XEProfileHeader : UIView
@property(nonatomic, readonly) UIImageView* avatar;
@property(nonatomic, readonly) UILabel* name;
@property(nonatomic, readonly) UILabel* status;
@property(nonatomic, readonly) UILabel* caption;
// "Edit" under the avatar, hidden until given a target.
@property(nonatomic, readonly) UIButton* editPicture;
- (void)setButton:(nullable UIButton*)button;
@end

@implementation XEProfileHeader {
  UIImageView* _avatar;
  UILabel* _name;
  UILabel* _status;
  UILabel* _caption;
  UIButton* _editPicture;
  UIStackView* _stack;
  UIButton* _button;
}
- (UIImageView*)avatar { return _avatar; }
- (UIButton*)editPicture { return _editPicture; }
- (UILabel*)name { return _name; }
- (UILabel*)status { return _status; }
- (UILabel*)caption { return _caption; }

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _avatar = [[UIImageView alloc] init];
    _avatar.contentMode = UIViewContentModeScaleAspectFit;
    _avatar.isAccessibilityElement = NO;
    _name = [[UILabel alloc] init];
    _name.font = xe_scaled_system_font(UIFontTextStyleTitle2, 24.0, UIFontWeightBold);
    _name.adjustsFontForContentSizeCategory = YES;
    _name.textColor = [XeniaTheme textPrimary];
    _name.textAlignment = NSTextAlignmentCenter;
    _name.numberOfLines = 0;
    _name.accessibilityTraits = UIAccessibilityTraitHeader;
    _status = [[UILabel alloc] init];
    xe_apply_label_font(_status, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
    _status.textColor = [XeniaTheme textSecondary];
    _status.textAlignment = NSTextAlignmentCenter;
    _status.numberOfLines = 0;
    _caption = [[UILabel alloc] init];
    xe_apply_label_font(_caption, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular);
    _caption.textColor = [XeniaTheme textSecondary];
    _caption.textAlignment = NSTextAlignmentCenter;
    _caption.numberOfLines = 0;
    // As Contacts: a small "Edit" under the photo.
    UIButtonConfiguration* edit = [UIButtonConfiguration plainButtonConfiguration];
    edit.title = NSLocalizedString(@"Edit", @"Profile page: button under the gamer picture");
    edit.buttonSize = UIButtonConfigurationSizeSmall;
    edit.contentInsets = NSDirectionalEdgeInsetsMake(2, 8, 2, 8);
    _editPicture = [[UIButton buttonWithConfiguration:edit primaryAction:nil] retain];
    _editPicture.accessibilityLabel =
        NSLocalizedString(@"Edit Gamer Picture", @"Profile page: VoiceOver label");
    _editPicture.hidden = YES;
    _stack = [[UIStackView alloc]
        initWithArrangedSubviews:@[ _avatar, _editPicture, _name, _status, _caption ]];
    _stack.axis = UILayoutConstraintAxisVertical;
    _stack.alignment = UIStackViewAlignmentFill;
    _stack.spacing = 4;
    [_stack setCustomSpacing:10 afterView:_avatar];
    [_stack setCustomSpacing:2 afterView:_editPicture];
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_stack];
    [NSLayoutConstraint activateConstraints:@[
      [_avatar.heightAnchor constraintEqualToConstant:72],
      [_stack.topAnchor constraintEqualToAnchor:self.topAnchor constant:4],
      [_stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:24],
      [_stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-24],
      [_stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-14],
    ]];
  }
  return self;
}

- (void)dealloc {
  [_avatar release];
  [_name release];
  [_status release];
  [_caption release];
  [_editPicture release];
  [_stack release];
  [_button release];
  [super dealloc];
}

- (void)setButton:(UIButton*)button {
  if (_button == button) {
    return;
  }
  [_button removeFromSuperview];
  [_button release];
  _button = [button retain];
  if (button) {
    // Under the status line, above its caption.
    [_stack insertArrangedSubview:button
                          atIndex:[_stack.arrangedSubviews indexOfObject:_status] + 1];
    [_stack setCustomSpacing:18 afterView:_status];
    [_stack setCustomSpacing:10 afterView:button];
  } else {
    [_stack setCustomSpacing:4 afterView:_status];
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  _caption.hidden = _caption.text.length == 0;
}
@end

// ---- Copy Profile: what, then where ----------------------------------------------------------

@interface XECopyProfileForm : XEForm
@end

@implementation XECopyProfileForm {
  XeniaLiveExportOptions* _options;
  NSArray<NSString*>* _sizes;
  NSArray<XeniaLiveLocalProfile*>* _others;
  BOOL _busy;
}

- (void)dealloc {
  [_options release];
  [_sizes release];
  [_others release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Copy Profile", @"Copy profile sheet title");
  [self addCloseButton];
  _options = [[XeniaLiveExportOptions alloc] init];
  _options.saves = _options.achievements = _options.settings = YES;
  // Graphics and patches are tuned to each device: off unless asked for.
  _options.gameSettings = NO;
  _sizes = [@[ @"", @"", @"" ] retain];
  __unsafe_unretained XECopyProfileForm* unsafe_self = self;
  [xe_ios_live_account_service()
      exportSizesWithCompletion:^(uint64_t saves, uint64_t achievements, uint64_t settings) {
        [unsafe_self->_sizes release];
        unsafe_self->_sizes = [@[ SizeText(saves), SizeText(achievements), SizeText(settings) ] retain];
        [unsafe_self.tableView reloadData];
      }];
  [xe_ios_live_account_service()
      localProfilesWithCompletion:^(NSArray<XeniaLiveLocalProfile*>* profiles) {
        NSMutableArray* others = [NSMutableArray array];
        for (XeniaLiveLocalProfile* profile in profiles) {
          if (!profile.thisProfile) {
            [others addObject:profile];
          }
        }
        [unsafe_self->_others release];
        unsafe_self->_others = [others copy];
        [unsafe_self.tableView reloadData];
      }];
}

- (BOOL)anything {
  return _options.saves || _options.achievements || _options.settings || _options.gameSettings;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? 4 : 2;
}

// Two plain section headers, What and To, lined up with each other.
- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return section == 1 ? NSLocalizedString(@"To", @"Copy profile sheet: header")
                      : NSLocalizedString(@"What", @"Copy profile sheet: header");
}

- (UIView*)tableView:(UITableView* __unused)tableView viewForHeaderInSection:(NSInteger __unused)section {
  return nil;
}

- (CGFloat)tableView:(UITableView* __unused)tableView heightForHeaderInSection:(NSInteger __unused)section {
  return UITableViewAutomaticDimension;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (section != 1) {
    return nil;
  }
  return _others.count ? NSLocalizedString(@"Another profile here gets the game saves. If it has a "
                                           @"newer save for a game, that one is kept.",
                                           @"Copy profile sheet: footer")
                       : nil;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 1) {
    const BOOL to_profile = indexPath.row == 1;
    UITableViewCell* cell =
        ActionCell(to_profile ? NSLocalizedString(@"Another Profile on This Device…",
                                                  @"Copy profile sheet: row")
                              : NSLocalizedString(@"AirDrop or Files…", @"Copy profile sheet: row"));
    const BOOL enabled =
        !_busy && (to_profile ? (_options.saves && _others.count) : [self anything]);
    cell.userInteractionEnabled = enabled;
    cell.contentView.alpha = enabled ? 1.0 : 0.4;
    return cell;
  }
  NSArray* rows = @[
    @[ NSLocalizedString(@"Game Saves", @"Copy profile sheet: switch"), @"externaldrive" ],
    @[ NSLocalizedString(@"Achievements", @"Copy profile sheet: switch"), @"trophy" ],
    @[ NSLocalizedString(@"Controls and Profile Settings", @"Copy profile sheet: switch"),
       @"gamecontroller" ],
    @[ NSLocalizedString(@"Game Settings", @"Copy profile sheet: switch"), @"slider.horizontal.3" ],
  ];
  const NSUInteger index = (NSUInteger)indexPath.row;
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = rows[index][0];
  if (index < 3) {
    content.secondaryText = _sizes[index].length ? _sizes[index] : nil;
  } else {
    content.secondaryText =
        NSLocalizedString(@"Tuned for each device", @"Copy profile sheet: switch detail");
  }
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.image = Glyph(rows[index][1]);
  cell.contentConfiguration = content;
  UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
  toggle.on = index == 0   ? _options.saves
              : index == 1 ? _options.achievements
              : index == 2 ? _options.settings
                           : _options.gameSettings;
  toggle.onTintColor = [XeniaTheme accent];
  toggle.tag = (NSInteger)index;
  toggle.accessibilityLabel = rows[index][0];
  [toggle addTarget:self action:@selector(toggled:) forControlEvents:UIControlEventValueChanged];
  cell.accessoryView = toggle;
  return cell;
}

- (void)toggled:(UISwitch*)toggle {
  switch (toggle.tag) {
    case 0:
      _options.saves = toggle.on;
      break;
    case 1:
      _options.achievements = toggle.on;
      break;
    case 2:
      _options.settings = toggle.on;
      break;
    default:
      _options.gameSettings = toggle.on;
      break;
  }
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:1]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section != 1 || _busy) {
    return;
  }
  if (indexPath.row == 1) {
    [self chooseProfileFrom:[tableView cellForRowAtIndexPath:indexPath]];
  } else {
    [self exportFrom:indexPath];
  }
}

- (void)exportFrom:(NSIndexPath*)row {
  _busy = YES;
  // Kept until the export ends, which can take a while: the sheet may be closed meanwhile.
  __unsafe_unretained XECopyProfileForm* unsafe_self = self;
  [self retain];
  [xe_ios_live_account_service()
      exportProfileWithOptions:_options
                    completion:^(NSURL* file, NSError* error) {
                      [unsafe_self autorelease];
                      unsafe_self->_busy = NO;
                      if (!unsafe_self.view.window) {
                        if (file) {
                          [[NSFileManager defaultManager] removeItemAtURL:file error:nil];
                        }
                        return;
                      }
                      [unsafe_self.tableView reloadData];
                      if (!file) {
                        ShowAlert(unsafe_self,
                                  NSLocalizedString(@"Couldn’t Copy", @"Copy profile sheet: alert title"),
                                  error.localizedDescription);
                        return;
                      }
                      UIActivityViewController* share = [[[UIActivityViewController alloc]
                          initWithActivityItems:@[ file ]
                          applicationActivities:nil] autorelease];
                      // The table, not the cell: the reload above recycles the cell, and a
                      // popover anchored to it lands in the iPad window's corner.
                      share.popoverPresentationController.sourceView = unsafe_self.tableView;
                      share.popoverPresentationController.sourceRect =
                          [unsafe_self.tableView rectForRowAtIndexPath:row];
                      share.completionWithItemsHandler =
                          ^(UIActivityType __unused type, BOOL __unused completed,
                            NSArray* __unused items, NSError* __unused e) {
                            [[NSFileManager defaultManager] removeItemAtURL:file error:nil];
                          };
                      [unsafe_self presentViewController:share animated:YES completion:nil];
                    }];
  [self.tableView reloadData];
}

- (void)chooseProfileFrom:(UIView*)source {
  UIAlertController* sheet = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Copy Game Saves To", @"Copy profile sheet: menu title")
                       message:nil
                preferredStyle:UIAlertControllerStyleActionSheet];
  __unsafe_unretained XECopyProfileForm* unsafe_self = self;
  for (XeniaLiveLocalProfile* profile in _others) {
    [sheet addAction:[UIAlertAction actionWithTitle:profile.gamertag
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction* __unused action) {
                                              [unsafe_self copyToProfile:profile];
                                            }]];
  }
  [sheet addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Copy profile sheet: menu")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  sheet.popoverPresentationController.sourceView = source;
  sheet.popoverPresentationController.sourceRect = source.bounds;
  [self presentViewController:sheet animated:YES completion:nil];
}

- (void)copyToProfile:(XeniaLiveLocalProfile*)profile {
  _busy = YES;
  [self.tableView reloadData];
  __unsafe_unretained XECopyProfileForm* unsafe_self = self;
  [self retain];  // Until the copy ends: the sheet may be closed meanwhile.
  NSString* name = [[profile.gamertag copy] autorelease];
  [xe_ios_live_account_service()
      copySavesToProfile:profile.profileId
              completion:^(NSUInteger copied, NSUInteger kept, NSError* error) {
                [unsafe_self autorelease];
                unsafe_self->_busy = NO;
                if (!unsafe_self.view.window) {
                  return;
                }
                [unsafe_self.tableView reloadData];
                if (error) {
                  ShowAlert(unsafe_self, NSLocalizedString(@"Couldn’t Copy", @"Copy profile sheet: alert title"),
                            error.localizedDescription);
                  return;
                }
                NSString* message =
                    kept ? [NSString stringWithFormat:NSLocalizedString(
                                                          @"%1$lu saves copied to %2$@. %3$lu newer saves "
                                                          @"there were kept.",
                                                          @"Copy profile sheet: result; count, gamertag, count"),
                                                      (unsigned long)copied, name, (unsigned long)kept]
                         : [NSString stringWithFormat:NSLocalizedString(
                                                          @"%1$lu saves copied to %2$@.",
                                                          @"Copy profile sheet: result; count, gamertag"),
                                                      (unsigned long)copied, name];
                ShowAlert(unsafe_self, NSLocalizedString(@"Saves Copied", @"Copy profile sheet: alert title"),
                          message);
              }];
}

@end

// ---- Add a Device: a QR code and the same code to type, on the signed-in device ---------------

@interface XEAddDeviceForm : XEForm
@end

@implementation XEAddDeviceForm {
  XEQRHero* _hero;
  NSString* _code;
  NSDate* _expires;
  NSTimer* _timer;
  BOOL _expired;
  BOOL _failed;
}

- (void)dealloc {
  [_timer invalidate];
  [_hero release];
  [_code release];
  [_expires release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Add a Device", @"Add a device sheet title");
  [self addCloseButton];
  _hero = [[XEQRHero alloc] init];
  _hero.title.font = [UIFont monospacedSystemFontOfSize:30 weight:UIFontWeightBold];
  _hero.subtitle.text = NSLocalizedString(@"Scan it with the other device’s camera, or type the code "
                                          @"there in I Have a Gamertag.",
                                          @"Add a device sheet: text under the code");
  [self load];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  FitTableHeader(_hero, self.tableView);
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  if (self.isBeingDismissed || self.navigationController.isBeingDismissed) {
    [_timer invalidate];
    _timer = nil;
  }
}

- (void)load {
  _expired = NO;
  _failed = NO;
  [_code release];
  _code = nil;
  _hero.code.image = nil;
  _hero.title.text = @"····-····";
  _hero.title.textColor = [XeniaTheme textMuted];
  _hero.refresh.text = NSLocalizedString(@"Getting a code…", @"Add a device sheet: while waiting");
  [self.tableView reloadData];
  __unsafe_unretained XEAddDeviceForm* unsafe_self = self;
  [xe_ios_live_account_service()
      createLinkCodeWithCompletion:^(NSString* code, NSDate* expires, NSError* __unused error) {
        if (!code.length) {
          unsafe_self->_failed = YES;
          unsafe_self->_hero.refresh.text =
              NSLocalizedString(@"Can’t reach XeniOS. Try again.", @"Add a device sheet: error");
          [unsafe_self.tableView reloadData];
          return;
        }
        unsafe_self->_code = [FormattedCode(code, 8) copy];
        [unsafe_self->_expires release];
        unsafe_self->_expires = [expires retain];
        unsafe_self->_hero.code.image = QRImage(LinkURL(code), 176);
        unsafe_self->_hero.title.text = unsafe_self->_code;
        unsafe_self->_hero.title.textColor = [XeniaTheme textPrimary];
        unsafe_self->_hero.title.accessibilityLabel =
            [[unsafe_self->_code componentsSeparatedByString:@"-"] componentsJoinedByString:@" "];
        [unsafe_self->_timer invalidate];
        unsafe_self->_timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                               target:unsafe_self
                                                             selector:@selector(tick)
                                                             userInfo:nil
                                                              repeats:YES];
        [unsafe_self tick];
        [unsafe_self.tableView reloadData];
      }];
}

- (void)tick {
  const NSInteger left = (NSInteger)ceil(_expires.timeIntervalSinceNow);
  if (left <= 0) {
    [_timer invalidate];
    _timer = nil;
    _expired = YES;
    _hero.code.image = nil;
    _hero.title.textColor = [XeniaTheme textMuted];
    _hero.refresh.text = NSLocalizedString(@"This code has expired.", @"Add a device sheet: expired");
    [self.tableView reloadData];
    return;
  }
  _hero.refresh.text = [NSString
      stringWithFormat:NSLocalizedString(@"Expires in %1$ld:%2$02ld",
                                         @"Add a device sheet: countdown; minutes, seconds"),
                       (long)(left / 60), (long)(left % 60)];
}

- (BOOL)needsNewCode {
  return _expired || _failed;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  // Get a New Code when the code is gone; Also Copy Profile where profile data can move.
  return ([self needsNewCode] ? 1 : 0) + (xe_ios_live_account_supports_profile_data() ? 1 : 0);
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger __unused)section {
  return nil;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if ([self needsNewCode] && indexPath.row == 0) {
    return ActionCell(NSLocalizedString(@"Get a New Code", @"Add a device sheet: row"));
  }
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = NSLocalizedString(@"Also Copy Profile…", @"Add a device sheet: row");
  content.secondaryText = NSLocalizedString(@"Saves, achievements and settings",
                                            @"Add a device sheet: row detail");
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.image = Glyph(@"square.and.arrow.up");
  cell.contentConfiguration = content;
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if ([self needsNewCode] && indexPath.row == 0) {
    [self load];
    return;
  }
  [self.navigationController pushViewController:[[[XECopyProfileForm alloc] init] autorelease]
                                       animated:YES];
}

@end

// ---- A camera that reads a XeniOS QR code ----------------------------------------------------

@interface XECodeScannerView : UIView <AVCaptureMetadataOutputObjectsDelegate>
@property(nonatomic, copy, nullable) void (^onCode)(NSString* text);
// What the frame says while it looks, and with no camera; and what Use Test Code reads, with no
// camera.
@property(nonatomic, copy) NSString* hint;
@property(nonatomic, copy) NSString* noCameraHint;
@property(nonatomic, copy) NSString* testText;
// The camera frame's height: 220 under a form's field; taller on a page that is only the camera.
@property(nonatomic) CGFloat frameHeight;
- (void)resume;
@end

@implementation XECodeScannerView {
  AVCaptureSession* _session;
  AVCaptureVideoPreviewLayer* _preview;
  UILabel* _hint;
  UIView* _frame;
  BOOL _started;
  BOOL _paused;
  NSString* _noCameraHint;
  NSString* _testText;
  NSLayoutConstraint* _frameHeight;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _frame = [[UIView alloc] init];
    _frameHeight = [[_frame.heightAnchor constraintEqualToConstant:220] retain];
    _frame.backgroundColor = [UIColor blackColor];
    _frame.layer.cornerRadius = 20;
    _frame.layer.cornerCurve = kCACornerCurveContinuous;
    _frame.layer.masksToBounds = YES;
    _frame.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_frame];
    _hint = [[UILabel alloc] init];
    xe_apply_label_font(_hint, UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
    _hint.textColor = [UIColor whiteColor];
    _hint.textAlignment = NSTextAlignmentCenter;
    _hint.numberOfLines = 0;
    _hint.text = NSLocalizedString(@"Point at the code on your other device",
                                   @"Sign in sheet: camera hint");
    _hint.translatesAutoresizingMaskIntoConstraints = NO;
    [_frame addSubview:_hint];
    [NSLayoutConstraint activateConstraints:@[
      [_frame.topAnchor constraintEqualToAnchor:self.topAnchor constant:8],
      [_frame.leadingAnchor constraintEqualToAnchor:self.layoutMarginsGuide.leadingAnchor],
      [_frame.trailingAnchor constraintEqualToAnchor:self.layoutMarginsGuide.trailingAnchor],
      [_frame.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-4],
      _frameHeight,
      [_hint.leadingAnchor constraintEqualToAnchor:_frame.leadingAnchor constant:16],
      [_hint.trailingAnchor constraintEqualToAnchor:_frame.trailingAnchor constant:-16],
      [_hint.bottomAnchor constraintEqualToAnchor:_frame.bottomAnchor constant:-14],
    ]];
  }
  return self;
}

- (void)dealloc {
  [_session stopRunning];
  [_session release];
  [_preview release];
  [_hint release];
  [_frame release];
  [_onCode release];
  [_noCameraHint release];
  [_testText release];
  [_frameHeight release];
  [super dealloc];
}

- (CGFloat)frameHeight {
  return _frameHeight.constant;
}

- (void)setFrameHeight:(CGFloat)height {
  _frameHeight.constant = height;
}

- (NSString*)hint {
  return _hint.text;
}

- (void)setHint:(NSString*)hint {
  _hint.text = hint;
}

- (NSString*)noCameraHint {
  return _noCameraHint ?: NSLocalizedString(@"This device has no camera. Type the code below.",
                                            @"Sign in sheet: no camera");
}

- (void)setNoCameraHint:(NSString*)text {
  [_noCameraHint autorelease];
  _noCameraHint = [text copy];
}

- (NSString*)testText {
  return _testText;
}

- (void)setTestText:(NSString*)text {
  [_testText autorelease];
  _testText = [text copy];
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  if (self.window && !_started) {
    _started = YES;
    [self startCamera];
  } else if (!self.window) {
    [_session stopRunning];
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  _preview.frame = _frame.bounds;
}

- (void)resume {
  _paused = NO;
  AVCaptureSession* session = _session;
  if (session && !session.running) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      [session startRunning];
    });
  }
}

- (void)startCamera {
  AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
  if (!device) {
    [self noCamera];
    return;
  }
  const AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
  if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
    _hint.text = NSLocalizedString(@"Allow camera access for XeniOS in Settings to scan, or type the "
                                   @"code below.",
                                   @"Sign in sheet: camera denied");
    return;
  }
  [self retain];
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                           completionHandler:^(BOOL granted) {
                             dispatch_async(dispatch_get_main_queue(), ^{
                               if (granted) {
                                 [self configureSession:device];
                               } else {
                                 self->_hint.text = NSLocalizedString(
                                     @"Allow camera access for XeniOS in Settings to scan, or type "
                                     @"the code below.",
                                     @"Sign in sheet: camera denied");
                               }
                               [self release];
                             });
                           }];
}

- (void)configureSession:(AVCaptureDevice*)device {
  AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:nil];
  if (!input) {
    [self noCamera];
    return;
  }
  _session = [[AVCaptureSession alloc] init];
  [_session addInput:input];
  AVCaptureMetadataOutput* output = [[[AVCaptureMetadataOutput alloc] init] autorelease];
  [_session addOutput:output];
  [output setMetadataObjectsDelegate:self queue:dispatch_get_main_queue()];
  output.metadataObjectTypes = @[ AVMetadataObjectTypeQRCode ];
  _preview = [[AVCaptureVideoPreviewLayer alloc] initWithSession:_session];
  _preview.videoGravity = AVLayerVideoGravityResizeAspectFill;
  _preview.frame = _frame.bounds;
  [_frame.layer insertSublayer:_preview atIndex:0];
  [self resume];
}

// No camera (the Simulator): a page given a test code still takes it.
- (void)noCamera {
  _hint.text = self.noCameraHint;
  if (!_testText) {
    return;
  }
  UIButtonConfiguration* config = [UIButtonConfiguration grayButtonConfiguration];
  config.title = @"Use Test Code";
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  UIButton* test = [UIButton buttonWithConfiguration:config primaryAction:nil];
  test.translatesAutoresizingMaskIntoConstraints = NO;
  [test addTarget:self action:@selector(useTestCode) forControlEvents:UIControlEventTouchUpInside];
  [_frame addSubview:test];
  [NSLayoutConstraint activateConstraints:@[
    [test.centerXAnchor constraintEqualToAnchor:_frame.centerXAnchor],
    [test.centerYAnchor constraintEqualToAnchor:_frame.centerYAnchor constant:-12],
  ]];
}

- (void)useTestCode {
  if (_onCode) {
    _onCode(self.testText);
  }
}

- (void)captureOutput:(AVCaptureOutput* __unused)output
    didOutputMetadataObjects:(NSArray<__kindof AVMetadataObject*>*)objects
              fromConnection:(AVCaptureConnection* __unused)connection {
  if (_paused) {
    return;
  }
  for (AVMetadataObject* object in objects) {
    if (![object isKindOfClass:[AVMetadataMachineReadableCodeObject class]]) {
      continue;
    }
    NSString* value = ((AVMetadataMachineReadableCodeObject*)object).stringValue;
    if (value.length && _onCode) {
      _paused = YES;
      [_session stopRunning];
      _onCode(value);
      return;
    }
  }
}

@end

// ---- A camera page on its own: Add Friend's scan button --------------------------------------

@interface XEScanForm : XEForm
- (instancetype)initWithTitle:(NSString*)title
                         hint:(NSString*)hint
                     testText:(nullable NSString*)test_text
                       onText:(BOOL (^)(NSString* text))on_text;
@end

@implementation XEScanForm {
  XECodeScannerView* _scanner;
  NSString* _hint;
  BOOL (^_onText)(NSString*);
}

- (instancetype)initWithTitle:(NSString*)title
                         hint:(NSString*)hint
                     testText:(NSString*)test_text
                       onText:(BOOL (^)(NSString*))on_text {
  if ((self = [super init])) {
    self.title = title;
    _hint = [hint copy];
    _onText = [on_text copy];
    _scanner = [[XECodeScannerView alloc] init];
    _scanner.hint = hint;
    _scanner.frameHeight = 360;
    _scanner.noCameraHint = NSLocalizedString(@"This device has no camera. Type the code instead.",
                                              @"Scan page: no camera");
    if (test_text) {
      _scanner.testText = test_text;
    }
  }
  return self;
}

- (void)dealloc {
  _scanner.onCode = nil;
  [_scanner release];
  [_hint release];
  [_onText release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  [self addCloseButton];
  __unsafe_unretained XEScanForm* unsafe_self = self;
  _scanner.onCode = ^(NSString* text) {
    [unsafe_self scanned:text];
  };
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  FitTableHeader(_scanner, self.tableView);
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 0;
}

- (void)scanned:(NSString*)text {
  if (_onText && _onText(text)) {
    [self dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  // Not one of ours: say so in the frame, then look again.
  _scanner.hint = NSLocalizedString(@"That isn’t a XeniOS code.", @"Scan page: wrong QR");
  __unsafe_unretained XEScanForm* unsafe_self = self;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   unsafe_self->_scanner.hint = unsafe_self->_hint;
                   [unsafe_self->_scanner resume];
                 });
}

@end

// ---- I Have a Gamertag: the camera, the code typed, a recovery code, or asking the other device

typedef NS_ENUM(NSInteger, XESignInMode) {
  XESignInModeCode = 0,  // Add a Device's QR code or its code, typed.
  XESignInModeAsk,       // The gamertag; a signed-in device approves.
  XESignInModeRecovery,  // The recovery code kept.
};

@interface XESignInForm : XEForm <UITextFieldDelegate>
- (instancetype)initWithCode:(nullable NSString*)code;
@end

@implementation XESignInForm {
  NSString* _initialCode;
  XESignInMode _mode;
  XECodeScannerView* _scanner;
  UITextField* _field;
  UIButton* _doneItem;
  BOOL _busy;
  // While asking the other device: the request it answers.
  XeniaLiveApprovalRequest* _request;
}

- (instancetype)initWithCode:(NSString*)code {
  if ((self = [super init])) {
    _initialCode = [code copy];
  }
  return self;
}

- (void)dealloc {
  _scanner.onCode = nil;
  [_request release];
  [_scanner release];
  [_initialCode release];
  [_field release];
  [_doneItem release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Sign In", @"Sign in sheet title");
  [self addCloseButton];
  _doneItem = [[self addDoneButtonWithTitle:NSLocalizedString(@"Sign In", @"Sign in sheet: button")]
      retain];
  _field = [[UITextField alloc] init];
  _field.delegate = self;
  _field.textAlignment = NSTextAlignmentCenter;
  _field.textColor = [XeniaTheme textPrimary];
  _field.autocorrectionType = UITextAutocorrectionTypeNo;
  _field.spellCheckingType = UITextSpellCheckingTypeNo;
  _field.returnKeyType = UIReturnKeyGo;
  _field.adjustsFontSizeToFitWidth = YES;
  _field.minimumFontSize = 16;
  _field.clearButtonMode = UITextFieldViewModeWhileEditing;
  _scanner = [[XECodeScannerView alloc] init];
  __unsafe_unretained XESignInForm* unsafe_self = self;
  _scanner.onCode = ^(NSString* text) {
    [unsafe_self scanned:text];
  };
  [self applyMode];
  if (_initialCode.length) {
    _field.text = FormattedCode(_initialCode, 8);
    _doneItem.enabled = CodeLength(_field.text) == 8;
    dispatch_async(dispatch_get_main_queue(), ^{
      [unsafe_self doneTapped];
    });
  }
}

// Off screen for good, not under something presented over it.
- (BOOL)isClosed {
  UIViewController* top = self.navigationController ?: self;
  return !self.view.window && !top.presentedViewController;
}

// Closing the sheet while the other device is asked takes the request back.
- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  if (_request && [self isClosed]) {
    [xe_ios_live_account_service() cancelApproval:_request];
  }
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  if (_mode == XESignInModeCode) {
    FitTableHeader(_scanner, self.tableView);
  }
}

- (NSUInteger)limit {
  // NormalizeRecoveryCode and NormalizeLinkCode.
  return _mode == XESignInModeRecovery ? 20 : 8;
}

// The other two ways in, as rows under the field.
- (NSArray<NSNumber*>*)otherModes {
  switch (_mode) {
    case XESignInModeCode:
      return @[ @(XESignInModeAsk), @(XESignInModeRecovery) ];
    case XESignInModeAsk:
      return @[ @(XESignInModeCode), @(XESignInModeRecovery) ];
    case XESignInModeRecovery:
      return @[ @(XESignInModeCode), @(XESignInModeAsk) ];
  }
  return @[];
}

- (NSString*)titleForMode:(XESignInMode)mode {
  switch (mode) {
    case XESignInModeCode:
      return NSLocalizedString(@"Use a Code From My Other Device", @"Sign in sheet: row");
    case XESignInModeAsk:
      return NSLocalizedString(@"Approve on My Other Device", @"Sign in sheet: row");
    case XESignInModeRecovery:
      return NSLocalizedString(@"Use a Recovery Code Instead", @"Sign in sheet: row");
  }
  return nil;
}

- (void)applyMode {
  switch (_mode) {
    case XESignInModeCode:
      [self setHeaderText:NSLocalizedString(@"Or type the code from Add a Device on your other "
                                            @"device.",
                                            @"Sign in sheet: text above the field")];
      break;
    case XESignInModeAsk:
      [self setHeaderText:NSLocalizedString(@"Type your gamertag. Your other device, signed in "
                                            @"to it and open, asks you to let this one in.",
                                            @"Sign in sheet: text above the gamertag field")];
      break;
    case XESignInModeRecovery:
      [self setHeaderText:NSLocalizedString(@"Type the recovery code you kept. Your gamertag, "
                                            @"friends and blocks come back to this device.",
                                            @"Sign in sheet: text above the field")];
      break;
  }
  const BOOL asking = _mode == XESignInModeAsk;
  NSString* placeholder = asking ? NSLocalizedString(@"Gamertag", @"Sign in sheet: field")
                          : _mode == XESignInModeRecovery ? @"XXXX-XXXX-XXXX-XXXX-XXXX"
                                                          : @"XXXX-XXXX";
  _field.attributedPlaceholder = [[[NSAttributedString alloc]
      initWithString:placeholder
          attributes:@{NSForegroundColorAttributeName : [XeniaTheme textMuted]}] autorelease];
  _field.font = asking ? [UIFont systemFontOfSize:22 weight:UIFontWeightSemibold]
                       : [UIFont monospacedSystemFontOfSize:24 weight:UIFontWeightBold];
  _field.autocapitalizationType =
      asking ? UITextAutocapitalizationTypeWords : UITextAutocapitalizationTypeAllCharacters;
  _field.text = @"";
  _field.accessibilityLabel = asking ? NSLocalizedString(@"Gamertag", @"Sign in sheet: field")
                              : _mode == XESignInModeRecovery
                                  ? NSLocalizedString(@"Recovery code", @"Sign in sheet: field")
                                  : NSLocalizedString(@"Code", @"Sign in sheet: field");
  [self setFooterText:nil color:nil];
  _doneItem.enabled = NO;
  if (_mode == XESignInModeCode) {
    [self.view setNeedsLayout];
  } else {
    self.tableView.tableHeaderView = nil;
    [_field becomeFirstResponder];
  }
  [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 1 ? (NSInteger)[self otherModes].count : 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 1) {
    return ActionCell([self
        titleForMode:(XESignInMode)[self otherModes][(NSUInteger)indexPath.row].integerValue]);
  }
  return FieldCell(_field);
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  return section == 1 && _mode == XESignInModeRecovery
             ? NSLocalizedString(@"Lost the recovery code too? Nothing can bring the account back.",
                                 @"Sign in sheet: footer")
             : nil;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 1 && !_busy) {
    _mode = (XESignInMode)[self otherModes][(NSUInteger)indexPath.row].integerValue;
    [self applyMode];
  }
}

- (void)scanned:(NSString*)text {
  NSString* code = CodeFromLinkURL(text);
  if (!code.length) {
    [self setFooterText:NSLocalizedString(@"That isn’t a XeniOS code.", @"Sign in sheet: wrong QR")
                  color:[UIColor systemRedColor]];
    __unsafe_unretained XECodeScannerView* scanner = _scanner;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     [scanner resume];
                   });
    return;
  }
  _field.text = FormattedCode(code, 8);
  _doneItem.enabled = CodeLength(_field.text) == 8;
  [self doneTapped];
}

- (NSString*)typedGamertag {
  return [_field.text
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

- (BOOL)textField:(UITextField*)field
    shouldChangeCharactersInRange:(NSRange)range
                replacementString:(NSString*)string {
  NSString* next = [field.text stringByReplacingCharactersInRange:range withString:string];
  [self setFooterText:nil color:nil];
  if (_mode == XESignInModeAsk) {
    // Gamertags are 15 characters at most.
    if (next.length > 15) {
      return NO;
    }
    _doneItem.enabled =
        !_busy &&
        [[next stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]
            length] > 0;
    return YES;
  }
  field.text = FormattedCode(next, [self limit]);
  _doneItem.enabled = !_busy && CodeLength(field.text) == [self limit];
  return NO;
}

- (BOOL)textFieldShouldReturn:(UITextField* __unused)field {
  [self doneTapped];
  return NO;
}

- (void)finishWithStatus:(XeniaLiveLinkStatus)status {
  _busy = NO;
  XESetFormCommitBusy(self, NO);
  if (status == XeniaLiveLinkStatusLinked) {
    // A second device has a way back in already. Recovering killed the old code, and the new
    // one isn't shown here: the profile page asks to set one up again.
    SetRecoverySaved(_mode != XESignInModeRecovery);
    [self dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  [self showProblem:LinkMessage(status)];
  [_scanner resume];
}

- (void)showProblem:(NSString*)message {
  [self setFooterText:message color:[UIColor systemRedColor]];
  UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, message);
}

- (void)doneTapped {
  if (!_doneItem.enabled || _busy) {
    return;
  }
  _busy = YES;
  XESetFormCommitBusy(self, YES);
  __unsafe_unretained XESignInForm* unsafe_self = self;
  void (^finish)(XeniaLiveLinkStatus) = ^(XeniaLiveLinkStatus status) {
    [unsafe_self finishWithStatus:status];
  };
  switch (_mode) {
    case XESignInModeCode:
      [xe_ios_live_account_service() linkWithCode:_field.text completion:finish];
      break;
    case XESignInModeRecovery:
      [xe_ios_live_account_service() recoverWithCode:_field.text completion:finish];
      break;
    case XESignInModeAsk:
      [self askOtherDevice];
      break;
  }
}

// Asks the account's signed-in devices, then shows the number to pick there and waits. The
// answers hold the form (the service lets go of them once they're called), so a closed sheet
// still gets them.
- (void)askOtherDevice {
  [_field resignFirstResponder];
  XESignInForm* form = self;
  id<XeniaLiveAccountService> service = xe_ios_live_account_service();
  [service
      requestApprovalForGamertag:[self typedGamertag]
                      completion:^(XeniaLiveApprovalRequest* request,
                                   XeniaLiveApprovalStatus status) {
                        if (!request) {
                          [form approvalEnded:status];
                          return;
                        }
                        // Closed while it was still being sent: nobody waits for the answer.
                        if ([form isClosed]) {
                          [service cancelApproval:request];
                          return;
                        }
                        [form->_request release];
                        form->_request = [request retain];
                        [form
                            setFooterText:[NSString stringWithFormat:
                                                        NSLocalizedString(
                                                            @"On your other device, choose %@.",
                                                            @"Sign in sheet: while waiting for the "
                                                            @"other device; %@ is two digits"),
                                                        request.matchNumber ?: @""]
                                    color:[XeniaTheme accent]];
                        [service waitForApproval:request
                                      completion:^(XeniaLiveApprovalStatus result) {
                                        [form->_request release];
                                        form->_request = nil;
                                        [form approvalEnded:result];
                                      }];
                      }];
}

- (void)approvalEnded:(XeniaLiveApprovalStatus)status {
  _busy = NO;
  XESetFormCommitBusy(self, NO);
  switch (status) {
    case XeniaLiveApprovalStatusApproved:
      // The other device has the account already: this one is its second way back in.
      SetRecoverySaved(YES);
      [self dismissViewControllerAnimated:YES completion:nil];
      return;
    case XeniaLiveApprovalStatusDenied:
      [self showProblem:NSLocalizedString(@"Your other device said no, or the number didn’t "
                                          @"match. Try again, and choose the number shown here.",
                                          @"Sign in sheet: approval refused")];
      return;
    case XeniaLiveApprovalStatusTimedOut:
      [self showProblem:NSLocalizedString(@"No answer. Check the gamertag, open XeniOS on your "
                                          @"other device and try again.",
                                          @"Sign in sheet: approval timed out")];
      return;
    case XeniaLiveApprovalStatusUnreachable:
      [self showProblem:LinkMessage(XeniaLiveLinkStatusUnreachable)];
      return;
    case XeniaLiveApprovalStatusCancelled:
      [self setFooterText:nil color:nil];
      return;
  }
}

@end

// ---- Report a player: why, an optional note, and whether to block them too -------------------

@interface XEReportForm : XEForm <UITextViewDelegate>
- (instancetype)initWithXuid:(uint64_t)xuid gamertag:(nullable NSString*)gamertag;
@end

@implementation XEReportForm {
  uint64_t _xuid;
  NSString* _gamertag;
  // The name to show: the gamertag, or "this player".
  NSString* _name;
  NSInteger _reason;  // A XeniaLiveReportReason, or -1 before one is picked.
  UITextView* _note;
  XEFormText* _counter;
  UISwitch* _block;
  UIButton* _send;
}

- (instancetype)initWithXuid:(uint64_t)xuid gamertag:(NSString*)gamertag {
  if ((self = [super init])) {
    _xuid = xuid;
    _gamertag = [gamertag copy];
    _name = [(gamertag.length ? gamertag
                              : NSLocalizedString(@"this player", @"Report form: a player whose "
                                                                  @"gamertag isn't known")) copy];
    _reason = -1;
  }
  return self;
}

- (void)dealloc {
  _note.delegate = nil;
  [_gamertag release];
  [_name release];
  [_note release];
  [_counter release];
  [_block release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = [NSString
      stringWithFormat:NSLocalizedString(@"Report %@", @"Report form title; %@ is a gamertag"),
                       _name];
  [self addCloseButton];
  _send = [self addDoneButtonWithTitle:NSLocalizedString(@"Send", @"Report form: commit button")];
  _send.enabled = NO;  // Until a reason is picked.
  [self setHeaderText:[NSString
                          stringWithFormat:NSLocalizedString(
                                               @"Reports go to the people who run XeniOS online, "
                                               @"with the game you’re in. %@ isn’t told who "
                                               @"reported them.",
                                               @"Report form: text above the reasons; %@ is a "
                                               @"gamertag"),
                                           _name]];

  _note = [[UITextView alloc] init];
  _note.translatesAutoresizingMaskIntoConstraints = NO;
  [_note.heightAnchor constraintEqualToConstant:112.0].active = YES;
  _note.backgroundColor = [UIColor clearColor];
  _note.textColor = [XeniaTheme textPrimary];
  _note.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  _note.adjustsFontForContentSizeCategory = YES;
  _note.textContainerInset = UIEdgeInsetsMake(10, 0, 10, 0);
  _note.textContainer.lineFragmentPadding = 0;
  _note.delegate = self;
  _note.accessibilityLabel = NSLocalizedString(@"Note", @"Report form: VoiceOver for the note");
  _note.accessibilityHint =
      NSLocalizedString(@"Optional. Up to 500 characters.", @"Report form: VoiceOver hint");
  _counter = [[XEFormText alloc] init];
  _counter.label.textAlignment = NSTextAlignmentNatural;
  [self updateCounter];

  _block = [[UISwitch alloc] init];
  _block.on = YES;
  _block.onTintColor = [XeniaTheme accent];
}

- (void)updateCounter {
  _counter.label.text = [NSString
      stringWithFormat:NSLocalizedString(@"%lu/%lu", @"Report form: characters in the note, of "
                                                     @"the most it holds"),
                       (unsigned long)_note.text.length, (unsigned long)XeniaLiveReportNoteLimit];
}

- (NSString*)reasonTitle:(NSInteger)reason {
  switch ((XeniaLiveReportReason)reason) {
    case XeniaLiveReportReasonCheating:
      return NSLocalizedString(@"Cheating", @"Report reason");
    case XeniaLiveReportReasonHarassment:
      return NSLocalizedString(@"Harassment", @"Report reason");
    case XeniaLiveReportReasonOffensiveName:
      return NSLocalizedString(@"Offensive gamertag", @"Report reason");
    case XeniaLiveReportReasonOffensiveContent:
      return NSLocalizedString(@"Offensive content", @"Report reason");
    case XeniaLiveReportReasonSpam:
      return NSLocalizedString(@"Spam", @"Report reason");
    case XeniaLiveReportReasonOther:
      return NSLocalizedString(@"Something else", @"Report reason");
  }
  return @"";
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 3;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? XeniaLiveReportReasonOther + 1 : 1;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return section == 1 ? NSLocalizedString(@"Note (Optional)", @"Report form: section header")
                      : nil;
}

- (UIView*)tableView:(UITableView*)tableView viewForFooterInSection:(NSInteger)section {
  // The counter is a view of its own, so typing updates it without reloading the note's row.
  return section == 1 ? _counter : [super tableView:tableView viewForFooterInSection:section];
}

- (CGFloat)tableView:(UITableView*)tableView heightForFooterInSection:(NSInteger)section {
  return section == 1 ? [_counter heightForWidth:TextWidth(tableView)]
                      : [super tableView:tableView heightForFooterInSection:section];
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
  if (indexPath.section == 0) {
    content.text = [self reasonTitle:indexPath.row];
    cell.contentConfiguration = content;
    cell.tintColor = [XeniaTheme accent];
    cell.accessoryType = indexPath.row == _reason ? UITableViewCellAccessoryCheckmark
                                                  : UITableViewCellAccessoryNone;
    cell.accessibilityTraits = indexPath.row == _reason ? UIAccessibilityTraitSelected
                                                        : UIAccessibilityTraitNone;
    return cell;
  }
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  if (indexPath.section == 1) {
    [cell.contentView addSubview:_note];
    [NSLayoutConstraint activateConstraints:@[
      [_note.leadingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.leadingAnchor],
      [_note.trailingAnchor
          constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.trailingAnchor],
      [_note.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor],
      [_note.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor],
    ]];
    return cell;
  }
  content.text = [NSString
      stringWithFormat:NSLocalizedString(@"Also Block %@", @"Report form: switch; %@ is a "
                                                           @"gamertag"),
                       _name];
  cell.contentConfiguration = content;
  _block.accessibilityLabel = content.text;
  cell.accessoryView = _block;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section != 0) {
    return;
  }
  _reason = indexPath.row;
  _send.enabled = YES;
  [tableView reloadSections:[NSIndexSet indexSetWithIndex:0]
           withRowAnimation:UITableViewRowAnimationNone];
}

- (BOOL)textView:(UITextView*)textView
    shouldChangeTextInRange:(NSRange)range
            replacementText:(NSString*)text {
  const NSUInteger limit = XeniaLiveReportNoteLimit;
  const NSUInteger kept = textView.text.length - range.length;
  if (kept + text.length <= limit) {
    return YES;
  }
  // A paste that's too long goes in as far as it fits, without cutting a character in two.
  const NSUInteger room = kept < limit ? limit - kept : 0;
  NSUInteger cut = room;
  while (cut > 0) {
    const NSRange last = [text rangeOfComposedCharacterSequenceAtIndex:cut - 1];
    if (NSMaxRange(last) <= room) {
      cut = NSMaxRange(last);
      break;
    }
    cut = last.location;
  }
  if (cut) {
    [textView replaceRange:[self textRangeIn:textView for:range]
                  withText:[text substringToIndex:cut]];
    [self updateCounter];
  }
  return NO;
}

- (UITextRange*)textRangeIn:(UITextView*)textView for:(NSRange)range {
  UITextPosition* start = [textView positionFromPosition:textView.beginningOfDocument
                                                  offset:(NSInteger)range.location];
  UITextPosition* end = [textView positionFromPosition:start offset:(NSInteger)range.length];
  return [textView textRangeFromPosition:start toPosition:end];
}

- (void)textViewDidChange:(UITextView* __unused)textView {
  [self updateCounter];
}

// The whole note and its counter above the Send button, once the keyboard is up.
- (void)textViewDidBeginEditing:(UITextView* __unused)textView {
  XEReportForm* form = [self retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.35 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (form.isViewLoaded && form.view.window) {
                     [form.tableView scrollRectToVisible:[form.tableView rectForFooterInSection:1]
                                                animated:YES];
                   }
                   [form release];
                 });
}

- (void)doneTapped {
  if (_reason < 0) {
    return;
  }
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  if (![social reportPlayer:_xuid reason:(XeniaLiveReportReason)_reason note:_note.text]) {
    ShowAlert(self, NSLocalizedString(@"Couldn’t Send Report", @"Report form: alert title"),
              NSLocalizedString(@"You need to be online to report a player. Try again once "
                                @"you’re connected.",
                                @"Report form: alert message when offline"));
    return;
  }
  // After the report, so it's sent first; the answer comes as a toast once this has closed.
  if (_block.on) {
    [social blockPlayer:_xuid gamertag:_gamertag];
  }
  [self dismissViewControllerAnimated:YES completion:nil];
}

@end

// ---- Blocked players: who this profile blocked, with Unblock for each ----------------------

@interface XEBlockedPlayersViewController : XESheetTableViewController
@end

@implementation XEBlockedPlayersViewController {
  NSArray<XeniaLiveBlockedPlayer*>* _players;
  // The empty state, as the table's footer: the glass card fits the table's content, so the
  // message has to be part of it (a contentUnavailableConfiguration overlays a zero-height card).
  UIContentUnavailableView* _empty;
}

- (instancetype)init {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_players release];
  [_empty release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Blocked Players", @"Blocked players page title");
  self.tableView.backgroundColor = [UIColor clearColor];
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(reload)
                                               name:XeniaLiveSocialDidChangeNotification
                                             object:nil];
  [self reload];
}

- (void)viewDidLayoutSubviews {
  // Size the footer first, so the card fits the table with the message in it. A table picks up
  // a footer's new size only when the footer is set again; doing that only on a change keeps
  // this to one pass.
  UITableView* table = self.tableView;
  if (_empty && table.tableFooterView == _empty) {
    const CGFloat width = CGRectGetWidth(table.bounds);
    const CGFloat height =
        std::ceil([_empty systemLayoutSizeFittingSize:CGSizeMake(width, 0.0)
                        withHorizontalFittingPriority:UILayoutPriorityRequired
                              verticalFittingPriority:UILayoutPriorityFittingSizeLevel]
                      .height);
    if (width > 0.0 && (std::fabs(CGRectGetWidth(_empty.frame) - width) >= 0.5 ||
                        std::fabs(CGRectGetHeight(_empty.frame) - height) >= 0.5)) {
      _empty.frame = CGRectMake(0.0, 0.0, width, height);
      table.tableFooterView = _empty;
    }
  }
  [super viewDidLayoutSubviews];
}

- (void)reload {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  NSArray<XeniaLiveBlockedPlayer*>* players = social.blockedPlayers;
  const BOOL online = social.state == XeniaLiveSocialStateOnline;
  BOOL same = players.count == _players.count;
  for (NSUInteger i = 0; same && i < players.count; ++i) {
    same = players[i].xuid == _players[i].xuid &&
           (players[i].gamertag == _players[i].gamertag ||
            [players[i].gamertag isEqualToString:_players[i].gamertag]);
  }
  if (!same) {
    [_players release];
    _players = [players copy];
    [self.tableView reloadData];
  }
  if (_players.count) {
    if (self.tableView.tableFooterView == _empty) {
      self.tableView.tableFooterView = nil;
    }
    return;
  }
  UIContentUnavailableConfiguration* empty = [UIContentUnavailableConfiguration emptyConfiguration];
  empty.image = [UIImage systemImageNamed:online ? @"hand.raised" : @"wifi.slash"];
  empty.text = online ? NSLocalizedString(@"No Blocked Players", @"Blocked players: empty title")
                      : NSLocalizedString(@"Not Online", @"Blocked players: offline title");
  empty.secondaryText =
      online ? NSLocalizedString(@"To block a player, open their gamer card and choose Block from "
                                 @"the ⋯ menu.",
                                 @"Blocked players: empty text")
             : NSLocalizedString(@"Your blocked players show here while you’re online.",
                                 @"Blocked players: offline text");
  if (!_empty) {
    _empty = [[UIContentUnavailableView alloc] initWithConfiguration:empty];
  } else {
    _empty.configuration = empty;
  }
  // The next layout measures it at the table's width.
  if (self.tableView.tableFooterView != _empty) {
    self.tableView.tableFooterView = _empty;
  }
  [self.view setNeedsLayout];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return _players.count ? 1 : 0;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  return (NSInteger)_players.count;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger __unused)section {
  return NSLocalizedString(@"Blocked players can’t send you friend requests or invites, and don’t "
                           @"show in your lists.",
                           @"Blocked players: text under the list");
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaLiveBlockedPlayer* player = _players[(NSUInteger)indexPath.row];
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  if (player.gamertag.length) {
    content.text = player.gamertag;
  } else {
    content.text = NSLocalizedString(@"Unknown Player", @"Blocked players: a row with no name");
    content.secondaryText = NSLocalizedString(@"Blocked on another device",
                                              @"Blocked players: why a row has no name");
    content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  }
  content.image = [UIImage systemImageNamed:@"person.crop.circle.badge.xmark"];
  content.imageProperties.tintColor = [XeniaTheme textSecondary];
  cell.contentConfiguration = content;
  UIButtonConfiguration* config = [UIButtonConfiguration grayButtonConfiguration];
  config.title = NSLocalizedString(@"Unblock", @"Blocked players: row button");
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.buttonSize = UIButtonConfigurationSizeSmall;
  config.baseForegroundColor = [XeniaTheme accent];
  UIButton* unblock = [UIButton buttonWithConfiguration:config primaryAction:nil];
  unblock.accessibilityLabel = [NSString
      stringWithFormat:NSLocalizedString(@"Unblock %@", @"Blocked players: VoiceOver for a row's "
                                                        @"button; %@ is a gamertag"),
                       content.text];
  const uint64_t xuid = player.xuid;
  [unblock addAction:[UIAction actionWithHandler:^(__kindof UIAction* action) {
             ((UIButton*)action.sender).enabled = NO;
             [[XeniaLiveSocial shared] unblockPlayer:xuid];
           }]
    forControlEvents:UIControlEventTouchUpInside];
  [unblock sizeToFit];
  cell.accessoryView = unblock;
  return cell;
}

@end

// ---- The profile page: one list for the profile's account, devices and data ------------------

typedef NS_ENUM(NSInteger, XEPageRow) {
  XEPageRowAddDevice = 0,
  XEPageRowSharedSaves,
  XEPageRowCopyProfile,
  XEPageRowRecovery,
  XEPageRowGamertag,
  XEPageRowBlocked,
  XEPageRowAppearOffline,
  XEPageRowRequestsAnyone,
  XEPageRowRequestsCode,
  XEPageRowDevice = 1000,  // + index into the device list.
};

@interface XEProfilePageViewController : XESheetTableViewController
- (instancetype)initWithGamertag:(NSString*)gamertag
                          active:(BOOL)active
                        switchTo:(nullable BOOL (^)(void))switch_to
                   changePicture:(nullable void (^)(UIViewController* page))change_picture;
@end

@implementation XEProfilePageViewController {
  NSString* _gamertag;
  BOOL _active;
  BOOL (^_switchTo)(void);
  void (^_changePicture)(UIViewController*);
  XEProfileHeader* _header;
  NSArray<NSArray<NSNumber*>*>* _sections;
  NSArray<NSString*>* _sectionTitles;
  NSArray<XeniaLiveDevice*>* _devices;
  NSString* _sharedText;
  // The Privacy group's index (its footer explains Appear Offline), or -1, and the blocked count
  // its row shows.
  NSInteger _privacySection;
  NSUInteger _blockedCount;
  // The Friend Requests group's index, or -1 (offline, or the service hasn't the setting), and
  // the choice it shows.
  NSInteger _requestsSection;
  XeniaLiveFriendRequestsSetting _friendRequests;
}

- (instancetype)initWithGamertag:(NSString*)gamertag
                          active:(BOOL)active
                        switchTo:(BOOL (^)(void))switch_to
                   changePicture:(void (^)(UIViewController*))change_picture {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
    _gamertag = [gamertag copy];
    _active = active;
    _switchTo = [switch_to copy];
    _changePicture = [change_picture copy];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_gamertag release];
  [_switchTo release];
  [_changePicture release];
  [_header release];
  [_sections release];
  [_sectionTitles release];
  [_devices release];
  [_sharedText release];
  [super dealloc];
}

- (BOOL)online {
  return _active && xe_ios_live_account_service().summary.online;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor clearColor];
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  _header = [[XEProfileHeader alloc] init];
  [_header.editPicture addTarget:self
                          action:@selector(editPictureTapped)
                forControlEvents:UIControlEventTouchUpInside];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(accountChanged)
                                               name:XeniaLiveAccountDidChangeNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(socialChanged)
                                               name:XeniaLiveSocialDidChangeNotification
                                             object:nil];
  [self reload];
}

// Friends' presence changes often; only the blocked count and who may send friend requests show
// here.
- (void)socialChanged {
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  if ((_privacySection >= 0 && social.blockedPlayerCount != _blockedCount) ||
      (_active && social.friendRequestsSetting != _friendRequests)) {
    [self reload];
  }
}

- (void)appearOfflineToggled:(UISwitch*)toggle {
  [XeniaLiveSocial shared].appearOffline = toggle.on;
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self reload];
  [self refreshDevices];
  [self refreshShared];
}

- (void)viewDidLayoutSubviews {
  FitTableHeader(_header, self.tableView);
  [super viewDidLayoutSubviews];
}

- (void)editPictureTapped {
  if (_changePicture) {
    _changePicture(self);
  }
}

- (void)accountChanged {
  [self reload];
  [self refreshDevices];
}

- (void)refreshDevices {
  if (![self online]) {
    [_devices release];
    _devices = nil;
    return;
  }
  __unsafe_unretained XEProfilePageViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      loadDevicesWithCompletion:^(NSArray<XeniaLiveDevice*>* devices, NSError* __unused error) {
        [unsafe_self->_devices release];
        unsafe_self->_devices = [devices copy];
        [unsafe_self reload];
      }];
}

// "With Kid", "With 2 Profiles", "Every Profile" or "Off".
- (void)refreshShared {
  if (!_active || !xe_ios_live_account_supports_shared_saves()) {
    return;
  }
  id<XeniaLiveAccountService> service = xe_ios_live_account_service();
  __unsafe_unretained XEProfilePageViewController* unsafe_self = self;
  void (^set)(NSString*) = ^(NSString* text) {
    [unsafe_self->_sharedText release];
    unsafe_self->_sharedText = [text copy];
    [unsafe_self.tableView reloadData];
  };
  if (service.savesSharedAcrossProfiles) {
    set(NSLocalizedString(@"Every Profile", @"Profile page: Shared Saves detail"));
    return;
  }
  [service localProfilesWithCompletion:^(NSArray<XeniaLiveLocalProfile*>* profiles) {
    NSMutableArray<XeniaLiveLocalProfile*>* others = [NSMutableArray array];
    for (XeniaLiveLocalProfile* profile in profiles) {
      if (!profile.thisProfile) {
        [others addObject:profile];
      }
    }
    if (!others.count) {
      set(NSLocalizedString(@"Off", @"Profile page: Shared Saves detail"));
      return;
    }
    NSMutableArray<NSString*>* with = [NSMutableArray array];
    __block NSUInteger waiting = others.count;
    for (XeniaLiveLocalProfile* profile in others) {
      [service saveGamesSharedWithProfile:profile.profileId
                               completion:^(NSArray<XeniaLiveSaveGame*>* games) {
                                 for (XeniaLiveSaveGame* game in games) {
                                   if (game.shared) {
                                     [with addObject:profile.gamertag];
                                     break;
                                   }
                                 }
                                 if (--waiting) {
                                   return;
                                 }
                                 if (!with.count) {
                                   set(NSLocalizedString(@"Off", @"Profile page: Shared Saves detail"));
                                 } else if (with.count == 1) {
                                   set([NSString stringWithFormat:NSLocalizedString(
                                                                      @"With %@",
                                                                      @"Profile page: Shared Saves "
                                                                      @"detail; %@ is a gamertag"),
                                                                  with.firstObject]);
                                 } else {
                                   set([NSString stringWithFormat:NSLocalizedString(
                                                                      @"With %lu Profiles",
                                                                      @"Profile page: Shared Saves "
                                                                      @"detail"),
                                                                  (unsigned long)with.count]);
                                 }
                               }];
    }
  }];
}

- (void)reload {
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  const BOOL online = [self online];
  NSString* name = online ? summary.gamertag : _gamertag;
  // The name is the page's headline, under the avatar; the bar keeps only ‹ and ✕ (as Contacts).
  self.title = name;
  self.navigationItem.title = @"";
  // The picture belongs to the local profile, whatever the online name.
  _header.avatar.image =
      xe_profile_avatar_image(xe_profile_has_picture(_gamertag) ? _gamertag : name, _active, 72.0);
  // Only a signed-in profile's picture can be changed.
  _header.editPicture.hidden = !(_active && _changePicture);
  _header.name.text = name;
  _header.status.textColor = [XeniaTheme textSecondary];
  _header.caption.text = nil;
  NSString* device = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad
                         ? NSLocalizedString(@"On this iPad only", @"Profile page: status")
                         : NSLocalizedString(@"On this iPhone only", @"Profile page: status");
  if (!_active) {
    _header.status.text = device;
    [_header setButton:_switchTo ? PrimaryButton([NSString stringWithFormat:NSLocalizedString(
                                                                    @"Switch to %@",
                                                                    @"Profile page: button; %@ is "
                                                                    @"a gamertag"),
                                                                _gamertag],
                                                 self, @selector(switchTapped))
                                 : nil];
  } else if (!online) {
    _header.status.text = device;
    [_header setButton:PrimaryButton(NSLocalizedString(@"Get a Gamertag", @"Profile page: button"),
                                     self, @selector(getGamertagTapped))];
    _header.caption.text =
        NSLocalizedString(@"Play with friends, in parties and public games. Your saves and "
                          @"achievements stay.",
                          @"Profile page: caption under Get a Gamertag");
  } else {
    [_header setButton:nil];
    if (summary.renameFlagged) {
      _header.status.text =
          NSLocalizedString(@"Pick a new gamertag", @"Profile page: status when flagged");
      _header.status.textColor = [UIColor systemOrangeColor];
    } else if (_devices.count > 1) {
      _header.status.text = [NSString
          stringWithFormat:NSLocalizedString(@"On %lu devices", @"Profile page: status; a count"),
                           (unsigned long)_devices.count];
    } else {
      _header.status.text = NSLocalizedString(@"Online", @"Profile page: status");
    }
  }
  _header.isAccessibilityElement = NO;
  [_header setNeedsLayout];

  NSMutableArray* sections = [NSMutableArray array];
  NSMutableArray* titles = [NSMutableArray array];
  _privacySection = -1;
  _requestsSection = -1;
  _friendRequests = XeniaLiveFriendRequestsSettingUnknown;
  if (_active) {
    if (online) {
      NSMutableArray* devices = [NSMutableArray array];
      for (NSUInteger i = 0; i < _devices.count; ++i) {
        [devices addObject:@(XEPageRowDevice + (NSInteger)i)];
      }
      [devices addObject:@(XEPageRowAddDevice)];
      [sections addObject:devices];
      [titles addObject:NSLocalizedString(@"Devices", @"Profile page: header")];
    }
    if (xe_ios_live_account_supports_profile_data()) {
      [sections addObject:xe_ios_live_account_supports_shared_saves()
                              ? @[ @(XEPageRowSharedSaves), @(XEPageRowCopyProfile) ]
                              : @[ @(XEPageRowCopyProfile) ]];
      [titles addObject:NSLocalizedString(@"Profile Data", @"Profile page: header")];
    }
    if (online) {
      _privacySection = (NSInteger)sections.count;
      _blockedCount = [XeniaLiveSocial shared].blockedPlayerCount;
      // Appear Offline last, so the group's footer sits under it.
      [sections addObject:@[ @(XEPageRowBlocked), @(XEPageRowAppearOffline) ]];
      [titles addObject:NSLocalizedString(@"Privacy", @"Profile page: header")];
      // Under Privacy, as its own choice of two with its own footer; left out while the service
      // doesn't say it (one without the setting).
      _friendRequests = [XeniaLiveSocial shared].friendRequestsSetting;
      if (_friendRequests != XeniaLiveFriendRequestsSettingUnknown) {
        _requestsSection = (NSInteger)sections.count;
        [sections addObject:@[ @(XEPageRowRequestsAnyone), @(XEPageRowRequestsCode) ]];
        [titles addObject:NSLocalizedString(@"Friend Requests", @"Profile page: header over who "
                                                                @"may send friend requests")];
      }
      [sections addObject:@[ @(XEPageRowRecovery), @(XEPageRowGamertag) ]];
      [titles addObject:@""];
    }
  }
  [_sections release];
  _sections = [sections copy];
  [_sectionTitles release];
  _sectionTitles = [titles copy];
  [self.tableView reloadData];
  [self.view setNeedsLayout];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return (NSInteger)_sections.count;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return (NSInteger)_sections[(NSUInteger)section].count;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  NSString* title = _sectionTitles[(NSUInteger)section];
  return title.length ? title : nil;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (section == _requestsSection) {
    return NSLocalizedString(@"With My Code, players can add you only with your friend code, link "
                             @"or QR code.",
                             @"Profile page: footer under Friend Requests");
  }
  return section == _privacySection
             ? NSLocalizedString(@"Friends see you as offline. You can still join games and get "
                                 @"invites.",
                                 @"Profile page: footer under Appear Offline")
             : nil;
}

- (NSInteger)rowAt:(NSIndexPath*)indexPath {
  return _sections[(NSUInteger)indexPath.section][(NSUInteger)indexPath.row].integerValue;
}

- (BOOL)wantsRecovery {
  return !RecoverySaved() && _devices.count <= 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  const NSInteger row = [self rowAt:indexPath];
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  if (row == XEPageRowAddDevice) {
    return ActionCell(NSLocalizedString(@"Add a Device…", @"Profile page: row"));
  }
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  UIListContentConfiguration* content = [UIListContentConfiguration valueCellConfiguration];
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  if (row >= XEPageRowDevice) {
    XeniaLiveDevice* device = _devices[(NSUInteger)(row - XEPageRowDevice)];
    content = [UIListContentConfiguration subtitleCellConfiguration];
    content.text = device.thisDevice
                       ? [NSString stringWithFormat:NSLocalizedString(
                                                        @"%@ (This Device)",
                                                        @"Profile page: device row; %@ is its name"),
                                                    device.name]
                       : device.name;
    if (!device.lastSeen || device.thisDevice) {
      content.secondaryText = NSLocalizedString(@"Active now", @"Profile page: device detail");
    } else {
      NSRelativeDateTimeFormatter* relative = [[[NSRelativeDateTimeFormatter alloc] init] autorelease];
      relative.dateTimeStyle = NSRelativeDateTimeFormatterStyleNamed;
      content.secondaryText = [NSString
          stringWithFormat:NSLocalizedString(@"Last seen %@",
                                             @"Profile page: device detail; %@ is like “yesterday”"),
                           [relative localizedStringForDate:device.lastSeen relativeToDate:[NSDate date]]];
    }
    content.secondaryTextProperties.color = [XeniaTheme textSecondary];
    content.image = Glyph([device.kind isEqualToString:@"ipad"]  ? @"ipad"
                          : [device.kind isEqualToString:@"mac"] ? @"laptopcomputer"
                                                                 : @"iphone");
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = device.thisDevice ? UITableViewCellSelectionStyleNone
                                            : UITableViewCellSelectionStyleDefault;
    cell.contentConfiguration = content;
    return cell;
  }
  switch (row) {
    case XEPageRowSharedSaves:
      content.text = NSLocalizedString(@"Shared Saves", @"Profile page: row");
      content.secondaryText = _sharedText;
      content.image = Glyph(@"person.2");
      break;
    case XEPageRowCopyProfile:
      content.text = NSLocalizedString(@"Copy Profile…", @"Profile page: row");
      content.image = Glyph(@"square.and.arrow.up");
      break;
    case XEPageRowRecovery:
      content.image = Glyph(@"key");
      if ([self wantsRecovery]) {
        content.text = NSLocalizedString(@"Set Up Account Recovery", @"Profile page: row");
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.accessoryView = AttentionAccessory();
      } else {
        content.text = NSLocalizedString(@"Account Recovery", @"Profile page: row");
        content.secondaryText = RecoverySaved()
                                    ? NSLocalizedString(@"On", @"Profile page: row detail")
                                    : NSLocalizedString(@"Off", @"Profile page: row detail");
      }
      break;
    case XEPageRowBlocked:
      content.text = NSLocalizedString(@"Blocked Players", @"Profile page: row");
      content.image = Glyph(@"hand.raised");
      if (_blockedCount) {
        content.secondaryText = [NSString stringWithFormat:@"%lu", (unsigned long)_blockedCount];
      }
      break;
    case XEPageRowAppearOffline: {
      content = [UIListContentConfiguration cellConfiguration];
      content.text = NSLocalizedString(@"Appear Offline", @"Profile page: switch");
      content.image = Glyph(@"eye.slash");
      cell.accessoryType = UITableViewCellAccessoryNone;
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
      UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
      toggle.on = [XeniaLiveSocial shared].appearOffline;
      toggle.onTintColor = [XeniaTheme accent];
      toggle.accessibilityLabel = content.text;
      [toggle addTarget:self
                    action:@selector(appearOfflineToggled:)
          forControlEvents:UIControlEventValueChanged];
      cell.accessoryView = toggle;
      break;
    }
    case XEPageRowRequestsAnyone:
    case XEPageRowRequestsCode: {
      // A choice: a checkmark on the one the service has.
      content = [UIListContentConfiguration cellConfiguration];
      const BOOL code = row == XEPageRowRequestsCode;
      content.text = code ? NSLocalizedString(@"Only with My Code",
                                              @"Profile page: friend requests only by friend code")
                          : NSLocalizedString(@"Anyone",
                                              @"Profile page: friend requests from anyone");
      const BOOL chosen = _friendRequests == (code ? XeniaLiveFriendRequestsSettingCode
                                                   : XeniaLiveFriendRequestsSettingAnyone);
      cell.accessoryType =
          chosen ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
      cell.tintColor = [XeniaTheme accent];
      cell.accessibilityTraits = chosen ? UIAccessibilityTraitSelected : UIAccessibilityTraitNone;
      break;
    }
    case XEPageRowGamertag:
      content.text = NSLocalizedString(@"Change Gamertag", @"Profile page: row");
      content.image = Glyph(@"person.text.rectangle");
      if (summary.renameFlagged) {
        content.secondaryText = NSLocalizedString(@"Needed", @"Profile page: row detail");
        content.secondaryTextProperties.color = [UIColor systemOrangeColor];
      } else if (summary.nextRename.timeIntervalSinceNow > 0) {
        content.secondaryText = [NSString
            stringWithFormat:NSLocalizedString(@"After %@", @"Profile page: row detail; %@ is a date"),
                             DateText(summary.nextRename)];
      }
      break;
    default:
      break;
  }
  cell.contentConfiguration = content;
  return cell;
}

- (UISwipeActionsConfiguration*)tableView:(UITableView* __unused)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  const NSInteger row = [self rowAt:indexPath];
  if (row < XEPageRowDevice) {
    return nil;
  }
  XeniaLiveDevice* device = _devices[(NSUInteger)(row - XEPageRowDevice)];
  if (device.thisDevice) {
    return nil;
  }
  __unsafe_unretained XEProfilePageViewController* unsafe_self = self;
  UIContextualAction* remove = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:NSLocalizedString(@"Remove", @"Profile page: swipe action")
                        handler:^(UIContextualAction* __unused action, UIView* __unused view,
                                  void (^done)(BOOL)) {
                          [unsafe_self confirmRemove:device];
                          done(YES);
                        }];
  return [UISwipeActionsConfiguration configurationWithActions:@[ remove ]];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  const NSInteger row = [self rowAt:indexPath];
  if (row >= XEPageRowDevice) {
    XeniaLiveDevice* device = _devices[(NSUInteger)(row - XEPageRowDevice)];
    if (!device.thisDevice) {
      [self confirmRemove:device];
    }
    return;
  }
  switch (row) {
    // No field to type in, so these push inside the card, like Shared Saves.
    case XEPageRowAddDevice:
      [self.navigationController pushViewController:[[[XEAddDeviceForm alloc] init] autorelease]
                                           animated:YES];
      break;
    case XEPageRowSharedSaves:
      [self.navigationController pushViewController:[[[XEShareHubViewController alloc] init] autorelease]
                                           animated:YES];
      break;
    case XEPageRowCopyProfile:
      [self.navigationController pushViewController:[[[XECopyProfileForm alloc] init] autorelease]
                                           animated:YES];
      break;
    case XEPageRowRecovery:
      if ([self wantsRecovery] || !RecoverySaved()) {
        [self makeRecoveryCodeFirstTime:YES];
      } else {
        [self confirmNewRecoveryCode];
      }
      break;
    case XEPageRowGamertag:
      [self presentRename];
      break;
    case XEPageRowBlocked:
      [self.navigationController
          pushViewController:[[[XEBlockedPlayersViewController alloc] init] autorelease]
                    animated:YES];
      break;
    // The checkmark moves when the service answers (socialChanged), here and on other devices.
    case XEPageRowRequestsAnyone:
      [XeniaLiveSocial shared].friendRequestsSetting = XeniaLiveFriendRequestsSettingAnyone;
      break;
    case XEPageRowRequestsCode:
      [XeniaLiveSocial shared].friendRequestsSetting = XeniaLiveFriendRequestsSettingCode;
      break;
    default:
      break;
  }
}

- (void)switchTapped {
  if (_switchTo && _switchTo()) {
    _active = YES;
    [self reload];
    [self refreshDevices];
    [self refreshShared];
  }
}

- (void)getGamertagTapped {
  __unsafe_unretained XEProfilePageViewController* unsafe_self = self;
  xe_ios_present_go_online(self, _gamertag, ^{
    // A new account starts without a recovery code saved; the page asks for one.
    SetRecoverySaved(NO);
    [unsafe_self reload];
    [unsafe_self refreshDevices];
  });
}

- (void)presentRename {
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  __unsafe_unretained XEProfilePageViewController* unsafe_self = self;
  XEGamertagForm* form = [[[XEGamertagForm alloc]
      initWithMode:summary.renameFlagged ? XEGamertagModeFlagged : XEGamertagModeRename
       currentName:summary.gamertag
              done:^(XeniaLiveNameResult* __unused result) {
                [unsafe_self reload];
              }] autorelease];
  PresentForm(self, form, NO);
}

- (void)makeRecoveryCodeFirstTime:(BOOL)first {
  __unsafe_unretained XEProfilePageViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      createRecoveryCodeWithCompletion:^(NSString* code, NSError* error) {
        if (!code.length) {
          ShowAlert(unsafe_self,
                    NSLocalizedString(@"Couldn’t Make a Code", @"Profile page: alert title"),
                    error.localizedDescription
                        ?: NSLocalizedString(@"Check your connection and try again.",
                                             @"Profile page: alert message"));
          return;
        }
        PresentForm(unsafe_self,
                    [[[XERecoveryForm alloc] initWithCode:code
                                                firstTime:first
                                                     done:^{
                                                       SetRecoverySaved(YES);
                                                       [unsafe_self reload];
                                                     }] autorelease],
                    YES);
      }];
}

- (void)confirmNewRecoveryCode {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Make a New Recovery Code?",
                                                 @"Profile page: alert title")
                       message:NSLocalizedString(@"Your old code stops working. The new one is "
                                                 @"shown once.",
                                                 @"Profile page: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Profile page: alert button")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  __unsafe_unretained XEProfilePageViewController* unsafe_self = self;
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Make Code",
                                                                    @"Profile page: alert button")
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction* __unused action) {
                                            [unsafe_self makeRecoveryCodeFirstTime:NO];
                                          }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)confirmRemove:(XeniaLiveDevice*)device {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:[NSString stringWithFormat:NSLocalizedString(
                                                              @"Remove %@?",
                                                              @"Profile page: alert title; %@ is "
                                                              @"the device name"),
                                                          device.name]
                       message:NSLocalizedString(@"It’s signed out, and needs a new code from Add a "
                                                 @"Device to come back.",
                                                 @"Profile page: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Profile page: alert button")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  __unsafe_unretained XEProfilePageViewController* unsafe_self = self;
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Remove", @"Profile page: alert button")
                                 style:UIAlertActionStyleDestructive
                               handler:^(UIAlertAction* __unused action) {
                                 [xe_ios_live_account_service()
                                     removeDevice:device
                                       completion:^(NSError* error) {
                                         if (error) {
                                           ShowAlert(unsafe_self,
                                                     NSLocalizedString(@"Couldn’t Remove",
                                                                       @"Profile page: alert title"),
                                                     error.localizedDescription);
                                         }
                                         [unsafe_self refreshDevices];
                                       }];
                               }]];
  [self presentViewController:alert animated:YES completion:nil];
}

@end


// ---- Entry points ----------------------------------------------------------------------------

UIViewController* xe_ios_new_profile_page(
    NSString* gamertag, BOOL active, BOOL (^_Nullable switch_to)(void),
    void (^_Nullable change_picture)(UIViewController* page)) {
  return [[[XEProfilePageViewController alloc] initWithGamertag:gamertag
                                                          active:active
                                                        switchTo:switch_to
                                                   changePicture:change_picture] autorelease];
}

void xe_ios_present_report_player(UIViewController* presenter, uint64_t xuid,
                                  NSString* _Nullable gamertag) {
  PresentForm(presenter, [[[XEReportForm alloc] initWithXuid:xuid gamertag:gamertag] autorelease],
              NO);
}

void xe_ios_present_sign_in(UIViewController* presenter, NSString* _Nullable code) {
  if (xe_ios_online_features_enabled()) {
    PresentForm(presenter, [[[XESignInForm alloc] initWithCode:code] autorelease], NO);
    return;
  }
  // Signing in goes online, so with Online Features off the sheet could only fail; ask first.
  NSString* kept = [[code copy] autorelease];
  xe_ios_confirm_turn_on_online_features(presenter, ^{
    xe_ios_set_online_features_enabled(YES);
    PresentForm(presenter, [[[XESignInForm alloc] initWithCode:kept] autorelease], NO);
  });
}

BOOL xe_ios_handle_link_url(NSURL* url, UIViewController* presenter) {
  NSString* code = CodeFromLinkURL(url.absoluteString);
  if (!code.length) {
    return NO;
  }
  if (!xe_ios_live_account_ui_enabled()) {
    return YES;
  }
  // The sign-in page sends a code from a link at once, so an online device would re-link
  // without a word. Say what's true instead.
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  if (summary.online) {
    ShowAlert(TopOf(presenter),
              NSLocalizedString(@"Already Signed In", @"Device link opened while online: title"),
              [NSString stringWithFormat:NSLocalizedString(
                                             @"This device is already signed in as %@.",
                                             @"Device link opened while online; %@ is the gamertag"),
                                         summary.gamertag]);
    return YES;
  }
  xe_ios_present_sign_in(presenter, code);
  return YES;
}

void xe_ios_present_go_online(UIViewController* presenter, NSString* profile_gamertag,
                              void (^done)(void)) {
  // The recovery code waits: the profile page asks for it (Set Up Account Recovery) rather than
  // putting it between the player and the game.
  void (^finished)(void) = [[done copy] autorelease];
  XEGamertagForm* form = [[[XEGamertagForm alloc]
      initWithMode:XEGamertagModeGoOnline
       currentName:profile_gamertag
              done:^(XeniaLiveNameResult* __unused result) {
                if (finished) {
                  finished();
                }
              }] autorelease];
  PresentForm(presenter, form, NO);
}

void xe_ios_present_flagged_rename_if_needed(UIViewController* presenter) {
  static BOOL asked = NO;
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  if (asked || !summary.online || !summary.renameFlagged) {
    return;
  }
  asked = YES;
  XEGamertagForm* form = [[[XEGamertagForm alloc] initWithMode:XEGamertagModeFlagged
                                                   currentName:summary.gamertag
                                                          done:nil] autorelease];
  PresentForm(presenter, form, NO);
}

// Incoming sign-in approvals, shown wherever the app is.
void xe_ios_present_incoming_approval_if_any(UIViewController* presenter) {
  XeniaLiveIncomingApproval* approval = xe_ios_live_account_service().pendingApproval;
  if (!approval || !xe_ios_live_account_service().summary.online) {
    return;
  }
  UIViewController* top = TopOf(presenter);
  if ([top isKindOfClass:[UINavigationController class]] &&
      [((UINavigationController*)top).viewControllers.firstObject isKindOfClass:[XEIncomingForm class]]) {
    return;  // Already up.
  }
  PresentForm(presenter, [[[XEIncomingForm alloc] initWithApproval:approval] autorelease], YES);
}

void xe_ios_observe_incoming_approvals(UIViewController* (^presenter)(void)) {
  static id observer = nil;
  if (observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:observer];
  }
  UIViewController* (^provider)(void) = [[presenter copy] autorelease];
  observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:XeniaLiveIncomingApprovalNotification
                  object:nil
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification* __unused note) {
                UIViewController* host = provider ? provider() : nil;
                if (host) {
                  xe_ios_present_incoming_approval_if_any(host);
                }
              }];
  [observer retain];
}

void xe_ios_present_copy_profile(UIViewController* presenter) {
  PresentForm(presenter, [[[XECopyProfileForm alloc] init] autorelease], NO);
}

UIImage* xe_ios_qr_image(NSString* text, CGFloat side) { return QRImage(text, side); }

void xe_ios_present_code_scanner(UIViewController* presenter, NSString* title, NSString* hint,
                                 NSString* _Nullable test_text, BOOL (^on_text)(NSString* text)) {
  PresentForm(presenter,
              [[[XEScanForm alloc] initWithTitle:title hint:hint testText:test_text onText:on_text]
                  autorelease],
              NO);
}
