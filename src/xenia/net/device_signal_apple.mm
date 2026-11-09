/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/device_signal_apple.h"

#include <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <IOKit/IOKitLib.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "xenia/base/logging.h"

namespace xe::net {
namespace {

using Secret = std::array<uint8_t, 32>;

// SHA-256 of |label| and |size| bytes of |data|, as 64 hex digits.
std::string HashedHex(const char* label, const void* data, size_t size) {
  uint8_t digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_CTX sha;
  CC_SHA256_Init(&sha);
  CC_SHA256_Update(&sha, label, static_cast<CC_LONG>(std::strlen(label)));
  CC_SHA256_Update(&sha, data, static_cast<CC_LONG>(size));
  CC_SHA256_Final(digest, &sha);
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  for (const uint8_t byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 15]);
  }
  return hex;
}

NSDictionary* SecretItem() {
  return @{
    (id)kSecClass : (id)kSecClassGenericPassword,
    (id)kSecAttrService : @"xenios.device-signal",
    (id)kSecAttrAccount : @"secret",
  };
}

OSStatus ReadSecret(Secret& secret) {
  NSMutableDictionary* query = [[SecretItem() mutableCopy] autorelease];
  query[(id)kSecReturnData] = @YES;
  query[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
  CFTypeRef found = nullptr;
  const OSStatus status = SecItemCopyMatching((CFDictionaryRef)query, &found);
  if (status != errSecSuccess) {
    return status;
  }
  NSData* data = (NSData*)found;
  const bool usable = [data isKindOfClass:[NSData class]] && data.length == secret.size();
  if (usable) {
    std::memcpy(secret.data(), data.bytes, secret.size());
  }
  CFRelease(found);
  return usable ? errSecSuccess : errSecDecode;
}

// Only when the Keychain says there is no secret. It also fails before the first
// unlock, and a new secret then would make this look like a different device.
OSStatus CreateSecret(Secret& secret) {
  if (SecRandomCopyBytes(kSecRandomDefault, secret.size(), secret.data()) != errSecSuccess) {
    return errSecAllocate;
  }
  NSMutableDictionary* item = [[SecretItem() mutableCopy] autorelease];
  item[(id)kSecValueData] = [NSData dataWithBytes:secret.data() length:secret.size()];
  // ThisDeviceOnly: not in backups, so a restored copy doesn't share it.
  item[(id)kSecAttrAccessible] = (id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
  const OSStatus status = SecItemAdd((CFDictionaryRef)item, nullptr);
  return status == errSecDuplicateItem ? ReadSecret(secret) : status;
}

}  // namespace

std::string KeychainDeviceSignal() {
  static std::mutex mutex;
  static std::string signal;
  std::lock_guard<std::mutex> lock(mutex);
  if (!signal.empty()) {
    return signal;
  }
  @autoreleasepool {
    Secret secret;
    OSStatus status = ReadSecret(secret);
    if (status == errSecItemNotFound) {
      status = CreateSecret(secret);
    }
    if (status != errSecSuccess) {
      XELOGW("Live account: no Keychain device signal ({})", static_cast<int>(status));
      return {};
    }
    signal = HashedHex("xenios-device-signal\n", secret.data(), secret.size());
  }
  return signal;
}

#if TARGET_OS_OSX
std::string MachineDeviceSignal() {
  static std::mutex mutex;
  static std::string signal;
  std::lock_guard<std::mutex> lock(mutex);
  if (!signal.empty()) {
    return signal;
  }
  // MACH_PORT_NULL is the default main port on every macOS version.
  const io_service_t service =
      IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOPlatformExpertDevice"));
  if (!service) {
    return {};
  }
  CFTypeRef value =
      IORegistryEntryCreateCFProperty(service, CFSTR(kIOPlatformUUIDKey), kCFAllocatorDefault, 0);
  IOObjectRelease(service);
  if (!value) {
    return {};
  }
  if (CFGetTypeID(value) == CFStringGetTypeID()) {
    char uuid[64] = {};
    if (CFStringGetCString(static_cast<CFStringRef>(value), uuid, sizeof(uuid),
                           kCFStringEncodingUTF8)) {
      signal = HashedHex("xenios-machine-signal\n", uuid, std::strlen(uuid));
    }
  }
  CFRelease(value);
  return signal;
}
#endif

}  // namespace xe::net
