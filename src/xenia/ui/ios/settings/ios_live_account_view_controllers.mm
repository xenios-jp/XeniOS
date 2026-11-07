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

#import "xenia/ui/ios/shared/ios_live_account.h"
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
    return _headerText.length ? [_header heightForWidth:tableView.bounds.size.width] : 12;
  }
  return UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView*)tableView heightForFooterInSection:(NSInteger)section {
  if (section == 0) {
    return _footerText.length ? [_footer heightForWidth:tableView.bounds.size.width] : 12;
  }
  return UITableViewAutomaticDimension;
}

@end

namespace {

// A form in the Profile sheet's frame: a form sheet in a navigation controller. |modal| blocks
// swiping it away.
void PresentForm(UIViewController* presenter, XEForm* form, BOOL modal) {
  UINavigationController* navigation =
      [[[UINavigationController alloc] initWithRootViewController:form] autorelease];
  navigation.modalPresentationStyle = UIModalPresentationFormSheet;
  // The sheet's own material follows the presented controller's style, so the glass reads dark
  // at every detent.
  navigation.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  navigation.view.backgroundColor = [XeniaTheme glassSheet];
  navigation.sheetPresentationController.detents = @[
    [UISheetPresentationControllerDetent mediumDetent],
    [UISheetPresentationControllerDetent largeDetent]
  ];
  navigation.modalInPresentation = modal;
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
      [self setHeaderText:NSLocalizedString(@"This is how friends find you online. Your saves and "
                                            @"achievements are kept.",
                                            @"Gamertag sheet: text above the field")];
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
                                                ? NSLocalizedString(@"Go Online",
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
  __unsafe_unretained XEGamertagForm* unsafe_self = self;
  NSString* text = [[_field.text copy] autorelease];
  [xe_ios_live_account_service()
      checkGamertag:text
         completion:^(XeniaLiveNameResult* result) {
           if (generation != unsafe_self->_generation) {
             return;
           }
           [unsafe_self applyCheck:result forName:text];
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
  __unsafe_unretained XEGamertagForm* unsafe_self = self;
  void (^finish)(XeniaLiveNameResult*) = ^(XeniaLiveNameResult* result) {
    [unsafe_self claimFinished:result name:name];
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
                          ? NSLocalizedString(@"Save this code. It’s the only way back into your "
                                              @"account if you lose every device. We show it once.",
                                              @"Recovery code sheet: text above the code")
                          : NSLocalizedString(@"This is your new code. The old one no longer works. "
                                              @"We show it once.",
                                              @"Recovery code sheet: text above the code")];
  // No ✕: it stays until the code is saved.
  _doneItem = [self addDoneButtonWithTitle:NSLocalizedString(@"I’ve Saved It",
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
  content.text = NSLocalizedString(@"I saved it somewhere safe", @"Recovery code sheet: switch");
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
  share.popoverPresentationController.sourceView = [tableView cellForRowAtIndexPath:indexPath];
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

// ---- Link another device: the code, with a countdown ------------------------------------------

@interface XELinkCodeForm : XEForm
@end

@implementation XELinkCodeForm {
  NSString* _code;
  NSDate* _expires;
  NSTimer* _timer;
  BOOL _expired;
  BOOL _failed;
}

- (void)dealloc {
  [_timer invalidate];
  [_code release];
  [_expires release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Link Another Device", @"Link code sheet title");
  [self setHeaderText:NSLocalizedString(@"On the new device, choose “Use an Existing Account”, then "
                                        @"“Enter Link Code”.",
                                        @"Link code sheet: text above the code")];
  [self addCloseButton];
  [self load];
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  [_timer invalidate];
  _timer = nil;
}

- (NSString*)note {
  return NSLocalizedString(@"Both devices will share your gamertag, friends and blocks. Saves stay "
                           @"on each device.",
                           @"Link code sheet: note under the code");
}

- (void)load {
  _expired = NO;
  _failed = NO;
  [_code release];
  _code = nil;
  [self setFooterText:NSLocalizedString(@"Getting a code…", @"Link code sheet: while waiting")
                color:nil];
  [self.tableView reloadData];
  __unsafe_unretained XELinkCodeForm* unsafe_self = self;
  [xe_ios_live_account_service()
      createLinkCodeWithCompletion:^(NSString* code, NSDate* expires, NSError* __unused error) {
        if (!code.length) {
          unsafe_self->_failed = YES;
          [unsafe_self setFooterText:NSLocalizedString(@"Can’t reach XeniOS. Try again.",
                                                       @"Link code sheet: error")
                               color:[UIColor systemRedColor]];
          [unsafe_self.tableView reloadData];
          return;
        }
        unsafe_self->_code = [code copy];
        [unsafe_self->_expires release];
        unsafe_self->_expires = [expires retain];
        [unsafe_self->_timer invalidate];
        unsafe_self->_timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                               target:unsafe_self
                                                             selector:@selector(tick)
                                                             userInfo:nil
                                                              repeats:YES];
        [unsafe_self.tableView reloadData];
        [unsafe_self tick];
      }];
}

- (void)tick {
  const NSInteger left = (NSInteger)ceil(_expires.timeIntervalSinceNow);
  if (left <= 0) {
    [_timer invalidate];
    _timer = nil;
    _expired = YES;
    [self setFooterText:NSLocalizedString(@"This code has expired.", @"Link code sheet: expired")
                  color:[UIColor systemOrangeColor]];
    [self.tableView reloadData];
    return;
  }
  [self setFooterText:[NSString stringWithFormat:NSLocalizedString(
                                                     @"Expires in %1$ld:%2$02ld.\n%3$@",
                                                     @"Link code sheet: countdown, then a note; "
                                                     @"minutes, seconds, note"),
                                                 (long)(left / 60), (long)(left % 60), [self note]]
                color:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return (_expired || _failed) ? 2 : 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  return 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 1) {
    return ActionCell(NSLocalizedString(@"Get a New Code", @"Link code sheet: row"));
  }
  return CodeCell((_code.length && !_expired) ? _code : @"········", 40,
                  _expired ? [XeniaTheme textMuted] : [XeniaTheme textPrimary], 28);
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 1) {
    [self load];
  }
}

@end

// ---- Use an existing account / Recover account: one field for either code ---------------------

@interface XEEnterCodeForm : XEForm <UITextFieldDelegate>
- (instancetype)initRecovering:(BOOL)recovering;
@end

@implementation XEEnterCodeForm {
  BOOL _recovering;
  UITextField* _field;
  UIButton* _doneItem;
  BOOL _busy;
}

- (instancetype)initRecovering:(BOOL)recovering {
  if ((self = [super init])) {
    _recovering = recovering;
  }
  return self;
}

- (void)dealloc {
  [_field release];
  [_doneItem release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  [self addCloseButton];
  _doneItem = [[self addDoneButtonWithTitle:_recovering
                                                ? NSLocalizedString(@"Recover",
                                                                    @"Code sheet: button")
                                                : NSLocalizedString(@"Link Device",
                                                                    @"Code sheet: button")]
      retain];
  _field = [[UITextField alloc] init];
  _field.delegate = self;
  _field.textAlignment = NSTextAlignmentCenter;
  _field.textColor = [XeniaTheme textPrimary];
  _field.font = [UIFont monospacedSystemFontOfSize:24 weight:UIFontWeightBold];
  _field.autocapitalizationType = UITextAutocapitalizationTypeAllCharacters;
  _field.autocorrectionType = UITextAutocorrectionTypeNo;
  _field.spellCheckingType = UITextSpellCheckingTypeNo;
  _field.returnKeyType = UIReturnKeyGo;
  _field.adjustsFontSizeToFitWidth = YES;
  _field.minimumFontSize = 16;
  _field.clearButtonMode = UITextFieldViewModeWhileEditing;
  [self applyMode];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [_field becomeFirstResponder];
}

- (NSString*)recoverNote {
  return _recovering ? NSLocalizedString(@"Lost the code too? Nothing can recover the account.",
                                         @"Code sheet: note under the field")
                     : nil;
}

- (void)applyMode {
  self.title = _recovering ? NSLocalizedString(@"Recover Account", @"Code sheet title")
                           : NSLocalizedString(@"Existing Account", @"Code sheet title");
  [self setHeaderText:_recovering
                          ? NSLocalizedString(@"Enter the recovery code you saved. Your gamertag, "
                                              @"friends and blocks come back to this device.",
                                              @"Code sheet: text above the field")
                          : NSLocalizedString(@"Enter the code from your other device. Open Link "
                                              @"Another Device there.",
                                              @"Code sheet: text above the field")];
  _field.attributedPlaceholder = [[[NSAttributedString alloc]
      initWithString:_recovering ? @"XXXX-XXXX-XXXX" : @"XXXX-XXXX"
          attributes:@{NSForegroundColorAttributeName : [XeniaTheme textMuted]}] autorelease];
  _field.text = @"";
  [self setFooterText:[self recoverNote] color:nil];
  _doneItem.enabled = NO;
  [self.tableView reloadData];
  [_field becomeFirstResponder];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  return 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath* __unused)indexPath {
  return FieldCell(_field);
}

- (BOOL)textField:(UITextField*)field
    shouldChangeCharactersInRange:(NSRange)range
                replacementString:(NSString*)string {
  NSString* next = [field.text stringByReplacingCharactersInRange:range withString:string];
  const NSUInteger limit = _recovering ? 12 : 8;
  field.text = FormattedCode(next, limit);
  [self setFooterText:[self recoverNote] color:nil];
  _doneItem.enabled = !_busy && CodeLength(field.text) == limit;
  return NO;
}

- (BOOL)textFieldShouldReturn:(UITextField* __unused)field {
  [self doneTapped];
  return NO;
}

- (void)doneTapped {
  if (!_doneItem.enabled) {
    return;
  }
  _busy = YES;
  XESetFormCommitBusy(self, YES);
  __unsafe_unretained XEEnterCodeForm* unsafe_self = self;
  void (^finish)(XeniaLiveLinkStatus) = ^(XeniaLiveLinkStatus status) {
    unsafe_self->_busy = NO;
    XESetFormCommitBusy(unsafe_self, NO);
    if (status == XeniaLiveLinkStatusLinked) {
      [unsafe_self dismissViewControllerAnimated:YES completion:nil];
      return;
    }
    NSString* message = LinkMessage(status);
    [unsafe_self setFooterText:message color:[UIColor systemRedColor]];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, message);
  };
  if (_recovering) {
    [xe_ios_live_account_service() recoverWithCode:_field.text completion:finish];
  } else {
    [xe_ios_live_account_service() linkWithCode:_field.text completion:finish];
  }
}

@end

// ---- Saves between profiles on this device ----------------------------------------------

@interface XEShareProfileViewController : XESheetTableViewController
- (instancetype)initWithProfile:(XeniaLiveLocalProfile*)profile;
@end

@implementation XEShareProfileViewController {
  XeniaLiveLocalProfile* _profile;
  NSArray<XeniaLiveSaveGame*>* _games;
  BOOL _busy;
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
  return 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? 1 : (NSInteger)_games.count;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return section == 1 && _games.count ? NSLocalizedString(@"Keep shared", @"Share saves: header") : nil;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (section == 0) {
    return NSLocalizedString(@"A one-time copy. If both profiles have a save, the newer one is kept.",
                             @"Share saves: footer under Copy");
  }
  return _games.count ? [NSString stringWithFormat:NSLocalizedString(
                                                       @"Shared games use one set of saves for both profiles. "
                                                       @"%@’s old saves for them are kept as a backup.",
                                                       @"Share saves: footer; %@ is a gamertag"),
                                                   _profile.gamertag]
                      : NSLocalizedString(@"No games have saves yet.", @"Share saves: empty");
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 0) {
    return ActionCell([NSString stringWithFormat:NSLocalizedString(@"Copy All Saves to %@…",
                                                                   @"Share saves: row; %@ is a gamertag"),
                                                 _profile.gamertag]);
  }
  XeniaLiveSaveGame* game = _games[(NSUInteger)indexPath.row];
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = game.name;
  content.secondaryText = SizeText(game.bytes);
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

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section != 0 || _busy) {
    return;
  }
  _busy = YES;
  __unsafe_unretained XEShareProfileViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      copySavesToProfile:_profile.profileId
              completion:^(NSUInteger copied, NSUInteger kept, NSError* error) {
                unsafe_self->_busy = NO;
                NSString* message =
                    error ? error.localizedDescription
                          : [NSString stringWithFormat:NSLocalizedString(
                                                           @"Copied %lu saves to %@. %lu newer saves there were "
                                                           @"kept.",
                                                           @"Share saves: result; counts and a gamertag"),
                                                       (unsigned long)copied, unsafe_self->_profile.gamertag,
                                                       (unsigned long)kept];
                ShowAlert(unsafe_self,
                          error ? NSLocalizedString(@"Couldn’t Copy", @"Share saves: alert title")
                                : NSLocalizedString(@"Saves Copied", @"Share saves: alert title"),
                          message);
              }];
}

@end

@interface XEShareHubViewController : XESheetTableViewController
@end

@implementation XEShareHubViewController {
  NSArray<XeniaLiveLocalProfile*>* _others;
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
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Share Saves", @"Share saves title");
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
  return section == 1 ? NSLocalizedString(@"Or one profile", @"Share saves: header") : nil;
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
    content.text = NSLocalizedString(@"Share Across All Profiles", @"Share saves: switch");
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
  content.text = profile.gamertag;
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
      alertControllerWithTitle:NSLocalizedString(@"Share Saves Across All Profiles?",
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

// ---- Export profile --------------------------------------------------------------------------

@interface XEExportForm : XEForm <UIDocumentPickerDelegate>
@end

@implementation XEExportForm {
  XeniaLiveExportOptions* _options;
  NSArray<NSString*>* _sizes;
  BOOL _sharing;
}

- (void)dealloc {
  [_options release];
  [_sizes release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Move Profile", @"Export sheet title");
  [self setHeaderText:NSLocalizedString(@"Saves, achievements and settings as one file. Saves don’t "
                                        @"sync between devices.",
                                        @"Export sheet: text above the list")];
  [self addCloseButton];
  _options = [[XeniaLiveExportOptions alloc] init];
  _options.saves = _options.achievements = _options.settings = YES;
  _sizes = [@[ @"", @"", @"" ] retain];
  __unsafe_unretained XEExportForm* unsafe_self = self;
  [xe_ios_live_account_service()
      exportSizesWithCompletion:^(uint64_t saves, uint64_t achievements, uint64_t settings) {
        [unsafe_self->_sizes release];
        unsafe_self->_sizes = [@[ SizeText(saves), SizeText(achievements), SizeText(settings) ] retain];
        [unsafe_self.tableView reloadData];
      }];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 4;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? 3 : 1;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return section == 3 ? NSLocalizedString(@"On this device", @"Export sheet: header") : nil;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  return section == 3 ? NSLocalizedString(@"Copy saves to another profile here, or keep them shared.",
                                          @"Export sheet: footer")
                      : nil;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 3) {
    UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                    reuseIdentifier:nil] autorelease];
    UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
    content.text = NSLocalizedString(@"Share Saves with Profiles", @"Export sheet: row");
    content.image = Glyph(@"person.2");
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
  }
  if (indexPath.section == 2) {
    return ActionCell(NSLocalizedString(@"Import from File…", @"Export sheet: row"));
  }
  if (indexPath.section == 1) {
    UITableViewCell* cell = ActionCell(NSLocalizedString(@"Export via AirDrop or Files…",
                                                         @"Export sheet: row"));
    cell.userInteractionEnabled = (_options.saves || _options.achievements || _options.settings) && !_sharing;
    cell.contentView.alpha = cell.userInteractionEnabled ? 1.0 : 0.4;
    return cell;
  }
  NSArray* rows = @[
    @[ NSLocalizedString(@"Game Saves", @"Export sheet: row"), @"externaldrive.fill", @(0x0A84FF) ],
    @[ NSLocalizedString(@"Achievements", @"Export sheet: row"), @"trophy.fill", @(0xFF9F0A) ],
    @[ NSLocalizedString(@"Settings", @"Export sheet: row"), @"gearshape.fill", @(0x8E8E93) ],
  ];
  const NSUInteger index = (NSUInteger)indexPath.row;
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = rows[index][0];
  content.secondaryText = _sizes[index].length ? _sizes[index] : nil;
  content.image = Glyph(rows[index][1]);
  cell.contentConfiguration = content;
  UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
  toggle.on = index == 0 ? _options.saves : index == 1 ? _options.achievements : _options.settings;
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
    default:
      _options.settings = toggle.on;
      break;
  }
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:1]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == 3) {
    [self.navigationController pushViewController:[[[XEShareHubViewController alloc] init] autorelease]
                                         animated:YES];
    return;
  }
  if (indexPath.section == 2) {
    UTType* type = [UTType typeWithIdentifier:@"jp.xenios.xenios.profile"];
    UIDocumentPickerViewController* picker = [[[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[ type ?: UTTypeItem ]
                            asCopy:YES] autorelease];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
    return;
  }
  if (indexPath.section != 1 || _sharing) {
    return;
  }
  _sharing = YES;
  UIView* source = [tableView cellForRowAtIndexPath:indexPath];
  __unsafe_unretained XEExportForm* unsafe_self = self;
  [xe_ios_live_account_service()
      exportProfileWithOptions:_options
                    completion:^(NSURL* file, NSError* error) {
                      unsafe_self->_sharing = NO;
                      if (!file) {
                        ShowAlert(unsafe_self,
                                  NSLocalizedString(@"Couldn’t Export", @"Export sheet: alert title"),
                                  error.localizedDescription);
                        return;
                      }
                      UIActivityViewController* share = [[[UIActivityViewController alloc]
                          initWithActivityItems:@[ file ]
                          applicationActivities:nil] autorelease];
                      share.popoverPresentationController.sourceView = source;
                      share.completionWithItemsHandler =
                          ^(UIActivityType __unused type, BOOL __unused completed,
                            NSArray* __unused items, NSError* __unused e) {
                            [[NSFileManager defaultManager] removeItemAtURL:file error:nil];
                          };
                      [unsafe_self presentViewController:share animated:YES completion:nil];
                    }];
}

- (void)documentPicker:(UIDocumentPickerViewController* __unused)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  if (urls.firstObject) {
    xe_ios_handle_opened_profile_file(urls.firstObject, self);
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
  auto in_eye = [&](size_t x, size_t y) {
    return (x < 8 && y < 8) || (x >= n - 8 && y < 8) || (x < 8 && y >= n - 8);
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
        if (x + 3 >= mid && x <= mid + 3 && y + 3 >= mid && y <= mid + 3) {
          continue;  // Under the mark.
        }
        CGRect dot = CGRectMake((CGFloat)x * unit + unit * 0.06, (CGFloat)y * unit + unit * 0.06,
                                unit * 0.88, unit * 0.88);
        [[UIBezierPath bezierPathWithRoundedRect:dot cornerRadius:unit * 0.44] fill];
      }
    }
    const size_t eyes[3][2] = {{0, 0}, {n - 7, 0}, {0, n - 7}};
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
    CGRect mark = CGRectMake(((CGFloat)mid - 2.5) * unit, ((CGFloat)mid - 2.5) * unit, unit * 5, unit * 5);
    [[UIColor whiteColor] setFill];
    [[UIBezierPath bezierPathWithRoundedRect:CGRectInset(mark, -unit * 0.4, -unit * 0.4)
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

// ---- The QR code page on the new device --------------------------------------------------

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

@interface XEQRForm : XEForm
@end

@implementation XEQRForm {
  XEQRHero* _hero;
  XeniaLiveApprovalRequest* _request;
  NSTimer* _timer;
  NSUInteger _generation;
  BOOL _done;
}

- (void)dealloc {
  [_timer invalidate];
  [_hero release];
  [_request release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Link Device", @"QR sheet title");
  [self addCloseButton];
  _hero = [[XEQRHero alloc] initWithFrame:CGRectMake(0, 0, 320, 400)];
  _hero.title.text = NSLocalizedString(@"Scan with your other device", @"QR sheet: heading");
  _hero.subtitle.text = NSLocalizedString(@"On it, open Online Account, then Link Another Device, "
                                          @"then Scan QR Code.",
                                          @"QR sheet: how to scan");
  self.tableView.tableHeaderView = _hero;
  [self fetch];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  CGRect frame = _hero.frame;
  frame.size.width = self.tableView.bounds.size.width;
  const CGFloat height = [_hero systemLayoutSizeFittingSize:CGSizeMake(frame.size.width, UILayoutFittingCompressedSize.height)
                              withHorizontalFittingPriority:UILayoutPriorityRequired
                                    verticalFittingPriority:UILayoutPriorityFittingSizeLevel].height;
  if (fabs(frame.size.height - height) > 0.5 || fabs(_hero.frame.size.width - frame.size.width) > 0.5) {
    frame.size.height = height;
    _hero.frame = frame;
    self.tableView.tableHeaderView = _hero;
  }
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  if (self.isBeingDismissed || self.navigationController.isBeingDismissed || !self.parentViewController) {
    [self stop];
  }
}

- (void)stop {
  [_timer invalidate];
  _timer = nil;
  ++_generation;
  if (_request && !_done) {
    [xe_ios_live_account_service() cancelApproval:_request];
  }
}

// A new code now, and again whenever it runs out.
- (void)fetch {
  const NSUInteger generation = ++_generation;
  __unsafe_unretained XEQRForm* unsafe_self = self;
  [xe_ios_live_account_service()
      requestQRLinkWithCompletion:^(XeniaLiveApprovalRequest* request, XeniaLiveApprovalStatus status) {
        if (generation != unsafe_self->_generation) {
          return;
        }
        if (!request.qrPayload.length) {
          unsafe_self->_hero.refresh.text = status == XeniaLiveApprovalStatusUnreachable
                                                ? NSLocalizedString(@"Can’t reach XeniOS", @"QR sheet: error")
                                                : nil;
          return;
        }
        [unsafe_self->_request release];
        unsafe_self->_request = [request retain];
        unsafe_self->_hero.code.image = QRImage(request.qrPayload, 176);
        [unsafe_self->_timer invalidate];
        unsafe_self->_timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                               target:unsafe_self
                                                             selector:@selector(tick)
                                                             userInfo:nil
                                                              repeats:YES];
        [unsafe_self tick];
        [xe_ios_live_account_service()
            waitForApproval:request
                 completion:^(XeniaLiveApprovalStatus result) {
                   if (generation != unsafe_self->_generation) {
                     return;
                   }
                   [unsafe_self finished:result];
                 }];
      }];
}

- (void)tick {
  if (_request.expires.timeIntervalSinceNow <= 0) {
    [_timer invalidate];
    _timer = nil;
    return;
  }
  _hero.refresh.text = [NSString stringWithFormat:NSLocalizedString(@"Refreshes in %@",
                                                                    @"QR sheet: countdown; %@ is m:ss"),
                                                  CountdownText(_request.expires)];
}

- (void)finished:(XeniaLiveApprovalStatus)status {
  [_timer invalidate];
  _timer = nil;
  switch (status) {
    case XeniaLiveApprovalStatusApproved:
      _done = YES;
      _hero.title.text = NSLocalizedString(@"Signed in", @"Sign-in approval: result");
      _hero.refresh.text = nil;
      _hero.code.alpha = 0.15;
      UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, _hero.title.text);
      [self performSelector:@selector(closeTapped) withObject:nil afterDelay:1.1];
      break;
    case XeniaLiveApprovalStatusTimedOut:
      [self fetch];  // A fresh code.
      break;
    case XeniaLiveApprovalStatusCancelled:
      break;
    default:
      _hero.refresh.text = NSLocalizedString(@"That didn’t go through. Try again.",
                                             @"Sign-in approval: neutral failure");
      [self performSelector:@selector(fetch) withObject:nil afterDelay:2.0];
      break;
  }
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  return 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath* __unused)indexPath {
  UITableViewCell* cell = ActionCell(NSLocalizedString(@"Enter a code instead", @"QR sheet: row"));
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  [self stop];
  [self.navigationController pushViewController:[[[XEEnterCodeForm alloc] initRecovering:NO] autorelease]
                                       animated:YES];
}

@end

// ---- Approve on another device: the new device waits ------------------------------------

@interface XEApproveForm : XEForm <UITextFieldDelegate>
@end

typedef NS_ENUM(NSInteger, XEApproveState) {
  XEApproveStateEntering = 0,
  XEApproveStateWaiting,
  XEApproveStateSignedIn,
  XEApproveStateFailed,
};

@implementation XEApproveForm {
  UITextField* _field;
  UIButton* _doneItem;
  XEApproveState _state;
  XeniaLiveApprovalRequest* _request;
  NSString* _name;
  NSTimer* _timer;
  NSUInteger _generation;
}

- (void)dealloc {
  [_timer invalidate];
  [_field release];
  [_doneItem release];
  [_request release];
  [_name release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  [self addCloseButton];
  _doneItem = [[self addDoneButtonWithTitle:NSLocalizedString(@"Ask for Approval",
                                                              @"Approval sheet: button")] retain];
  _field = [[UITextField alloc] init];
  _field.delegate = self;
  _field.placeholder = NSLocalizedString(@"Gamertag", @"Gamertag sheet: field placeholder");
  _field.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  _field.adjustsFontForContentSizeCategory = YES;
  _field.textColor = [XeniaTheme textPrimary];
  _field.autocapitalizationType = UITextAutocapitalizationTypeWords;
  _field.autocorrectionType = UITextAutocorrectionTypeNo;
  _field.spellCheckingType = UITextSpellCheckingTypeNo;
  _field.returnKeyType = UIReturnKeyContinue;
  [_field addTarget:self action:@selector(textChanged) forControlEvents:UIControlEventEditingChanged];
  [self showState:XEApproveStateEntering];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  if (_state == XEApproveStateEntering) {
    [_field becomeFirstResponder];
  }
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  if (!self.parentViewController || self.navigationController.isBeingDismissed) {
    [self stopWaiting];
  }
}

- (void)stopWaiting {
  [_timer invalidate];
  _timer = nil;
  ++_generation;
  if (_request && _state == XEApproveStateWaiting) {
    [xe_ios_live_account_service() cancelApproval:_request];
  }
}

- (void)textChanged {
  const BOOL ok = _field.text.length > 0 && xe_ios_gamertag_local_error(_field.text) == nil;
  _doneItem.enabled = ok;
}

- (BOOL)textFieldShouldReturn:(UITextField* __unused)field {
  [self doneTapped];
  return NO;
}

- (void)showState:(XEApproveState)state {
  _state = state;
  self.title = NSLocalizedString(@"Approve on Another Device", @"Approval sheet title");
  switch (state) {
    case XEApproveStateEntering:
      [self setHeaderText:NSLocalizedString(@"Enter your gamertag. We’ll ask your other devices to "
                                            @"approve this one.",
                                            @"Approval sheet: text above the field")];
      [self setFooterText:nil color:nil];
      XESetFormCommitBusy(self, NO);
      XESetFormCommitHidden(self, NO);
      [self textChanged];
      break;
    case XEApproveStateWaiting:
      [self setHeaderText:NSLocalizedString(@"Open XeniOS on your iPhone, iPad or Mac and approve "
                                            @"this number.",
                                            @"Approval sheet: text above the number")];
      XESetFormCommitHidden(self, YES);
      [self tick];
      break;
    case XEApproveStateSignedIn:
      [self setHeaderText:nil];
      [self setFooterText:nil color:nil];
      XESetFormCommitHidden(self, YES);
      break;
    case XEApproveStateFailed:
      [self setHeaderText:nil];
      [self setFooterText:NSLocalizedString(@"That didn’t go through. Check the gamertag and try "
                                            @"again.",
                                            @"Approval sheet: neutral failure")
                    color:nil];
      XESetFormCommitHidden(self, YES);
      break;
  }
  [self.tableView reloadData];
}

- (void)doneTapped {
  if (_state != XEApproveStateEntering || !_doneItem.enabled) {
    return;
  }
  [_field resignFirstResponder];
  [_name release];
  _name = [_field.text copy];
  const NSUInteger generation = ++_generation;
  XESetFormCommitBusy(self, YES);
  __unsafe_unretained XEApproveForm* unsafe_self = self;
  [xe_ios_live_account_service()
      requestApprovalForGamertag:_name
                      completion:^(XeniaLiveApprovalRequest* request, XeniaLiveApprovalStatus status) {
                        if (generation != unsafe_self->_generation) {
                          return;
                        }
                        if (!request) {
                          [unsafe_self failedWithStatus:status];
                          return;
                        }
                        [unsafe_self->_request release];
                        unsafe_self->_request = [request retain];
                        [unsafe_self showState:XEApproveStateWaiting];
                        unsafe_self->_timer =
                            [NSTimer scheduledTimerWithTimeInterval:1.0
                                                             target:unsafe_self
                                                           selector:@selector(tick)
                                                           userInfo:nil
                                                            repeats:YES];
                        [xe_ios_live_account_service()
                            waitForApproval:request
                                 completion:^(XeniaLiveApprovalStatus result) {
                                   if (generation != unsafe_self->_generation) {
                                     return;
                                   }
                                   [unsafe_self failedOrDone:result];
                                 }];
                      }];
}

- (void)failedWithStatus:(XeniaLiveApprovalStatus)status {
  (void)status;
  [self showState:XEApproveStateFailed];
}

- (void)failedOrDone:(XeniaLiveApprovalStatus)status {
  [_timer invalidate];
  _timer = nil;
  if (status == XeniaLiveApprovalStatusApproved) {
    [self showState:XEApproveStateSignedIn];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification,
                                    NSLocalizedString(@"Signed in", @"Sign-in approval: result"));
    [self performSelector:@selector(closeTapped) withObject:nil afterDelay:1.2];
    return;
  }
  if (status == XeniaLiveApprovalStatusCancelled) {
    return;
  }
  // Denied, timed out and unreachable read the same: nothing says whether the name exists.
  [self showState:XEApproveStateFailed];
  UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification,
                                  NSLocalizedString(@"That didn’t go through. Check the gamertag and try again.",
                                                    @"Approval sheet: neutral failure"));
}

- (void)tick {
  if (_state != XEApproveStateWaiting) {
    return;
  }
  [self setFooterText:[NSString stringWithFormat:NSLocalizedString(@"Signing in as %1$@\n%2$@ left",
                                                                   @"Approval sheet: under the number; "
                                                                   @"gamertag, then m:ss"),
                                                 _name, CountdownText(_request.expires)]
                color:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return _state == XEApproveStateSignedIn ? 1 : 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  return 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 1) {
    return ActionCell(_state == XEApproveStateWaiting ? NSLocalizedString(@"Cancel", @"Approval sheet: row")
                                                      : NSLocalizedString(@"Try Again", @"Approval sheet: row"));
  }
  switch (_state) {
    case XEApproveStateEntering:
      return FieldCell(_field);
    case XEApproveStateWaiting:
      return CodeCell(_request.matchNumber ?: @"", 64, [XeniaTheme textPrimary], 26);
    case XEApproveStateSignedIn: {
      UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                      reuseIdentifier:nil] autorelease];
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
      UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
      content.text = NSLocalizedString(@"Signed in", @"Sign-in approval: result");
      content.textProperties.alignment = UIListContentTextAlignmentCenter;
      content.image = [UIImage systemImageNamed:@"checkmark.circle.fill"];
      content.imageProperties.tintColor = [UIColor systemGreenColor];
      cell.contentConfiguration = content;
      return cell;
    }
    case XEApproveStateFailed:
      return FieldCell(_field);
  }
  return nil;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section != 1) {
    return;
  }
  if (_state == XEApproveStateWaiting) {
    [self stopWaiting];
    [self closeTapped];
  } else {
    [self showState:XEApproveStateEntering];
    [_field becomeFirstResponder];
  }
}

@end

// ---- The new device's way in: four ways to sign in --------------------------------------

@interface XEExistingForm : XEForm
@end

@implementation XEExistingForm

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Existing Account", @"Existing account sheet title");
  [self setHeaderText:NSLocalizedString(@"Bring your gamertag, friends and blocks to this device. "
                                        @"Saves stay on each device.",
                                        @"Existing account sheet: text above the list")];
  [self addCloseButton];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  return 4;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  NSArray* rows = @[
    @[ NSLocalizedString(@"Approve on Another Device", @"Existing account row"),
       NSLocalizedString(@"Use your iPhone, iPad or Mac", @"Existing account row detail"),
       @"checkmark.shield.fill", @(0x30D158) ],
    @[ NSLocalizedString(@"Scan QR Code", @"Existing account row"),
       NSLocalizedString(@"Show a code for your other device", @"Existing account row detail"),
       @"qrcode", @(0x0A84FF) ],
    @[ NSLocalizedString(@"Enter Link Code", @"Existing account row"), @"", @"link", @(0x5E5CE6) ],
    @[ NSLocalizedString(@"Enter Recovery Code", @"Existing account row"), @"", @"key.fill", @(0xFF9F0A) ],
  ];
  NSArray* row = rows[(NSUInteger)indexPath.row];
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = row[0];
  content.secondaryText = [row[1] length] ? row[1] : nil;
  content.image = Glyph(row[2]);
  cell.contentConfiguration = content;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  XEForm* next = nil;
  switch (indexPath.row) {
    case 0:
      next = [[[XEApproveForm alloc] init] autorelease];
      break;
    case 1:
      next = [[[XEQRForm alloc] init] autorelease];
      break;
    case 2:
      next = [[[XEEnterCodeForm alloc] initRecovering:NO] autorelease];
      break;
    default:
      next = [[[XEEnterCodeForm alloc] initRecovering:YES] autorelease];
      break;
  }
  [self.navigationController pushViewController:next animated:YES];
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

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger __unused)section {
  return 1;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  return section == 1 ? [NSString stringWithFormat:NSLocalizedString(@"Pick the number on %@",
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

// ---- Scanning a QR code on the signed-in device ------------------------------------------

@interface XEScanViewController : UIViewController <AVCaptureMetadataOutputObjectsDelegate>
@end

@implementation XEScanViewController {
  AVCaptureSession* _session;
  AVCaptureVideoPreviewLayer* _preview;
  UILabel* _hint;
  BOOL _handling;
}

- (void)dealloc {
  [_session stopRunning];
  [_session release];
  [_preview release];
  [_hint release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Scan QR Code", @"Scanner title");
  self.view.backgroundColor = [UIColor blackColor];
  XESetSheetClose(self, self, @selector(close));
  _hint = [[UILabel alloc] init];
  xe_apply_label_font(_hint, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  _hint.textColor = [UIColor whiteColor];
  _hint.textAlignment = NSTextAlignmentCenter;
  _hint.numberOfLines = 0;
  _hint.text = NSLocalizedString(@"Point the camera at the code on your new device.", @"Scanner: hint");
  _hint.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:_hint];
  [NSLayoutConstraint activateConstraints:@[
    [_hint.leadingAnchor constraintEqualToAnchor:self.view.layoutMarginsGuide.leadingAnchor constant:12],
    [_hint.trailingAnchor constraintEqualToAnchor:self.view.layoutMarginsGuide.trailingAnchor constant:-12],
    [_hint.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-28],
  ]];
  [self startCamera];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  _preview.frame = self.view.bounds;
}

- (void)close {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)startCamera {
  AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
  if (!device) {
    [self noCamera];
    return;
  }
  const AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
  if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
    _hint.text = NSLocalizedString(@"Allow camera access for XeniOS in Settings to scan the code.",
                                   @"Scanner: camera denied");
    return;
  }
  __unsafe_unretained XEScanViewController* unsafe_self = self;
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                           completionHandler:^(BOOL granted) {
                             dispatch_async(dispatch_get_main_queue(), ^{
                               if (granted) {
                                 [unsafe_self configureSession:device];
                               } else {
                                 unsafe_self->_hint.text = NSLocalizedString(
                                     @"Allow camera access for XeniOS in Settings to scan the code.",
                                     @"Scanner: camera denied");
                               }
                             });
                           }];
}

- (void)configureSession:(AVCaptureDevice*)device {
  NSError* error = nil;
  AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
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
  _preview.frame = self.view.bounds;
  [self.view.layer insertSublayer:_preview atIndex:0];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    [_session startRunning];
  });
}

// No camera (the Simulator): the fake service can still hand over a test code.
- (void)noCamera {
  _hint.text = NSLocalizedString(@"This device has no camera.", @"Scanner: no camera");
  if (xe_ios_live_account_service_is_stub()) {
    UIButton* test = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
    config.title = @"Use Test Code";
    config.baseBackgroundColor = [XeniaTheme accent];
    config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    test.configuration = config;
    test.translatesAutoresizingMaskIntoConstraints = NO;
    [test addTarget:self action:@selector(useTestCode) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:test];
    [NSLayoutConstraint activateConstraints:@[
      [test.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
      [test.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
    ]];
  }
}

- (void)useTestCode {
  [self handlePayload:@"xenios://link?r=test"];
}

- (void)captureOutput:(AVCaptureOutput* __unused)output
    didOutputMetadataObjects:(NSArray<__kindof AVMetadataObject*>*)objects
              fromConnection:(AVCaptureConnection* __unused)connection {
  for (AVMetadataObject* object in objects) {
    if ([object isKindOfClass:[AVMetadataMachineReadableCodeObject class]]) {
      NSString* value = ((AVMetadataMachineReadableCodeObject*)object).stringValue;
      if (value.length) {
        [self handlePayload:value];
        return;
      }
    }
  }
}

- (void)handlePayload:(NSString*)payload {
  if (_handling) {
    return;
  }
  _handling = YES;
  [_session stopRunning];
  __unsafe_unretained XEScanViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      beginApprovalFromQRPayload:payload
                      completion:^(XeniaLiveIncomingApproval* approval, XeniaLiveLinkStatus status) {
                        if (!approval) {
                          unsafe_self->_hint.text = status == XeniaLiveLinkStatusUnreachable
                                                        ? NSLocalizedString(@"Can’t reach XeniOS. Check your connection.",
                                                                            @"Scanner: error")
                                                        : NSLocalizedString(@"That isn’t a XeniOS code. Try again.",
                                                                            @"Scanner: wrong code");
                          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)),
                                         dispatch_get_main_queue(), ^{
                                           unsafe_self->_handling = NO;
                                           unsafe_self->_hint.text = NSLocalizedString(
                                               @"Point the camera at the code on your new device.",
                                               @"Scanner: hint");
                                           dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                                             [unsafe_self->_session startRunning];
                                           });
                                         });
                          return;
                        }
                        UIViewController* presenter = unsafe_self.presentingViewController;
                        [unsafe_self dismissViewControllerAnimated:YES
                                                        completion:^{
                                                          PresentForm(presenter,
                                                                      [[[XEIncomingForm alloc] initWithApproval:approval] autorelease],
                                                                      YES);
                                                        }];
                      }];
}

@end

// ---- Devices ---------------------------------------------------------------------------------

@interface XEDevicesViewController : XESheetTableViewController
@end

@implementation XEDevicesViewController {
  NSArray<XeniaLiveDevice*>* _devices;
  BOOL _failed;
}

- (instancetype)init {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_devices release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Devices", @"Devices screen title");
  self.tableView.backgroundColor = [UIColor clearColor];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(reload)
                                               name:XeniaLiveAccountDidChangeNotification
                                             object:nil];
  [self reload];
}

- (void)reload {
  __unsafe_unretained XEDevicesViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      loadDevicesWithCompletion:^(NSArray<XeniaLiveDevice*>* devices, NSError* __unused error) {
        unsafe_self->_failed = devices == nil;
        if (devices) {
          [unsafe_self->_devices release];
          unsafe_self->_devices = [devices copy];
        }
        [unsafe_self.tableView reloadData];
      }];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? (NSInteger)_devices.count : 2;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (section == 0) {
    return _failed ? NSLocalizedString(@"Can’t load devices right now.", @"Devices screen: footer")
                   : NSLocalizedString(@"Remove a device you’ve lost. It’s signed out and can’t "
                                       @"come back without a new code.",
                                       @"Devices screen: footer");
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 1) {
    return ActionCell(indexPath.row == 0 ? NSLocalizedString(@"Show Link Code…", @"Devices screen: row")
                                         : NSLocalizedString(@"Scan QR Code…", @"Devices screen: row"));
  }
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  XeniaLiveDevice* device = _devices[(NSUInteger)indexPath.row];
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  NSString* symbol = [device.kind isEqualToString:@"ipad"]  ? @"ipad"
                     : [device.kind isEqualToString:@"mac"] ? @"laptopcomputer"
                                                            : @"iphone";
  content.text = device.thisDevice
                     ? [NSString stringWithFormat:NSLocalizedString(@"%@ (this device)",
                                                                    @"Devices screen: row; %@ is "
                                                                    @"the device name"),
                                                  device.name]
                     : device.name;
  content.secondaryText =
      !device.lastSeen
          ? NSLocalizedString(@"Active now", @"Devices screen: row detail")
          : [NSString stringWithFormat:NSLocalizedString(@"Last seen %@", @"Devices screen: row "
                                                                         @"detail; %@ is a time"),
                                       [[[[NSRelativeDateTimeFormatter alloc] init] autorelease]
                                           localizedStringForDate:device.lastSeen
                                                   relativeToDate:[NSDate date]]];
  content.image = Glyph(symbol);
  cell.contentConfiguration = content;
  if (!device.thisDevice) {
    UIButton* remove = [UIButton buttonWithType:UIButtonTypeSystem];
    [remove setTitle:NSLocalizedString(@"Remove", @"Devices screen: button") forState:UIControlStateNormal];
    remove.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    [remove setTitleColor:[UIColor systemRedColor] forState:UIControlStateNormal];
    [remove sizeToFit];
    __unsafe_unretained XEDevicesViewController* unsafe_self = self;
    [remove addAction:[UIAction actionWithHandler:^(__kindof UIAction* __unused a) {
              [unsafe_self confirmRemove:device];
            }]
        forControlEvents:UIControlEventTouchUpInside];
    cell.accessoryView = remove;
  }
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section != 1) {
    return;
  }
  if (indexPath.row == 0) {
    PresentForm(self, [[[XELinkCodeForm alloc] init] autorelease], NO);
  } else {
    UINavigationController* navigation = [[[UINavigationController alloc]
        initWithRootViewController:[[[XEScanViewController alloc] init] autorelease]] autorelease];
    navigation.modalPresentationStyle = UIModalPresentationFullScreen;
    [self presentViewController:navigation animated:YES completion:nil];
  }
}

- (void)confirmRemove:(XeniaLiveDevice*)device {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:[NSString stringWithFormat:NSLocalizedString(@"Remove %@?",
                                                                            @"Devices screen: alert "
                                                                            @"title; %@ is the "
                                                                            @"device name"),
                                                          device.name]
                       message:NSLocalizedString(@"It will be signed out and can’t come back without "
                                                 @"a new code.",
                                                 @"Devices screen: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Devices screen: alert button")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  __unsafe_unretained XEDevicesViewController* unsafe_self = self;
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Remove", @"Devices screen: alert button")
                                 style:UIAlertActionStyleDestructive
                               handler:^(UIAlertAction* __unused action) {
                                 [xe_ios_live_account_service()
                                     removeDevice:device
                                       completion:^(NSError* error) {
                                         if (error) {
                                           ShowAlert(unsafe_self,
                                                     NSLocalizedString(@"Couldn’t Remove",
                                                                       @"Devices screen: alert title"),
                                                     error.localizedDescription);
                                         }
                                         [unsafe_self reload];
                                       }];
                               }]];
  [self presentViewController:alert animated:YES completion:nil];
}

@end

// ---- The account page ------------------------------------------------------------------------

typedef NS_ENUM(NSInteger, XEAccountRow) {
  XEAccountRowGoOnline = 0,
  XEAccountRowUseExisting,
  XEAccountRowGamertag,
  XEAccountRowDevices,
  XEAccountRowLink,
  XEAccountRowScan,
  XEAccountRowRecovery,
  XEAccountRowExport,
  XEAccountRowImport,
};

@interface XEAccountViewController : XESheetTableViewController <UIDocumentPickerDelegate>
- (instancetype)initWithProfileGamertag:(NSString*)gamertag;
@end

@implementation XEAccountViewController {
  NSString* _profile;
  NSArray<NSArray<NSNumber*>*>* _sections;
  NSInteger _deviceCount;
}

- (instancetype)initWithProfileGamertag:(NSString*)gamertag {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
    _profile = [gamertag copy];
    _deviceCount = -1;
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [_profile release];
  [_sections release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Online Account", @"Account screen title");
  self.tableView.backgroundColor = [UIColor clearColor];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(reload)
                                               name:XeniaLiveAccountDidChangeNotification
                                             object:nil];
  [self reload];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self reload];
  [self refreshDevices];
}

- (void)refreshDevices {
  if (!xe_ios_live_account_service().summary.online) {
    return;
  }
  __unsafe_unretained XEAccountViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      loadDevicesWithCompletion:^(NSArray<XeniaLiveDevice*>* devices, NSError* __unused error) {
        unsafe_self->_deviceCount = devices ? (NSInteger)devices.count : -1;
        [unsafe_self.tableView reloadData];
      }];
}

- (void)reload {
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  [_sections release];
  if (summary.online) {
    _sections = [@[
      @[ @(XEAccountRowGamertag), @(XEAccountRowDevices), @(XEAccountRowRecovery) ],
      @[ @(XEAccountRowExport) ],
    ] retain];
  } else {
    _sections = [@[
      @[ @(XEAccountRowGoOnline) ],
      @[ @(XEAccountRowUseExisting), @(XEAccountRowImport) ],
    ] retain];
  }
  [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return (NSInteger)_sections.count;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  return (NSInteger)_sections[(NSUInteger)section].count;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  if (!summary.online && section == 0) {
    return NSLocalizedString(@"Friends, parties and public games. Your saves and achievements stay. "
                             @"Offline play works the same, with no account.",
                             @"Account screen: footer under Go Online");
  }
  if (summary.online && section == 1) {
    return NSLocalizedString(@"Saves and achievements don’t sync between devices. Move them as a file.",
                             @"Account screen: footer");
  }
  return nil;
}

- (XEAccountRow)rowAt:(NSIndexPath*)indexPath {
  return (XEAccountRow)_sections[(NSUInteger)indexPath.section][(NSUInteger)indexPath.row].integerValue;
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  const XEAccountRow row = [self rowAt:indexPath];
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  if (row == XEAccountRowGoOnline) {
    UITableViewCell* cell = ActionCell(NSLocalizedString(@"Go Online…", @"Account screen: row"));
    cell.accessibilityTraits |= UIAccessibilityTraitButton;
    return cell;
  }
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  switch (row) {
    case XEAccountRowGamertag: {
      content.text = summary.gamertag;
      NSString* detail;
      UIColor* detail_color = [XeniaTheme textSecondary];
      if (summary.renameFlagged) {
        detail = NSLocalizedString(@"Pick a new gamertag", @"Account screen: row detail");
        detail_color = [UIColor systemOrangeColor];
      } else if (summary.nextRename.timeIntervalSinceNow > 0) {
        detail = [NSString stringWithFormat:NSLocalizedString(@"Change again after %@",
                                                              @"Account screen: row detail; %@ is "
                                                              @"a date"),
                                            DateText(summary.nextRename)];
      } else {
        detail = NSLocalizedString(@"Tap to change", @"Account screen: row detail");
      }
      content.secondaryText = detail;
      content.secondaryTextProperties.color = detail_color;
      content.image = Glyph(@"person.text.rectangle.fill");
      break;
    }
    case XEAccountRowDevices:
      content = [UIListContentConfiguration valueCellConfiguration];
      content.text = NSLocalizedString(@"Devices", @"Account screen: row");
      content.secondaryText = _deviceCount >= 0 ? [NSString stringWithFormat:@"%ld", (long)_deviceCount] : nil;
      content.image = Glyph(@"laptopcomputer.and.iphone");
      break;
    case XEAccountRowLink:
      content.text = NSLocalizedString(@"Link Another Device", @"Account screen: row");
      content.image = Glyph(@"link");
      break;
    case XEAccountRowScan:
      content.text = NSLocalizedString(@"Scan QR Code", @"Account screen: row");
      content.image = Glyph(@"qrcode.viewfinder");
      break;
    case XEAccountRowRecovery:
      content.text = NSLocalizedString(@"New Recovery Code", @"Account screen: row");
      content.image = Glyph(@"key.fill");
      break;
    case XEAccountRowExport:
      content.text = NSLocalizedString(@"Move Profile", @"Account screen: row");
      content.image = Glyph(@"square.and.arrow.up");
      break;
    case XEAccountRowImport:
      content.text = NSLocalizedString(@"Import Profile", @"Account screen: row");
      content.image = Glyph(@"square.and.arrow.down");
      break;
    case XEAccountRowUseExisting:
      content.text = NSLocalizedString(@"Use an Existing Account", @"Account screen: row");
      content.image = Glyph(@"iphone.and.arrow.forward");
      break;
    case XEAccountRowGoOnline:
      break;
  }
  cell.contentConfiguration = content;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  switch ([self rowAt:indexPath]) {
    case XEAccountRowGoOnline:
      [self goOnline];
      break;
    case XEAccountRowUseExisting:
      PresentForm(self, [[[XEExistingForm alloc] init] autorelease], NO);
      break;
    case XEAccountRowScan: {
      UINavigationController* navigation = [[[UINavigationController alloc]
          initWithRootViewController:[[[XEScanViewController alloc] init] autorelease]] autorelease];
      navigation.modalPresentationStyle = UIModalPresentationFullScreen;
      [self presentViewController:navigation animated:YES completion:nil];
      break;
    }
    case XEAccountRowGamertag:
      [self presentRename];
      break;
    case XEAccountRowDevices:
      [self.navigationController pushViewController:[[[XEDevicesViewController alloc] init] autorelease]
                                           animated:YES];
      break;
    case XEAccountRowLink:
      PresentForm(self, [[[XELinkCodeForm alloc] init] autorelease], NO);
      break;
    case XEAccountRowRecovery:
      [self confirmNewRecoveryCode];
      break;
    case XEAccountRowExport:
      PresentForm(self, [[[XEExportForm alloc] init] autorelease], NO);
      break;
    case XEAccountRowImport:
      [self pickImportFile];
      break;
  }
}

- (void)goOnline {
  __unsafe_unretained XEAccountViewController* unsafe_self = self;
  xe_ios_present_go_online(self, _profile, ^{
    [unsafe_self reload];
    [unsafe_self refreshDevices];
  });
}

- (void)presentRename {
  XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
  __unsafe_unretained XEAccountViewController* unsafe_self = self;
  XEGamertagForm* form = [[[XEGamertagForm alloc]
      initWithMode:summary.renameFlagged ? XEGamertagModeFlagged : XEGamertagModeRename
       currentName:summary.gamertag
              done:^(XeniaLiveNameResult* __unused result) {
                [unsafe_self reload];
              }] autorelease];
  PresentForm(self, form, NO);
}

- (void)confirmNewRecoveryCode {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Create a New Recovery Code?",
                                                 @"Account screen: alert title")
                       message:NSLocalizedString(@"Your old code stops working. We show the new "
                                                 @"one once.",
                                                 @"Account screen: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Account screen: alert button")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  __unsafe_unretained XEAccountViewController* unsafe_self = self;
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Create", @"Account screen: alert button")
                                 style:UIAlertActionStyleDefault
                               handler:^(UIAlertAction* __unused action) {
                                 [xe_ios_live_account_service()
                                     createRecoveryCodeWithCompletion:^(NSString* code, NSError* error) {
                                       if (!code.length) {
                                         ShowAlert(unsafe_self,
                                                   NSLocalizedString(@"Couldn’t Create a Code",
                                                                     @"Account screen: alert title"),
                                                   error.localizedDescription
                                                       ?: NSLocalizedString(@"Check your connection and try again.",
                                                                            @"Account screen: alert message"));
                                         return;
                                       }
                                       PresentForm(unsafe_self,
                                                   [[[XERecoveryForm alloc] initWithCode:code
                                                                               firstTime:NO
                                                                                    done:nil] autorelease],
                                                   YES);
                                     }];
                               }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (void)pickImportFile {
  UTType* profile_type = [UTType typeWithIdentifier:@"jp.xenios.xenios.profile"];
  UIDocumentPickerViewController* picker =
      [[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ profile_type ?: UTTypeItem ]
                                                                   asCopy:YES] autorelease];
  picker.delegate = self;
  [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController* __unused)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  if (!urls.firstObject) {
    return;
  }
  __unsafe_unretained XEAccountViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      importProfileFromURL:urls.firstObject
                completion:^(NSError* error) {
                  ShowAlert(unsafe_self,
                            error ? NSLocalizedString(@"Couldn’t Import", @"Import: alert title")
                                  : NSLocalizedString(@"Profile Imported", @"Import: alert title"),
                            error ? error.localizedDescription
                                  : NSLocalizedString(@"Saves, achievements and settings are in "
                                                      @"place.",
                                                      @"Import: alert message"));
                  [unsafe_self reload];
                }];
}

@end

// ---- Entry points ----------------------------------------------------------------------------

UIViewController* xe_ios_new_account_view_controller(NSString* profile_gamertag) {
  return [[[XEAccountViewController alloc] initWithProfileGamertag:profile_gamertag] autorelease];
}

void xe_ios_present_go_online(UIViewController* presenter, NSString* profile_gamertag,
                              void (^done)(void)) {
  void (^finished)(void) = [[done copy] autorelease];
  __unsafe_unretained UIViewController* unsafe_presenter = presenter;
  XEGamertagForm* form = [[[XEGamertagForm alloc]
      initWithMode:XEGamertagModeGoOnline
       currentName:profile_gamertag
              done:^(XeniaLiveNameResult* result) {
                if (result.recoveryCode.length) {
                  // A new account: the recovery code, once, before anything else.
                  PresentForm(unsafe_presenter,
                              [[[XERecoveryForm alloc] initWithCode:result.recoveryCode
                                                          firstTime:YES
                                                               done:finished] autorelease],
                              YES);
                } else if (finished) {
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

// A .xeniosprofile opened from AirDrop or Files: confirm, then import it. Handles the file whether
// or not the account screens are open.
BOOL xe_ios_handle_opened_profile_file(NSURL* url, UIViewController* presenter) {
  if (![url.pathExtension.lowercaseString isEqualToString:@"xeniosprofile"]) {
    return NO;
  }
  if (!xe_ios_live_account_ui_enabled()) {
    ShowAlert(presenter, NSLocalizedString(@"Can’t Import Yet", @"Profile file: alert title"),
              NSLocalizedString(@"Profile files aren’t supported in this build.", @"Profile file: alert message"));
    return YES;
  }
  NSString* name = url.lastPathComponent.stringByDeletingPathExtension;
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:[NSString stringWithFormat:NSLocalizedString(@"Import “%@”?",
                                                                            @"Profile file: alert title; %@ "
                                                                            @"is the file name"),
                                                          name]
                       message:NSLocalizedString(@"Its saves, achievements and settings are added to this "
                                                 @"device.",
                                                 @"Profile file: alert message")
                preferredStyle:UIAlertControllerStyleAlert];
  NSURL* file = [[url retain] autorelease];
  void (^cleanup)(void) = ^{
    // An Inbox copy is ours to remove; a file in Files stays.
    if ([file.path containsString:@"/Documents/Inbox/"]) {
      [[NSFileManager defaultManager] removeItemAtURL:file error:nil];
    }
  };
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Profile file: alert button")
                                            style:UIAlertActionStyleCancel
                                          handler:^(UIAlertAction* __unused a) {
                                            cleanup();
                                          }]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Import", @"Profile file: alert button")
                                 style:UIAlertActionStyleDefault
                               handler:^(UIAlertAction* __unused a) {
                                 const BOOL scoped = [file startAccessingSecurityScopedResource];
                                 [xe_ios_live_account_service()
                                     importProfileFromURL:file
                                               completion:^(NSError* error) {
                                                 if (scoped) {
                                                   [file stopAccessingSecurityScopedResource];
                                                 }
                                                 cleanup();
                                                 ShowAlert(presenter,
                                                           error ? NSLocalizedString(@"Couldn’t Import", @"Import: alert title")
                                                                 : NSLocalizedString(@"Profile Imported", @"Import: alert title"),
                                                           error ? error.localizedDescription
                                                                 : NSLocalizedString(@"Saves, achievements and settings are in place.",
                                                                                     @"Import: alert message"));
                                               }];
                               }]];
  [TopOf(presenter) presentViewController:alert animated:YES completion:nil];
  return YES;
}

void xe_ios_present_move_profile(UIViewController* presenter) {
  PresentForm(presenter, [[[XEExportForm alloc] init] autorelease], NO);
}
