/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice/apple/apple_voice_device.h"

#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_MAC
#import <CoreAudio/CoreAudio.h>
#endif

namespace xe {
namespace apu {
namespace voice {

const char* const kVoiceChatChangedNotification = "XeniaVoiceChatChanged";

namespace {

// Posted by the iOS audio session code once the session is active again.
NSString* const kAudioSessionRestoredNotification =
    @"XeniaAudioSessionRestored";

// The I/O unit's buses: the speaker is element 0, the microphone element 1.
constexpr AudioUnitElement kSpeakerBus = 0;
constexpr AudioUnitElement kMicrophoneBus = 1;

// The most frames the unit renders at once, and so the microphone buffer.
constexpr UInt32 kMaxFrames = 4096;

// Rebuilds after an audio device change wait this long for the changes to
// settle, so a burst of notifications rebuilds once.
constexpr int64_t kRebuildDelayNs = 200 * NSEC_PER_MSEC;

#if XE_PLATFORM_MAC
// The default devices the unit opens, followed when the user changes them.
constexpr AudioObjectPropertyAddress kDefaultDevices[] = {
    {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
     kAudioObjectPropertyElementMain},
    {kAudioHardwarePropertyDefaultOutputDevice,
     kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
};
#endif

template <typename T>
OSStatus SetProperty(AudioUnit unit, AudioUnitPropertyID property,
                     AudioUnitScope scope, AudioUnitElement element,
                     const T& value) {
  return AudioUnitSetProperty(unit, property, scope, element, &value,
                              sizeof(value));
}

const char* PermissionText(AVAudioApplicationRecordPermission permission) {
  switch (permission) {
    case AVAudioApplicationRecordPermissionGranted:
      return "granted";
    case AVAudioApplicationRecordPermissionDenied:
      return "denied";
    default:
      return "not asked";
  }
}

class AppleVoiceDevice;

// What late blocks (notifications, the permission prompt, device listeners)
// hold instead of the device. Read and cleared only on the device's queue,
// so a block that runs after the device is gone finds it null.
struct DeviceCell {
  AppleVoiceDevice* device = nullptr;
};

class AppleVoiceDevice final : public VoiceDevice {
 public:
  AppleVoiceDevice()
      : queue_(dispatch_queue_create("xenia.voice", DISPATCH_QUEUE_SERIAL)),
        cell_(std::make_shared<DeviceCell>()),
        microphone_(kMaxFrames) {
    cell_->device = this;
  }

  ~AppleVoiceDevice() override {
    Stop();
    dispatch_sync(queue_, ^{
      cell_->device = nullptr;
    });
  }

  bool Start() override {
    dispatch_sync(queue_, ^{
      if (started_) {
        return;
      }
      started_ = true;
      ObserveLocked();
#if XE_PLATFORM_IOS
      // Once the microphone may be used, the session code switches to
      // play-and-record, then tells us through
      // kAudioSessionRestoredNotification.
      RequestPermissionThenActivateLocked();
#else
      RequestPermissionThenBuildLocked();
#endif
    });
    return true;
  }

  void Stop() override {
    dispatch_sync(queue_, ^{
      if (!started_) {
        return;
      }
      started_ = false;
      UnobserveLocked();
      TeardownLocked();
#if XE_PLATFORM_IOS
      if (session_active_) {
        session_active_ = false;
        PostVoiceChatChanged(false);
      }
#endif
    });
  }

 private:
#if XE_PLATFORM_IOS
  static void PostVoiceChatChanged(bool active) {
    NSString* name =
        [NSString stringWithUTF8String:kVoiceChatChangedNotification];
    [[NSNotificationCenter defaultCenter]
        postNotificationName:name
                      object:nil
                    userInfo:@{@"active" : @(active)}];
  }
#endif

  // Runs |work| on |queue| with the device, unless it is gone or stopped by
  // then. For blocks that may outlive the device.
  static void OnQueue(dispatch_queue_t queue, std::shared_ptr<DeviceCell> cell,
                      void (^work)(AppleVoiceDevice* device)) {
    dispatch_async(queue, ^{
      AppleVoiceDevice* device = cell->device;
      if (device && device->started_) {
        work(device);
      }
    });
  }

  // On queue_.
  void ObserveLocked() {
    dispatch_queue_t queue = queue_;
    std::shared_ptr<DeviceCell> cell = cell_;
    observers_.push_back([[NSNotificationCenter defaultCenter]
        addObserverForName:kAudioSessionRestoredNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  OnQueue(queue, cell, ^(AppleVoiceDevice* device) {
                    device->RequestPermissionThenBuildLocked();
                  });
                }]);
#if XE_PLATFORM_MAC
    device_listener_ = ^(UInt32, const AudioObjectPropertyAddress*) {
      OnQueue(queue, cell, ^(AppleVoiceDevice* device) {
        device->ScheduleRebuildLocked();
      });
    };
    for (const AudioObjectPropertyAddress& address : kDefaultDevices) {
      AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &address,
                                          queue_, device_listener_);
    }
#endif
  }

  // On queue_.
  void UnobserveLocked() {
    for (id observer : observers_) {
      [[NSNotificationCenter defaultCenter] removeObserver:observer];
    }
    observers_.clear();
#if XE_PLATFORM_MAC
    if (device_listener_) {
      for (const AudioObjectPropertyAddress& address : kDefaultDevices) {
        AudioObjectRemovePropertyListenerBlock(
            kAudioObjectSystemObject, &address, queue_, device_listener_);
      }
      device_listener_ = nil;
    }
#endif
  }

  // On queue_. A device came or went: rebuild for it once the changes settle.
  void ScheduleRebuildLocked() {
    if (rebuild_pending_) {
      return;
    }
    rebuild_pending_ = true;
    std::shared_ptr<DeviceCell> cell = cell_;
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, kRebuildDelayNs), queue_, ^{
          AppleVoiceDevice* device = cell->device;
          if (!device) {
            return;
          }
          device->rebuild_pending_ = false;
          if (device->started_) {
            XELOGI("Voice: audio devices changed, restarting");
            device->BuildLocked();
          }
        });
  }

#if XE_PLATFORM_IOS
  // On queue_. Voice runs only with the microphone: without access it stays
  // off (the app's Microphone control says so and links to Settings), and the
  // session keeps playing without recording.
  void RequestPermissionThenActivateLocked() {
    const AVAudioApplicationRecordPermission permission =
        [AVAudioApplication sharedInstance].recordPermission;
    if (permission == AVAudioApplicationRecordPermissionUndetermined) {
      dispatch_queue_t queue = queue_;
      std::shared_ptr<DeviceCell> cell = cell_;
      [AVAudioApplication
          requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
            XELOGI("Voice: microphone access {}",
                   granted ? "granted" : "denied");
            OnQueue(queue, cell, ^(AppleVoiceDevice* device) {
              device->RequestPermissionThenActivateLocked();
            });
          }];
      return;
    }
    if (permission != AVAudioApplicationRecordPermissionGranted) {
      XELOGW("Voice: no microphone access ({}), voice stays off",
             PermissionText(permission));
      // Tells the app's Microphone control; the session stays as it is.
      PostVoiceChatChanged(false);
      return;
    }
    if (!session_active_) {
      session_active_ = true;
      PostVoiceChatChanged(true);
    }
  }
#endif

  // On queue_.
  void RequestPermissionThenBuildLocked() {
#if XE_PLATFORM_IOS
    // The session is restored for other reasons too (an interruption ended,
    // the Microphone choice changed): only a headset that has the session
    // and the microphone opens the unit.
    if (!session_active_) {
      return;
    }
#endif
    if ([AVAudioApplication sharedInstance].recordPermission !=
        AVAudioApplicationRecordPermissionUndetermined) {
      BuildLocked();
      return;
    }
    dispatch_queue_t queue = queue_;
    std::shared_ptr<DeviceCell> cell = cell_;
    [AVAudioApplication
        requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
          XELOGI("Voice: microphone access {}",
                 granted ? "granted" : "denied");
          OnQueue(queue, cell, ^(AppleVoiceDevice* device) {
            device->BuildLocked();
          });
        }];
  }

  // On queue_. (Re)opens the audio unit for the current devices and
  // permission: voice processing if it starts, else plain audio.
  void BuildLocked() {
    TeardownLocked();
    const AVAudioApplicationRecordPermission permission =
        [AVAudioApplication sharedInstance].recordPermission;
    const bool mic_allowed =
        permission == AVAudioApplicationRecordPermissionGranted;
    if (!mic_allowed) {
      XELOGW("Voice: no microphone access ({}), speaker only",
             PermissionText(permission));
    }
    if (OpenLocked(kAudioUnitSubType_VoiceProcessingIO, mic_allowed)) {
      return;
    }
    XELOGW("Voice: no voice processing, using plain audio");
#if XE_PLATFORM_IOS
    OpenLocked(kAudioUnitSubType_RemoteIO, mic_allowed);
#else
    // A plain unit takes input and output from one device, and a Mac's
    // microphone and speakers are separate devices: speaker only.
    if (mic_allowed) {
      XELOGW("Voice: the microphone needs voice processing on macOS");
    }
    OpenLocked(kAudioUnitSubType_DefaultOutput, false);
#endif
  }

  // On queue_. True when the unit runs.
  bool OpenLocked(OSType subtype, bool with_microphone) {
    const AudioComponentDescription description = {
        kAudioUnitType_Output, subtype, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    AudioUnit unit = nullptr;
    if (!component || AudioComponentInstanceNew(component, &unit) != noErr) {
      return false;
    }
    const bool voice_processing =
        subtype == kAudioUnitSubType_VoiceProcessingIO;
    // The headset's format both ways; the unit converts to the hardware's.
    const AudioStreamBasicDescription format = {
        kSampleRate,
        kAudioFormatLinearPCM,
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        sizeof(int16_t),
        1,
        sizeof(int16_t),
        1,
        16,
        0};
    OSStatus status = noErr;
    const char* step = "";
    auto check = [&](const char* what, OSStatus result) {
      if (status == noErr && result != noErr) {
        status = result;
        step = what;
      }
    };
    check("max frames",
          SetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                      kAudioUnitScope_Global, 0, kMaxFrames));
    check("speaker format",
          SetProperty(unit, kAudioUnitProperty_StreamFormat,
                      kAudioUnitScope_Input, kSpeakerBus, format));
    check("speaker callback",
          SetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                      kAudioUnitScope_Input, kSpeakerBus,
                      AURenderCallbackStruct{&OnSpeaker, this}));
    if (with_microphone) {
      check("microphone",
            SetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                        kAudioUnitScope_Input, kMicrophoneBus, UInt32(1)));
      check("microphone format",
            SetProperty(unit, kAudioUnitProperty_StreamFormat,
                        kAudioUnitScope_Output, kMicrophoneBus, format));
      check("microphone callback",
            SetProperty(unit, kAudioOutputUnitProperty_SetInputCallback,
                        kAudioUnitScope_Global, kMicrophoneBus,
                        AURenderCallbackStruct{&OnMicrophone, this}));
    }
    if (voice_processing) {
      // Keep the game's own audio at full volume while the mic is open. In
      // party chat, advanced ducking dips it only while someone talks.
      check("ducking",
            SetProperty(unit, kAUVoiceIOProperty_OtherAudioDuckingConfiguration,
                        kAudioUnitScope_Global, 0,
                        AUVoiceIOOtherAudioDuckingConfiguration{
                            advanced_ducking(),
                            kAUVoiceIOOtherAudioDuckingLevelMin}));
    }
    // The microphone callback reads it, so it is set before the unit runs.
    microphone_unit_ = with_microphone ? unit : nullptr;
    check("initialize", AudioUnitInitialize(unit));
    check("start", AudioOutputUnitStart(unit));
    if (status != noErr) {
      XELOGW("Voice: {} audio failed at {} ({})",
             voice_processing ? "voice processing" : "plain", step,
             int32_t(status));
      AudioOutputUnitStop(unit);
      AudioUnitUninitialize(unit);
      AudioComponentInstanceDispose(unit);
      microphone_unit_ = nullptr;
      return false;
    }
    unit_ = unit;
    XELOGI("Voice: audio running (microphone {}, voice processing {})",
           with_microphone ? "on" : "off", voice_processing ? "on" : "off");
    return true;
  }

  // On queue_. Once it returns, neither callback runs again.
  void TeardownLocked() {
    if (!unit_) {
      return;
    }
    AudioOutputUnitStop(unit_);
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
    microphone_unit_ = nullptr;
  }

  // The unit's real-time thread: the speaker wants |frames| samples.
  static OSStatus OnSpeaker(void* context, AudioUnitRenderActionFlags*,
                            const AudioTimeStamp*, UInt32, UInt32 frames,
                            AudioBufferList* buffers) {
    auto* device = static_cast<AppleVoiceDevice*>(context);
    AudioBuffer& buffer = buffers->mBuffers[0];
    const UInt32 count =
        std::min(frames, UInt32(buffer.mDataByteSize / sizeof(int16_t)));
    device->ReadPlayback(static_cast<int16_t*>(buffer.mData), count);
    return noErr;
  }

  // The unit's real-time thread: the microphone has |frames| samples.
  static OSStatus OnMicrophone(void* context,
                               AudioUnitRenderActionFlags* flags,
                               const AudioTimeStamp* time, UInt32 bus,
                               UInt32 frames, AudioBufferList*) {
    auto* device = static_cast<AppleVoiceDevice*>(context);
    AudioUnit unit = device->microphone_unit_;
    if (!unit || frames > kMaxFrames) {
      return noErr;
    }
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = 1;
    list.mBuffers[0].mDataByteSize = UInt32(frames * sizeof(int16_t));
    list.mBuffers[0].mData = device->microphone_.data();
    if (AudioUnitRender(unit, flags, time, bus, frames, &list) == noErr) {
      device->capture().Write(device->microphone_.data(), frames);
    }
    return noErr;
  }

  dispatch_queue_t queue_;
  std::shared_ptr<DeviceCell> cell_;
  bool started_ = false;
  bool rebuild_pending_ = false;
#if XE_PLATFORM_IOS
  // The session was asked to play and record for this headset.
  bool session_active_ = false;
#endif
  std::vector<id> observers_;
#if XE_PLATFORM_MAC
  AudioObjectPropertyListenerBlock device_listener_ = nil;
#endif
  AudioUnit unit_ = nullptr;
  // The unit while it has the microphone, for the microphone callback. Set
  // only while the unit is stopped.
  AudioUnit microphone_unit_ = nullptr;
  // The microphone callback's, preallocated so it never allocates.
  std::vector<int16_t> microphone_;
};

}  // namespace

std::unique_ptr<VoiceDevice> CreateAppleVoiceDevice() {
  return std::make_unique<AppleVoiceDevice>();
}

}  // namespace voice
}  // namespace apu
}  // namespace xe
