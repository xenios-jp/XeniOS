/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_network_status.h"
#import "xenia/ui/ios/shared/ios_background_refresh.h"

#import <BackgroundTasks/BackgroundTasks.h>
#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/net/live_account.h"
#import "xenia/ui/ios/shared/ios_update_check.h"

namespace {

namespace live = xe::net::live;

NSString* const kAccountKey = @"XeniaBackgroundRefresh.Account";
NSString* const kNotifiedKey = @"XeniaBackgroundRefresh.Notified";
NSString* const kAskedKey = @"XeniaBackgroundRefresh.AskedToNotify";
// The most notified IDs kept: far more requests and invites than turn up between two wakes.
constexpr NSUInteger kNotifiedLimit = 200;
// iOS wakes the app at most this often, and usually less.
constexpr NSTimeInterval kRefreshInterval = 30 * 60;
// The lobby sends requests and invites with its welcome: a moment after signing in is enough.
constexpr double kSettleSeconds = 3.0;
// A refresh gets about 30 seconds; the check gives up well before.
constexpr double kLiveTimeoutSeconds = 20.0;

NSString* TaskIdentifier() {
  return [NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".refresh"];
}

// Each request, invite and release is notified once, ever: these are the ones that were.
BOOL WasNotified(NSString* key) {
  return [[[NSUserDefaults standardUserDefaults] arrayForKey:kNotifiedKey] containsObject:key];
}

void MarkNotified(NSString* key) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSMutableArray<NSString*>* notified =
      [[[defaults arrayForKey:kNotifiedKey] mutableCopy] autorelease] ?: [NSMutableArray array];
  if ([notified containsObject:key]) {
    return;
  }
  [notified addObject:key];
  if (notified.count > kNotifiedLimit) {
    [notified removeObjectsInRange:NSMakeRange(0, notified.count - kNotifiedLimit)];
  }
  [defaults setObject:notified forKey:kNotifiedKey];
}

// Posts it unless it was already, and counts it as notified only once iOS has it: with
// notifications not allowed (yet), it comes once they are.
void Notify(NSString* key, NSString* title, NSString* body, NSDictionary* info) {
  if (WasNotified(key)) {
    return;
  }
  UNMutableNotificationContent* content = [[[UNMutableNotificationContent alloc] init] autorelease];
  content.title = title;
  content.body = body;
  content.sound = [UNNotificationSound defaultSound];
  content.userInfo = info ?: @{};
  UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:key
                                                                        content:content
                                                                        trigger:nil];
  UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
  [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
    if (settings.authorizationStatus != UNAuthorizationStatusAuthorized &&
        settings.authorizationStatus != UNAuthorizationStatusProvisional) {
      return;
    }
    [center addNotificationRequest:request
             withCompletionHandler:^(NSError* error) {
               if (error) {
                 XELOGW("Background refresh: a notification wasn't posted ({})",
                        error.localizedDescription.UTF8String ?: "");
                 return;
               }
               dispatch_async(dispatch_get_main_queue(), ^{
                 MarkNotified(key);
               });
             }];
  }];
}

NSString* ToNS(const std::string& text) {
  return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

NSString* RequestKey(const live::Friend& peer) {
  return [NSString stringWithFormat:@"request.%016llX", (unsigned long long)peer.xuid];
}

NSString* InviteKey(const live::Invite& invite) {
  return [@"invite." stringByAppendingString:ToNS(invite.id)];
}

void NotifyLive(const std::vector<live::Friend>& friends,
                const std::vector<live::Invite>& invites) {
  for (const live::Friend& peer : friends) {
    if (peer.state != live::Friend::State::kIncoming) {
      continue;
    }
    Notify(RequestKey(peer), NSLocalizedString(@"Friend Request", @"Notification title"),
           [NSString stringWithFormat:NSLocalizedString(@"%@ wants to be friends.",
                                                        @"Notification; %@ is a gamertag"),
                                      ToNS(peer.gamertag)],
           @{@"kind" : @"friend_request"});
  }
  for (const live::Invite& invite : invites) {
    NSString* body =
        invite.party
            ? [NSString stringWithFormat:NSLocalizedString(@"%@ invited you to their party.",
                                                           @"Notification; %@ is a gamertag"),
                                         ToNS(invite.gamertag)]
            : [NSString stringWithFormat:NSLocalizedString(@"%@ invited you to a game.",
                                                           @"Notification; %@ is a gamertag"),
                                         ToNS(invite.gamertag)];
    Notify(InviteKey(invite), NSLocalizedString(@"Invite", @"Notification title"), body,
           @{@"kind" : @"invite"});
  }
}

// One look at the account, appearing offline so friends don't see it come online, and only
// signing in: a key the account no longer has is refused, never given an account. Calls |done|
// once, on the main queue, with whether the check went as it should (no account is fine).
// Returns a block that ends it early, notifying nothing more.
void (^CheckAccount(void (^done)(BOOL ok)))(void) {
  void (^finish)(BOOL) = [[done copy] autorelease];
  void (^nothing)(void) = ^{
  };
  NSDictionary* account = [[NSUserDefaults standardUserDefaults] dictionaryForKey:kAccountKey];
  NSString* lobby = account[@"lobby"];
  NSString* key_path = account[@"key"];
  const uint64_t xuid = strtoull([account[@"xuid"] UTF8String] ?: "", nullptr, 16);
  // Only the device's own key: making a new one would ask the lobby for another account.
  std::error_code error;
  if (!lobby.length || !key_path.length || !xuid) {
    finish(YES);
    return nothing;
  }
  if (!std::filesystem::exists(key_path.UTF8String, error)) {
    finish(NO);
    return nothing;
  }
  if (!xe_ios_network_available()) {
    finish(NO);
    return nothing;
  }
  auto key = live::DeviceKey::LoadOrCreate(key_path.UTF8String);
  if (!key) {
    finish(NO);
    return nothing;
  }

  struct Check {
    std::atomic<bool> finished = false;
    std::shared_ptr<live::AccountClient> client;
  };
  auto check = std::make_shared<Check>();
  // Ends the check once: reads what the lobby sent, and closes the connection off the main
  // queue (closing waits for its thread).
  void (^end_block)(BOOL) = ^(BOOL answered) {
    if (check->finished.exchange(true)) {
      return;
    }
    auto* client = new std::shared_ptr<live::AccountClient>(std::move(check->client));
    if (answered && *client) {
      NotifyLive((*client)->Friends(), (*client)->Invites());
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      delete client;
    });
    finish(answered);
  };
  // The client's events are C++ and don't retain blocks: they share this, which does. The
  // client lets go of it when it closes.
  struct EndHolder {
    explicit EndHolder(void (^block)(BOOL)) : end([block copy]) {}
    ~EndHolder() { [end release]; }
    void (^end)(BOOL);
  };
  auto holder = std::make_shared<EndHolder>(end_block);

  live::AccountClient::Events events;
  events.signed_in = [holder](uint64_t, const std::string&) {
    void (^end)(BOOL) = holder->end;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(kSettleSeconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     end(YES);
                   });
  };
  // Refused: the key was removed from the account, or the account is gone.
  events.request_failed = [holder](const std::string& request, const std::string&) {
    if (request != "auth") {
      return;
    }
    void (^end)(BOOL) = holder->end;
    dispatch_async(dispatch_get_main_queue(), ^{
      end(NO);
    });
  };
  live::AccountOptions options;
  options.appear_offline = true;
  options.sign_in_only = true;
  check->client = std::make_shared<live::AccountClient>(lobby.UTF8String, std::move(*key), xuid,
                                                        [account[@"gamertag"] UTF8String] ?: "",
                                                        std::move(events), std::move(options));
  void (^end)(BOOL) = holder->end;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(kLiveTimeoutSeconds * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   end(NO);
                 });
  return [[^{
    holder->end(NO);
  } copy] autorelease];
}

void CheckUpdate(void (^done)(void)) {
  void (^finish)(void) = [[done copy] autorelease];
  xe_check_for_update(NO, ^(XeniaUpdateInfo* info) {
    if (info.status == XeniaUpdateStatusUpdateAvailable && info.latestVersionText.length) {
      Notify([@"update." stringByAppendingString:info.latestVersionText],
             NSLocalizedString(@"XeniOS Update", @"Notification title"),
             [NSString stringWithFormat:NSLocalizedString(@"XeniOS %@ is available.",
                                                          @"Notification; %@ is the version"),
                                        info.latestVersionText],
             @{@"kind" : @"update", @"url" : info.downloadURLString ?: @""});
    }
    finish();
  });
}

// The task's state, on the main queue. The task holds its expiration handler, which holds this,
// which holds the task: ending lets go of the handler.
struct RefreshState {
  ~RefreshState() {
    [task release];
    [cancel_account release];
  }
  BGAppRefreshTask* task = nil;
  void (^cancel_account)(void) = nil;
  int pending = 2;  // The update check and the account check.
  bool ok = true;
  bool ended = false;
};

void Refresh(BGAppRefreshTask* task) {
  xe_ios_background_refresh_schedule();
  auto state = std::make_shared<RefreshState>();
  state->task = [task retain];
  // Once: when both checks are done, or when iOS's time is up (the account check stops then,
  // so nothing is notified after the task ended).
  auto end = [state](bool success) {
    if (state->ended) {
      return;
    }
    state->ended = true;
    state->task.expirationHandler = nil;
    if (state->cancel_account) {
      state->cancel_account();
    }
    [state->task setTaskCompletedWithSuccess:success];
  };
  auto checked = [state, end](bool ok) {
    state->ok = state->ok && ok;
    if (--state->pending == 0) {
      end(state->ok);
    }
  };
  task.expirationHandler = ^{
    dispatch_async(dispatch_get_main_queue(), ^{
      end(false);
    });
  };
  dispatch_async(dispatch_get_main_queue(), ^{
    CheckUpdate(^{
      checked(true);
    });
    void (^cancel)(void) = CheckAccount(^(BOOL ok) {
      checked(ok);
    });
    if (!state->ended) {
      state->cancel_account = [cancel copy];
    }
  });
}

}  // namespace

// Taps open the app; an update's opens its download page, once it's one the update flow trusts.
@interface XeniaNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation XeniaNotificationDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  NSDictionary* info = response.notification.request.content.userInfo;
  if ([info[@"kind"] isEqualToString:@"update"]) {
    NSURL* url = [NSURL URLWithString:info[@"url"] ?: @""];
    if (url && xe_ios_is_trusted_release_url(url)) {
      [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
    }
  }
  completionHandler();
}

// Only posted in the background; one that lands as the app opens still shows.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
  completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}

@end

void xe_ios_background_refresh_register(void) {
  static XeniaNotificationDelegate* delegate = [[XeniaNotificationDelegate alloc] init];
  [UNUserNotificationCenter currentNotificationCenter].delegate = delegate;
  const BOOL registered =
      [[BGTaskScheduler sharedScheduler] registerForTaskWithIdentifier:TaskIdentifier()
                                                            usingQueue:nil
                                                         launchHandler:^(__kindof BGTask* task) {
                                                           Refresh((BGAppRefreshTask*)task);
                                                         }];
  if (!registered) {
    XELOGW("Background refresh: the task isn't in BGTaskSchedulerPermittedIdentifiers");
  }
}

void xe_ios_background_refresh_schedule(void) {
  BGAppRefreshTaskRequest* request =
      [[[BGAppRefreshTaskRequest alloc] initWithIdentifier:TaskIdentifier()] autorelease];
  request.earliestBeginDate = [NSDate dateWithTimeIntervalSinceNow:kRefreshInterval];
  NSError* error = nil;
  if (![[BGTaskScheduler sharedScheduler] submitTaskRequest:request error:&error]) {
    // The Simulator, or Background App Refresh turned off for XeniOS.
    XELOGI("Background refresh: not scheduled ({})", error.localizedDescription.UTF8String ?: "");
  }
}

void xe_ios_request_notification_permission(void) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if ([defaults boolForKey:kAskedKey]) {
    return;
  }
  [defaults setBool:YES forKey:kAskedKey];
  [[UNUserNotificationCenter currentNotificationCenter]
      requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound |
                                      UNAuthorizationOptionBadge
                    completionHandler:^(BOOL granted, NSError* __unused error) {
                      XELOGI("Background refresh: notifications {}",
                             granted ? "allowed" : "not allowed");
                    }];
}

void xe_ios_background_refresh_set_account(NSString* lobby, uint64_t online_xuid,
                                           NSString* gamertag, NSString* key_path) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if (!lobby.length || !online_xuid || !key_path.length) {
    [defaults removeObjectForKey:kAccountKey];
    return;
  }
  NSDictionary* account = @{
    @"lobby" : lobby,
    @"xuid" : [NSString stringWithFormat:@"%016llX", (unsigned long long)online_xuid],
    @"gamertag" : gamertag ?: @"",
    @"key" : key_path,
  };
  if (![[defaults dictionaryForKey:kAccountKey] isEqualToDictionary:account]) {
    [defaults setObject:account forKey:kAccountKey];
  }
}

void xe_ios_background_refresh_note_seen(const std::vector<live::Friend>& friends,
                                         const std::vector<live::Invite>& invites) {
  for (const live::Friend& peer : friends) {
    if (peer.state == live::Friend::State::kIncoming) {
      MarkNotified(RequestKey(peer));
    }
  }
  for (const live::Invite& invite : invites) {
    MarkNotified(InviteKey(invite));
  }
}
