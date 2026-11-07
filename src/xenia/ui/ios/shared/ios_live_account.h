/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_LIVE_ACCOUNT_H_
#define XENIA_UI_IOS_SHARED_IOS_LIVE_ACCOUNT_H_

#import <UIKit/UIKit.h>

// The Live account, for the account screens (Profile > Online Account): going online
// with a gamertag, renaming, linking devices, recovery, and moving a profile's data.
//
// The screens only talk to XeniaLiveAccountService. Every call is asynchronous and its
// completion runs on the main queue. xe_ios_live_account_service() is a fake
// (XeniaLiveAccountStub) until the app installs the real one, built on
// xe::net::live::AccountClient, with xe_ios_live_account_set_service().

NS_ASSUME_NONNULL_BEGIN

// ---- Gamertags -----------------------------------------------------------------------

// What the lobby says about a name (gamertag-protocol.md: register/rename errors and the
// planned check call), plus two the app adds itself.
typedef NS_ENUM(NSInteger, XeniaLiveNameStatus) {
  XeniaLiveNameStatusAvailable = 0,
  XeniaLiveNameStatusTaken,       // Someone's, or held for 30 days after a change.
  XeniaLiveNameStatusReserved,    // Xbox, XeniOS, Admin and variants.
  XeniaLiveNameStatusNotAllowed,  // The blocklist.
  XeniaLiveNameStatusInvalid,     // Fails the 360 rules (the app checks these first).
  XeniaLiveNameStatusTooSoon,     // Renamed less than 30 days ago (see nextRename).
  XeniaLiveNameStatusBanned,
  XeniaLiveNameStatusLimit,       // Too many checks or tries; wait a little.
  XeniaLiveNameStatusUnreachable  // The lobby didn't answer. Not the lobby's word.
};

@interface XeniaLiveNameResult : NSObject
@property(nonatomic) XeniaLiveNameStatus status;
// Up to 3, built from the typed name, for Taken, Reserved and NotAllowed.
@property(nonatomic, copy) NSArray<NSString*>* suggestions;
// TooSoon: when the next change is allowed.
@property(nonatomic, strong, nullable) NSDate* nextRename;
// goOnline only: the recovery code of an account that was just created, shown once.
@property(nonatomic, copy, nullable) NSString* recoveryCode;
@end

// The 360 rules, on the device and instant: 1 to 15 letters, digits and single spaces,
// starting with a letter, not ending in a space. Returns nil if the name passes, else
// the message to show under the field ("Too long: 15 characters at most", "Can’t start
// with a number", "Letters, digits and single spaces only", "Can’t end with a space").
// Empty gives nil too (nothing to complain about yet).
NSString* _Nullable xe_ios_gamertag_local_error(NSString* name);

// ---- The account ---------------------------------------------------------------------

@interface XeniaLiveAccountSummary : NSObject
@property(nonatomic) BOOL online;  // The active profile has an online account.
@property(nonatomic, copy) NSString* gamertag;
// When the next rename is allowed; nil means now.
@property(nonatomic, strong, nullable) NSDate* nextRename;
// The migration flagged the name as a duplicate or bad: ask for a new one, once per
// sign-in. A rename then needs no wait.
@property(nonatomic) BOOL renameFlagged;
@end

@interface XeniaLiveDevice : NSObject
@property(nonatomic, copy) NSString* deviceId;
@property(nonatomic, copy) NSString* name;  // "Bo’s iPhone"
// SF Symbol-ish kind: "iphone", "ipad" or "mac".
@property(nonatomic, copy) NSString* kind;
@property(nonatomic) BOOL thisDevice;
@property(nonatomic, strong, nullable) NSDate* lastSeen;  // nil: active now
@end

typedef NS_ENUM(NSInteger, XeniaLiveLinkStatus) {
  XeniaLiveLinkStatusLinked = 0,
  XeniaLiveLinkStatusBadCode,     // Not a code, or no such code.
  XeniaLiveLinkStatusExpired,
  XeniaLiveLinkStatusLimit,
  XeniaLiveLinkStatusUnreachable,
};

@interface XeniaLiveExportOptions : NSObject
@property(nonatomic) BOOL saves;
@property(nonatomic) BOOL achievements;
@property(nonatomic) BOOL settings;  // Controls and the profile's own settings.
// Per-game settings (graphics, patches). Tuned for each device, so the sheet leaves it off.
@property(nonatomic) BOOL gameSettings;
@end

// ---- Saves on this device, between profiles -----------------------------------------------
//
// Saves live per profile. Two ways to bring them together, both on this device: copy them once,
// or have profiles use one set. Sharing is the emulator's job (one content folder both profiles
// point at); the screens only ask for it and show the state. A shared game must not be running
// when sharing is turned on or off (the call fails with an error saying so).

@interface XeniaLiveLocalProfile : NSObject
@property(nonatomic, copy) NSString* profileId;
@property(nonatomic, copy) NSString* gamertag;
@property(nonatomic) BOOL online;       // Has an online account.
@property(nonatomic) BOOL thisProfile;  // The signed-in one.
@end

// A game with saves on this profile, and whether its saves are shared with another profile.
@interface XeniaLiveSaveGame : NSObject
@property(nonatomic) uint32_t titleId;
@property(nonatomic, copy) NSString* name;
@property(nonatomic) uint64_t bytes;
@property(nonatomic) BOOL shared;  // With the profile asked about.
@end

// ---- Approving a sign-in from another device ---------------------------------------------
//
// Three ways to bring an account to a new device. The new device asks for one and waits; the
// signed-in device answers. Approval by number match: the new device shows 2 digits, the
// signed-in device shows three numbers and the player picks the one on the other screen.
// The lobby never says whether a gamertag exists: an unknown name just times out.

typedef NS_ENUM(NSInteger, XeniaLiveApprovalStatus) {
  XeniaLiveApprovalStatusApproved = 0,  // Signed in; the profile is online on this device.
  XeniaLiveApprovalStatusDenied,        // Said no, or picked the wrong number.
  XeniaLiveApprovalStatusTimedOut,      // Nobody answered in time (or no such gamertag).
  XeniaLiveApprovalStatusUnreachable,   // The lobby didn't answer.
  XeniaLiveApprovalStatusCancelled,     // The player cancelled.
};

// A request the new device is waiting on.
@interface XeniaLiveApprovalRequest : NSObject
@property(nonatomic, copy) NSString* requestId;
// QR join only: what the QR code carries.
@property(nonatomic, copy, nullable) NSString* qrPayload;
// Approval by number: the two digits to show ("47").
@property(nonatomic, copy, nullable) NSString* matchNumber;
@property(nonatomic, strong) NSDate* expires;  // About 2 minutes out.
@end

// What the signed-in device is asked: "Sign in on a new <deviceName>?"
@interface XeniaLiveIncomingApproval : NSObject
@property(nonatomic, copy) NSString* requestId;
@property(nonatomic, copy) NSString* deviceName;  // "Bo’s iPhone"
@property(nonatomic, copy) NSString* platform;    // "iphone", "ipad" or "mac"
@property(nonatomic, copy) NSString* platformText;  // "iPhone · iOS 26"
// Three numbers, one of them the match number, in the order to show.
@property(nonatomic, copy) NSArray<NSString*>* choices;
@property(nonatomic, strong) NSDate* expires;
@end

@protocol XeniaLiveAccountService <NSObject>

// The active profile's account, now. Posts XeniaLiveAccountDidChangeNotification when
// any of it changes (signed in or out, renamed, linked, flag cleared).
@property(nonatomic, readonly) XeniaLiveAccountSummary* summary;

// A name, as typed (the app has already run xe_ios_gamertag_local_error). ~30 a minute;
// Limit when over. Works before the profile is online.
- (void)checkGamertag:(NSString*)gamertag completion:(void (^)(XeniaLiveNameResult*))completion;

// Going online: claims the name on the lobby, renames the profile in place on success
// (saves and achievements stay). A first account comes back with a recoveryCode.
// Taken and the like come back with suggestions and nothing changed.
- (void)goOnlineWithGamertag:(NSString*)gamertag
                  completion:(void (^)(XeniaLiveNameResult*))completion;

// Renaming while online. Once per 30 days; capitals-only changes are free. A flagged
// account may rename at once.
- (void)renameToGamertag:(NSString*)gamertag completion:(void (^)(XeniaLiveNameResult*))completion;

// Link another device. On the signed-in device: a short code, K7Q2-M9XD, good for a few
// minutes. error is set if the lobby can't be reached.
- (void)createLinkCodeWithCompletion:(void (^)(NSString* _Nullable code, NSDate* _Nullable expires,
                                               NSError* _Nullable error))completion;
// On the new device: link with that code (any case, with or without the hyphen).
- (void)linkWithCode:(NSString*)code completion:(void (^)(XeniaLiveLinkStatus))completion;

- (void)loadDevicesWithCompletion:(void (^)(NSArray<XeniaLiveDevice*>* _Nullable devices,
                                            NSError* _Nullable error))completion;
- (void)removeDevice:(XeniaLiveDevice*)device completion:(void (^)(NSError* _Nullable error))completion;

// A new recovery code, R7K2-9QMD-4XTW. The old one stops working. (The first one comes
// with goOnline.)
- (void)createRecoveryCodeWithCompletion:(void (^)(NSString* _Nullable code,
                                                   NSError* _Nullable error))completion;
// Brings an account to this device with its recovery code.
- (void)recoverWithCode:(NSString*)code completion:(void (^)(XeniaLiveLinkStatus))completion;

// Moving a profile's data, which doesn't sync. Sizes are bytes, for the Export screen.
- (void)exportSizesWithCompletion:(void (^)(uint64_t saves, uint64_t achievements,
                                            uint64_t settings))completion;
// Writes one file to the temporary directory, named "<Gamertag>.xeniosprofile" (the extension
// jp.xenios.xenios.profile, which the app registers: AirDrop and Files open it in XeniOS, which
// calls importProfileFromURL:). The screen shares it and deletes it.
- (void)exportProfileWithOptions:(XeniaLiveExportOptions*)options
                      completion:(void (^)(NSURL* _Nullable file, NSError* _Nullable error))completion;
// Reads a file made by export into the active profile (or a new one).
- (void)importProfileFromURL:(NSURL*)url completion:(void (^)(NSError* _Nullable error))completion;

// ---- Approve on another device, and QR join --------------------------------------------

// New device: asks the account's other devices to approve a sign-in as |gamertag|. Returns the
// request to show (nil request: the lobby couldn't be reached; |status| says why). The result
// of the wait comes from waitForApproval:.
- (void)requestApprovalForGamertag:(NSString*)gamertag
                        completion:(void (^)(XeniaLiveApprovalRequest* _Nullable request,
                                             XeniaLiveApprovalStatus status))completion;
// New device: QR join. The request's qrPayload is what to encode; waiting works the same.
- (void)requestQRLinkWithCompletion:(void (^)(XeniaLiveApprovalRequest* _Nullable request,
                                              XeniaLiveApprovalStatus status))completion;
// New device: completes once with the outcome (approved, denied, timed out, cancelled).
- (void)waitForApproval:(XeniaLiveApprovalRequest*)request
             completion:(void (^)(XeniaLiveApprovalStatus status))completion;
- (void)cancelApproval:(XeniaLiveApprovalRequest*)request;

// Signed-in device: a sign-in waiting for an answer, or nil. Posts
// XeniaLiveIncomingApprovalNotification when one arrives or goes away.
@property(nonatomic, readonly, nullable) XeniaLiveIncomingApproval* pendingApproval;
// Approve (|chosen| is the number the player picked; the lobby decides if it's right) or deny.
// The screen has already asked for Face ID, Touch ID or the passcode before it approves.
- (void)approveIncoming:(XeniaLiveIncomingApproval*)approval
                 chosen:(NSString*)chosen
             completion:(void (^)(XeniaLiveApprovalStatus status))completion;
- (void)denyIncoming:(XeniaLiveIncomingApproval*)approval;
// Signed-in device: scanned a QR code. Returns the approval to show for it (the signed-in
// device then approves it like any other), or why not.
- (void)beginApprovalFromQRPayload:(NSString*)payload
                        completion:(void (^)(XeniaLiveIncomingApproval* _Nullable approval,
                                             XeniaLiveLinkStatus status))completion;

// ---- Saves between profiles on this device ----------------------------------------------

// The profiles on this device, the signed-in one first.
- (void)localProfilesWithCompletion:(void (^)(NSArray<XeniaLiveLocalProfile*>* profiles))completion;
// Every profile uses one set of saves. Turning it on merges them (the newest of each save is
// kept, the others are backed up first); off gives each profile its own copy again.
@property(nonatomic, readonly) BOOL savesSharedAcrossProfiles;
- (void)setSavesSharedAcrossProfiles:(BOOL)shared completion:(void (^)(NSError* _Nullable error))completion;
// The signed-in profile's games with saves, and which are shared with |other|.
- (void)saveGamesSharedWithProfile:(NSString*)otherProfileId
                        completion:(void (^)(NSArray<XeniaLiveSaveGame*>* games))completion;
// One game's saves shared (or not) between the signed-in profile and |other|.
- (void)setSharing:(BOOL)shared
          forTitle:(uint32_t)titleId
       withProfile:(NSString*)otherProfileId
        completion:(void (^)(NSError* _Nullable error))completion;
// A one-time copy of the signed-in profile's saves to |other|, keeping the newer of any two.
// |copied| and |kept| count saves.
- (void)copySavesToProfile:(NSString*)otherProfileId
                completion:(void (^)(NSUInteger copied, NSUInteger kept, NSError* _Nullable error))completion;

@end

extern NSNotificationName const XeniaLiveAccountDidChangeNotification;
extern NSNotificationName const XeniaLiveIncomingApprovalNotification;

// The service the screens use. The app installs the real one at startup; until then this
// is XeniaLiveAccountStub, which fakes everything in memory.
id<XeniaLiveAccountService> xe_ios_live_account_service(void);
void xe_ios_live_account_set_service(id<XeniaLiveAccountService> _Nullable service);
// YES for the stub (and nothing installed).
BOOL xe_ios_live_account_service_is_stub(void);

// Whether Profile shows Online Account. On when a real service is installed, or in a
// preview build of the screens (defaults write ... XeniaLiveAccountPreview -bool YES, or
// the launch argument -XeniaLiveAccountPreview YES), so the fake never shows in a normal
// build.
BOOL xe_ios_live_account_ui_enabled(void);

NS_ASSUME_NONNULL_END

#endif  // XENIA_UI_IOS_SHARED_IOS_LIVE_ACCOUNT_H_
