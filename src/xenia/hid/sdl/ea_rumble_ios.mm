/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Compiled with ARC (see CMakeLists.txt).

#import <ExternalAccessory/ExternalAccessory.h>
#import <UIKit/UIKit.h>

#include <array>
#include <atomic>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/hid/sdl/accessory_rumble.h"
#include "xenia/hid/sdl/ea_rumble_profile_ios.h"

using xe::hid::sdl::EARumbleProfile;
using xe::hid::sdl::kMaxEARumblePacketSize;

namespace {

using RumblePacket = std::array<uint8_t, kMaxEARumblePacketSize>;

uint32_t PackMotors(uint16_t low_frequency, uint16_t high_frequency) {
  return (uint32_t(low_frequency) << 16) | high_frequency;
}

void RunOnMain(dispatch_block_t block) {
  if (NSThread.isMainThread) {
    block();
  } else {
    dispatch_async(dispatch_get_main_queue(), block);
  }
}

}  // namespace

// One ExternalAccessory rumble session for one controller. Motor requests
// arrive on any thread and are coalesced into a single main queue update;
// everything else, the EASession and its streams included, lives on main.
@interface XEEARumbleSession : NSObject <NSStreamDelegate>
- (instancetype)initWithProfile:(const EARumbleProfile*)profile;
- (void)requestMotors:(uint32_t)motors;
- (void)invalidate;
@end

@implementation XEEARumbleSession {
  const EARumbleProfile* _profile;
  NSString* _protocol;
  NSString* _manufacturer;
  NSString* _model;

  EASession* _session;
  NSTimer* _resendTimer;
  BOOL _appActive;

  // Latest encoded motor levels, resent by _resendTimer while non-zero.
  RumblePacket _current;
  // Waiting for stream space; replaced by newer packets before it's written.
  RumblePacket _queued;
  BOOL _hasQueued;
  // Partially written packet, finished before anything newer is started.
  RumblePacket _writing;
  size_t _writingOffset;
  BOOL _isWriting;
  // Last packet fully written, to skip resending unchanged levels.
  RumblePacket _written;
  BOOL _hasWritten;

  std::atomic<uint32_t> _requestedMotors;
  std::atomic<bool> _updateScheduled;
  std::atomic<bool> _invalidated;
}

- (instancetype)initWithProfile:(const EARumbleProfile*)profile {
  self = [super init];
  if (self) {
    _profile = profile;
    _protocol = @(profile->protocol);
    _manufacturer = @(profile->manufacturer);
    _model = @(profile->model);
    // The emulator only creates controllers while in the foreground; a write
    // made in the background just fails and closes the session.
    _appActive = YES;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserver:self
               selector:@selector(applicationWillResignActive:)
                   name:UIApplicationWillResignActiveNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(applicationDidBecomeActive:)
                   name:UIApplicationDidBecomeActiveNotification
                 object:nil];
  }
  return self;
}

- (void)requestMotors:(uint32_t)motors {
  if (_invalidated.load(std::memory_order_acquire)) {
    return;
  }
  _requestedMotors.store(motors, std::memory_order_relaxed);
  if (_updateScheduled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    // Clear first: a request racing this update schedules one more, which at
    // worst finds nothing changed.
    self->_updateScheduled.store(false, std::memory_order_seq_cst);
    if (!self->_invalidated.load(std::memory_order_acquire) && self->_appActive) {
      [self applyMotors:self->_requestedMotors.load(std::memory_order_seq_cst)];
    }
  });
}

- (void)invalidate {
  _invalidated.store(true, std::memory_order_release);
  // The block keeps self alive until the motors are stopped on main.
  RunOnMain(^{
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [self stopAndClose];
  });
}

#pragma mark - Main thread

- (void)applyMotors:(uint32_t)motors {
  const uint16_t low_frequency = uint16_t(motors >> 16);
  const uint16_t high_frequency = uint16_t(motors);
  _profile->encode_packet(low_frequency, high_frequency, _current.data());
  if (motors) {
    [self startResendTimer];
  } else {
    [self stopResendTimer];
    // A pad that was never driven is already still.
    if (!_session) {
      return;
    }
  }
  if (![self isLatestPacket:_current]) {
    [self queuePacket:_current];
  }
}

- (BOOL)isLatestPacket:(const RumblePacket&)packet {
  const RumblePacket* latest = _hasQueued    ? &_queued
                               : _isWriting  ? &_writing
                               : _hasWritten ? &_written
                                             : nullptr;
  return latest && !std::memcmp(latest->data(), packet.data(), _profile->packet_size);
}

- (void)queuePacket:(const RumblePacket&)packet {
  _queued = packet;
  _hasQueued = YES;
  if ([self openSession]) {
    [self flush];
  }
}

- (void)startResendTimer {
  if (_resendTimer || _profile->resend_interval.count() <= 0) {
    return;
  }
  __weak XEEARumbleSession* weak_self = self;
  _resendTimer = [NSTimer
      timerWithTimeInterval:std::chrono::duration<double>(_profile->resend_interval).count()
                    repeats:YES
                      block:^(NSTimer*) {
                        [weak_self resendCurrentPacket];
                      }];
  [NSRunLoop.mainRunLoop addTimer:_resendTimer forMode:NSRunLoopCommonModes];
}

- (void)stopResendTimer {
  [_resendTimer invalidate];
  _resendTimer = nil;
}

- (void)resendCurrentPacket {
  if (_invalidated.load(std::memory_order_acquire) || !_appActive) {
    [self stopResendTimer];
    return;
  }
  [self queuePacket:_current];
}

- (EAAccessory*)findAccessory {
  for (EAAccessory* accessory in EAAccessoryManager.sharedAccessoryManager.connectedAccessories) {
    if (accessory.connected && [accessory.protocolStrings containsObject:_protocol] &&
        [accessory.manufacturer caseInsensitiveCompare:_manufacturer] == NSOrderedSame &&
        [accessory.modelNumber caseInsensitiveCompare:_model] == NSOrderedSame) {
      return accessory;
    }
  }
  return nil;
}

- (BOOL)openSession {
  if (_session) {
    return YES;
  }
  if (!_appActive) {
    return NO;
  }
  EAAccessory* accessory = [self findAccessory];
  if (!accessory) {
    return NO;
  }
  EASession* session = [[EASession alloc] initWithAccessory:accessory forProtocol:_protocol];
  if (!session.inputStream || !session.outputStream) {
    XELOGW("ExternalAccessory rumble: could not open a {} session", _profile->protocol);
    return NO;
  }
  for (NSStream* stream in @[ session.inputStream, session.outputStream ]) {
    stream.delegate = self;
    [stream scheduleInRunLoop:NSRunLoop.mainRunLoop forMode:NSDefaultRunLoopMode];
    [stream open];
  }
  _session = session;
  XELOGI("ExternalAccessory rumble: opened a {} session", _profile->protocol);
  return YES;
}

- (void)closeSession {
  [self stopResendTimer];
  _hasQueued = NO;
  _isWriting = NO;
  _hasWritten = NO;
  if (!_session) {
    return;
  }
  for (NSStream* stream in @[ _session.inputStream, _session.outputStream ]) {
    stream.delegate = nil;
    [stream close];
    [stream removeFromRunLoop:NSRunLoop.mainRunLoop forMode:NSDefaultRunLoopMode];
  }
  _session = nil;
}

// Writes as much as the stream takes now; the rest goes on space events.
- (void)flush {
  NSOutputStream* output = _session.outputStream;
  const size_t packet_size = _profile->packet_size;
  while (output.hasSpaceAvailable) {
    if (!_isWriting) {
      if (!_hasQueued) {
        return;
      }
      _writing = _queued;
      _hasQueued = NO;
      _writingOffset = 0;
      _isWriting = YES;
    }
    const NSInteger written = [output write:_writing.data() + _writingOffset
                                  maxLength:packet_size - _writingOffset];
    if (written <= 0) {
      XELOGW("ExternalAccessory rumble: write failed ({})", written);
      [self closeSession];
      return;
    }
    _writingOffset += size_t(written);
    if (_writingOffset == packet_size) {
      _written = _writing;
      _hasWritten = YES;
      _isWriting = NO;
    }
  }
}

// Best effort: the session is about to go away, so the stop packet can't wait
// for a space event.
- (void)stopAndClose {
  [self stopResendTimer];
  if (_session) {
    _profile->encode_packet(0, 0, _current.data());
    if (![self isLatestPacket:_current]) {
      [self queuePacket:_current];
    }
  }
  [self closeSession];
}

- (void)applicationWillResignActive:(NSNotification*)notification {
  _appActive = NO;
  [self stopAndClose];
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
  _appActive = YES;
}

- (void)stream:(NSStream*)stream handleEvent:(NSStreamEvent)event {
  switch (event) {
    case NSStreamEventHasBytesAvailable: {
      // Nothing to read, but the accessory stalls if its reports pile up.
      NSInputStream* input = (NSInputStream*)stream;
      uint8_t buffer[64];
      while (input.hasBytesAvailable && [input read:buffer maxLength:sizeof(buffer)] > 0) {
      }
    } break;
    case NSStreamEventHasSpaceAvailable:
      [self flush];
      break;
    case NSStreamEventErrorOccurred:
    case NSStreamEventEndEncountered:
      XELOGI("ExternalAccessory rumble: {} session ended", _profile->protocol);
      [self closeSession];
      break;
    default:
      break;
  }
}

@end

namespace xe {
namespace hid {
namespace sdl {

namespace {

class EARumble final : public AccessoryRumble {
 public:
  explicit EARumble(const EARumbleProfile* profile)
      : session_([[XEEARumbleSession alloc] initWithProfile:profile]) {}
  ~EARumble() override { [session_ invalidate]; }

  void SetMotors(uint16_t low_frequency, uint16_t high_frequency) override {
    [session_ requestMotors:PackMotors(low_frequency, high_frequency)];
  }

 private:
  XEEARumbleSession* session_;
};

}  // namespace

std::unique_ptr<AccessoryRumble> CreateAccessoryRumble(std::string_view controller_name) {
  const EARumbleProfile* profile = FindEARumbleProfile(controller_name);
  if (!profile) {
    return nullptr;
  }
  return std::make_unique<EARumble>(profile);
}

}  // namespace sdl
}  // namespace hid
}  // namespace xe
