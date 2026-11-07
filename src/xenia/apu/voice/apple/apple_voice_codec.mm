/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/voice/apple/apple_voice_codec.h"

#import <AVFAudio/AVFAudio.h>

#include <cstring>

#include "xenia/base/logging.h"

namespace xe {
namespace apu {
namespace voice {

namespace {

constexpr uint32_t kBitRate = 16000;

AVAudioFormat* PcmFormat() {
  return [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatInt16
                                          sampleRate:kSampleRate
                                            channels:1
                                         interleaved:YES];
}

AVAudioFormat* OpusFormat() {
  AudioStreamBasicDescription description = {};
  description.mSampleRate = kSampleRate;
  description.mFormatID = kAudioFormatOpus;
  description.mChannelsPerFrame = 1;
  description.mFramesPerPacket = kPartyFrameSamples;
  return [[AVAudioFormat alloc] initWithStreamDescription:&description];
}

class AppleOpusEncoder final : public VoiceEncoder {
 public:
  bool Open() {
    pcm_ = PcmFormat();
    opus_ = OpusFormat();
    converter_ = [[AVAudioConverter alloc] initFromFormat:pcm_ toFormat:opus_];
    if (!converter_) {
      return false;
    }
    converter_.bitRate = kBitRate;
    input_ = [[AVAudioPCMBuffer alloc] initWithPCMFormat:pcm_
                                           frameCapacity:kPartyFrameSamples];
    return input_ != nil;
  }

  std::vector<uint8_t> Encode(
      std::span<const int16_t, kPartyFrameSamples> frame) override {
    std::memcpy(input_.int16ChannelData[0], frame.data(),
                frame.size() * sizeof(int16_t));
    input_.frameLength = kPartyFrameSamples;
    AVAudioCompressedBuffer* output = [[AVAudioCompressedBuffer alloc]
          initWithFormat:opus_
          packetCapacity:1
       maximumPacketSize:converter_.maximumOutputPacketSize];
    __block bool fed = false;
    AVAudioPCMBuffer* input = input_;
    NSError* error = nil;
    const AVAudioConverterOutputStatus status = [converter_
        convertToBuffer:output
                  error:&error
     withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount,
                                        AVAudioConverterInputStatus* state) {
       if (fed) {
         *state = AVAudioConverterInputStatus_NoDataNow;
         return nil;
       }
       fed = true;
       *state = AVAudioConverterInputStatus_HaveData;
       return input;
     }];
    if (status == AVAudioConverterOutputStatus_Error || !output.packetCount) {
      return {};
    }
    const auto* bytes = static_cast<const uint8_t*>(output.data);
    return std::vector<uint8_t>(bytes, bytes + output.byteLength);
  }

 private:
  AVAudioFormat* pcm_ = nil;
  AVAudioFormat* opus_ = nil;
  AVAudioConverter* converter_ = nil;
  AVAudioPCMBuffer* input_ = nil;
};

class AppleOpusDecoder final : public VoiceDecoder {
 public:
  bool Open() {
    pcm_ = PcmFormat();
    opus_ = OpusFormat();
    converter_ = [[AVAudioConverter alloc] initFromFormat:opus_ toFormat:pcm_];
    output_ = [[AVAudioPCMBuffer alloc] initWithPCMFormat:pcm_
                                            frameCapacity:kPartyFrameSamples * 2];
    return converter_ && output_;
  }

  bool Decode(std::span<const uint8_t> packet,
              std::span<int16_t, kPartyFrameSamples> frame) override {
    std::fill(frame.begin(), frame.end(), int16_t(0));
    AVAudioCompressedBuffer* input = [[AVAudioCompressedBuffer alloc]
          initWithFormat:opus_
          packetCapacity:1
       maximumPacketSize:packet.size()];
    std::memcpy(input.data, packet.data(), packet.size());
    input.byteLength = uint32_t(packet.size());
    input.packetCount = 1;
    input.packetDescriptions[0] =
        AudioStreamPacketDescription{0, 0, uint32_t(packet.size())};
    output_.frameLength = 0;
    __block bool fed = false;
    NSError* error = nil;
    const AVAudioConverterOutputStatus status = [converter_
        convertToBuffer:output_
                  error:&error
     withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount,
                                        AVAudioConverterInputStatus* state) {
       if (fed) {
         *state = AVAudioConverterInputStatus_NoDataNow;
         return nil;
       }
       fed = true;
       *state = AVAudioConverterInputStatus_HaveData;
       return input;
     }];
    if (status == AVAudioConverterOutputStatus_Error) {
      return false;
    }
    // The first packet comes out short (the codec's lookahead); it's padded.
    const size_t count =
        std::min<size_t>(output_.frameLength, kPartyFrameSamples);
    std::memcpy(frame.data() + (kPartyFrameSamples - count),
                output_.int16ChannelData[0], count * sizeof(int16_t));
    return count != 0;
  }

 private:
  AVAudioFormat* pcm_ = nil;
  AVAudioFormat* opus_ = nil;
  AVAudioConverter* converter_ = nil;
  AVAudioPCMBuffer* output_ = nil;
};

}  // namespace

VoiceCodecFactory AppleOpusCodecFactory() {
  VoiceCodecFactory factory;
  factory.encoder = []() -> std::unique_ptr<VoiceEncoder> {
    @autoreleasepool {
      auto encoder = std::make_unique<AppleOpusEncoder>();
      if (!encoder->Open()) {
        XELOGE("Party voice: Apple's Opus encoder isn't available");
        return nullptr;
      }
      return encoder;
    }
  };
  factory.decoder = []() -> std::unique_ptr<VoiceDecoder> {
    @autoreleasepool {
      auto decoder = std::make_unique<AppleOpusDecoder>();
      if (!decoder->Open()) {
        XELOGE("Party voice: Apple's Opus decoder isn't available");
        return nullptr;
      }
      return decoder;
    }
  };
  return factory;
}

}  // namespace voice
}  // namespace apu
}  // namespace xe
