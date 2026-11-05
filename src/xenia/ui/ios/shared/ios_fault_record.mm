/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_fault_record.h"

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "xenia/base/exception_handler.h"
#include "xenia/base/guest_crash_info.h"
#include "xenia/ui/ios/shared/ios_crash_context.h"

// Exported so the crash reporter extension can find it by name.
extern "C" __attribute__((used, visibility("default")))
xe_ios_crash_context_t xe_ios_crash_context = {XE_IOS_CRASH_CONTEXT_MAGIC,
                                               XE_IOS_CRASH_CONTEXT_VERSION};

namespace {

constexpr char kRecordHeader[] = "xenios-fault 1\n";

int g_record_fd = -1;
char g_image_uuid[40];
uint64_t g_launch_ticks = 0;
mach_timebase_info_data_t g_timebase = {1, 1};
std::atomic<bool> g_fault_recorded{false};

// The last kMaxEvents events, oldest overwritten: slot g_event_count % size.
constexpr uint32_t kMaxEvents = 16;
struct EventSlot {
  std::atomic<uint32_t> event{0};
  std::atomic<uint32_t> ms{0};
};
EventSlot g_events[kMaxEvents];
std::atomic<uint32_t> g_event_count{0};

uint64_t MillisecondsSinceLaunch() {
  return (mach_absolute_time() - g_launch_ticks) * g_timebase.numer / g_timebase.denom / 1000000;
}

NSString* RecordPath() {
  return [NSHomeDirectory()
      stringByAppendingPathComponent:@"Library/Application Support/XeniOS/Metrics/fault.txt"];
}

// XPACI: strips pointer authentication from a return address saved by arm64e
// system code. ARMv8.3, which every device iOS 26 runs on has.
uint64_t StripPointerAuth(uint64_t value) {
  register uint64_t x16 asm("x16") = value;
  asm volatile(".inst 0xdac143f0" : "+r"(x16));
  return x16;
}

// Reads the executable's header address, __TEXT size and LC_UUID into the
// crash context and g_image_uuid.
void ReadImageInfo() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    for (uint32_t i = 0, count = _dyld_image_count(); i < count; ++i) {
      const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
      if (!header || header->filetype != MH_EXECUTE) {
        continue;
      }
      xe_ios_crash_context.image_base = reinterpret_cast<uint64_t>(header);
      const auto* command = reinterpret_cast<const load_command*>(header + 1);
      for (uint32_t j = 0; j < header->ncmds; ++j) {
        if (command->cmd == LC_SEGMENT_64) {
          const auto* segment = reinterpret_cast<const segment_command_64*>(command);
          if (!std::strcmp(segment->segname, SEG_TEXT)) {
            xe_ios_crash_context.image_size = segment->vmsize;
          }
        } else if (command->cmd == LC_UUID) {
          const uint8_t* u = reinterpret_cast<const uuid_command*>(command)->uuid;
          std::snprintf(g_image_uuid, sizeof(g_image_uuid),
                        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11],
                        u[12], u[13], u[14], u[15]);
        }
        command = reinterpret_cast<const load_command*>(
            reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
      }
      break;
    }
  });
}

void CopyString(char* destination, size_t capacity, NSString* source) {
  const char* utf8 = source ? source.UTF8String : "";
  const size_t length = std::strlen(utf8);
  if (length >= capacity) {
    destination[0] = '\0';
    return;
  }
  std::memcpy(destination, utf8, length + 1);
}

// Async-signal-safe from here to OnUnhandledFault's end: no allocation, no
// locks, no stdio. Memory is read with vm_read_overwrite, which fails instead
// of faulting on a bad frame pointer.
bool ReadFrameRecord(uint64_t address, uint64_t record[2]) {
  vm_size_t size = 0;
  return vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(address), 16,
                           reinterpret_cast<vm_address_t>(record), &size) == KERN_SUCCESS &&
         size == 16;
}

uint32_t WalkFrames(uint64_t fp, uint64_t* frames, uint32_t max_frames) {
  uint32_t count = 0;
  while (count < max_frames && fp && !(fp & 7)) {
    uint64_t record[2];
    if (!ReadFrameRecord(fp, record)) {
      break;
    }
    const uint64_t return_address = StripPointerAuth(record[1]);
    if (!return_address) {
      break;
    }
    frames[count++] = return_address;
    // Callers' frames are higher on the same stack; anything else ends the
    // walk (a fiber switch, a corrupt chain, a JIT frame without a record).
    if (record[0] <= fp || record[0] - fp > (UINT64_C(64) << 20)) {
      break;
    }
    fp = record[0];
  }
  return count;
}

struct RecordWriter {
  char text[4096];
  size_t length = 0;

  void Append(const char* s) {
    while (*s && length < sizeof(text)) {
      text[length++] = *s++;
    }
  }
  void AppendNumber(uint64_t value, unsigned base) {
    char digits[24];
    int count = 0;
    do {
      digits[count++] = "0123456789abcdef"[value % base];
      value /= base;
    } while (value);
    while (count && length < sizeof(text)) {
      text[length++] = digits[--count];
    }
  }
  void Hex(const char* key, uint64_t value) {
    Append(key);
    Append(" 0x");
    AppendNumber(value, 16);
    Append("\n");
  }
  void Decimal(const char* key, uint64_t value) {
    Append(key);
    Append(" ");
    AppendNumber(value, 10);
    Append("\n");
  }
};

char AccessLetter(xe::Exception::AccessViolationOperation operation) {
  switch (operation) {
    case xe::Exception::AccessViolationOperation::kRead:
      return 'r';
    case xe::Exception::AccessViolationOperation::kWrite:
      return 'w';
    default:
      return 'u';
  }
}

void OnUnhandledFault(int signal_number, uint64_t fault_address,
                      const xe::HostThreadContext* context,
                      xe::Exception::AccessViolationOperation operation) {
  // The first one only: a second thread faulting meanwhile is a consequence.
  if (g_fault_recorded.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  xe_ios_crash_context_t& c = xe_ios_crash_context;
  uint64_t thread_id = 0;
  pthread_threadid_np(nullptr, &thread_id);
  c.fault_thread_id = thread_id;
  c.fault_address = fault_address;
  c.fault_pc = context->pc;
  c.fault_lr = StripPointerAuth(context->x[30]);
  c.fault_sp = context->sp;
  c.fault_fp = context->x[29];
  c.fault_uptime_ms = MillisecondsSinceLaunch();
  c.fault_frame_count = WalkFrames(c.fault_fp, c.fault_frames, XE_IOS_CRASH_CONTEXT_MAX_FRAMES);
  // Last: a nonzero signal tells the extension the snapshot is complete.
  c.fault_signal = static_cast<uint32_t>(signal_number);

  if (g_record_fd < 0) {
    return;
  }
  RecordWriter w;
  w.Append(kRecordHeader);
  w.Append("launch ");
  w.Append(c.launch_id);
  w.Append("\n");
  w.Decimal("signal", c.fault_signal);
  w.Hex("fault_address", c.fault_address);
  w.Hex("pc", c.fault_pc);
  w.Hex("lr", c.fault_lr);
  w.Decimal("uptime_ms", c.fault_uptime_ms);
  w.Hex("title", c.title_id);
  w.Append("image_uuid ");
  w.Append(g_image_uuid);
  w.Append("\n");
  w.Hex("image_base", c.image_base);
  w.Hex("image_size", c.image_size);
  w.Hex("jit_base", c.jit_base);
  w.Hex("jit_size", c.jit_size);
  for (uint32_t i = 0; i < c.fault_frame_count; ++i) {
    w.Hex("frame", c.fault_frames[i]);
  }
  // A fetch from the fault address itself is an execute fault.
  const char access[2] = {
      fault_address == context->pc ? 'x' : AccessLetter(operation), '\0'};
  w.Append("access ");
  w.Append(access);
  w.Append("\n");
  // The name stays on the device: the next launch turns it into a role.
  char thread_name[64] = {};
  pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
  for (char& ch : thread_name) {
    ch = ch == '\n' ? ' ' : ch;
  }
  w.Decimal("thread_main", pthread_main_np() == 1);
  w.Append("thread_name ");
  w.Append(thread_name);
  w.Append("\n");
  xe::GuestCrashInfo guest = {};
  if (xe::GetGuestCrashInfo(*context, fault_address, &guest)) {
    w.Hex("guest_thread", guest.thread_id);
    w.Hex("guest_function", guest.function);
    w.Hex("guest_pc", guest.pc);
    w.Hex("guest_lr", guest.lr);
    w.Decimal("guest_via_helper", guest.via_helper);
    for (uint32_t i = 0; i < guest.frame_count; ++i) {
      w.Hex("guest_frame", guest.frames[i]);
    }
    if (guest.fault_in_guest) {
      w.Hex("guest_fault", guest.fault_address);
    }
  }
  const uint32_t event_count = g_event_count.load(std::memory_order_acquire);
  for (uint32_t i = event_count > kMaxEvents ? event_count - kMaxEvents : 0; i < event_count;
       ++i) {
    const EventSlot& slot = g_events[i % kMaxEvents];
    w.Append("event ");
    w.AppendNumber(slot.event.load(std::memory_order_relaxed), 10);
    w.Append(" ");
    w.AppendNumber(slot.ms.load(std::memory_order_relaxed), 10);
    w.Append("\n");
  }
  w.Append("end\n");
  (void)::write(g_record_fd, w.text, w.length);
  ::fsync(g_record_fd);
}

const char* SignalName(int signal_number) {
  switch (signal_number) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGBUS:
      return "SIGBUS";
    case SIGILL:
      return "SIGILL";
    case SIGTRAP:
      return "SIGTRAP";
    default:
      return "other";
  }
}

NSString* HexString(uint64_t value) {
  return [NSString stringWithFormat:@"0x%llx", static_cast<unsigned long long>(value)];
}

NSString* Hex8(uint64_t value) {
  return [NSString stringWithFormat:@"%08llX", static_cast<unsigned long long>(value & 0xFFFFFFFF)];
}

NSString* EventName(uint64_t event) {
  switch (event) {
    case XeIOSEventGameStart:
      return @"game-start";
    case XeIOSEventFirstFrame:
      return @"first-frame";
    case XeIOSEventShaderBurst:
      return @"shader-burst";
    case XeIOSEventMemoryWarning:
      return @"memory-warning";
    case XeIOSEventBackground:
      return @"background";
    case XeIOSEventForeground:
      return @"foreground";
    case XeIOSEventThermalSerious:
      return @"thermal-serious";
    case XeIOSEventThermalCritical:
      return @"thermal-critical";
    case XeIOSEventJitFailure:
      return @"jit-failure";
    case XeIOSEventGameStop:
      return @"game-stop";
    default:
      return nil;
  }
}

// What a thread does, from the name Xenia gave it (xe::threading::Thread::
// set_name). Guest threads may carry names the game chose; those only ever
// read as "guest".
NSString* ThreadRole(NSString* name, BOOL main_thread, BOOL guest_thread) {
  if (main_thread) {
    return @"main";
  }
  if (guest_thread || [name hasPrefix:@"XThread"] || [name hasPrefix:@"Main XThread"] ||
      [name rangeOfString:@"^.* \\([0-9A-F]{8}\\)$" options:NSRegularExpressionSearch].location !=
          NSNotFound) {
    return @"guest";
  }
  static NSArray<NSArray*>* const roles = [@[
    @[ @"gpu", @[ @"GPU Commands", @"GPU Frame limiter", @"Boot Animation" ] ],
    @[ @"shader", @[ @"Shader Translation", @"Shader Analysis", @"Shader Storage Writer",
                     @"Metal Shaders" ] ],
    @[ @"audio", @[ @"XMA Decoder", @"Audio Worker", @"XAudio2", @"Audio Media Player",
                    @"APU Pacing Driver" ] ],
    @[ @"kernel", @[ @"Kernel Dispatch", @"DPC Processor", @"Guest Scheduler",
                     @"xe::threading::TimerQueue" ] ],
    @[ @"io", @[ @"Guest I/O", @"STFS", @"FATX" ] ],
    @[ @"network", @[ @"Xenia Socket I/O", @"XNet" ] ],
    @[ @"input", @[ @"Gamepad Poll", @"SDL Input" ] ],
    @[ @"logging", @[ @"Logging Writer" ] ],
    @[ @"emulator", @[ @"Emulator" ] ],
  ] retain];
  for (NSArray* role in roles) {
    for (NSString* prefix in role[1]) {
      if ([name hasPrefix:prefix]) {
        return role[0];
      }
    }
  }
  return @"other";
}

}  // namespace

NSDictionary* xe_ios_fault_record_take_previous(void) {
  NSString* path = RecordPath();
  NSString* text = [NSString stringWithContentsOfFile:path
                                             encoding:NSUTF8StringEncoding
                                                error:nil];
  [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
  // A record cut short (the process died while writing it) isn't used.
  if (![text hasPrefix:@(kRecordHeader)] || ![text hasSuffix:@"\nend\n"]) {
    return nil;
  }
  NSMutableDictionary<NSString*, NSString*>* fields = [NSMutableDictionary dictionary];
  std::vector<uint64_t> addresses;
  NSMutableArray* guest_frames = [NSMutableArray array];
  NSMutableArray* events = [NSMutableArray array];
  for (NSString* line in [text componentsSeparatedByString:@"\n"]) {
    NSRange space = [line rangeOfString:@" "];
    if (space.location == NSNotFound) {
      continue;
    }
    NSString* key = [line substringToIndex:space.location];
    NSString* value = [line substringFromIndex:space.location + 1];
    if ([key isEqualToString:@"frame"]) {
      addresses.push_back(std::strtoull(value.UTF8String, nullptr, 16));
    } else if ([key isEqualToString:@"guest_frame"]) {
      [guest_frames addObject:Hex8(std::strtoull(value.UTF8String, nullptr, 16))];
    } else if ([key isEqualToString:@"event"]) {
      NSArray<NSString*>* parts = [value componentsSeparatedByString:@" "];
      NSString* name = parts.count == 2 ? EventName(std::strtoull(parts[0].UTF8String, nullptr, 10))
                                        : nil;
      if (name) {
        [events addObject:@{@"e" : name, @"ms" : @(std::strtoull(parts[1].UTF8String, nullptr, 10))}];
      }
    } else {
      fields[key] = value;
    }
  }
  auto number = [fields](NSString* key) -> uint64_t {
    NSString* value = fields[key];
    return value ? std::strtoull(value.UTF8String, nullptr, 0) : 0;
  };
  const uint64_t image_base = number(@"image_base");
  const uint64_t image_size = number(@"image_size");
  const uint64_t jit_base = number(@"jit_base");
  const uint64_t jit_size = number(@"jit_size");
  const uint64_t pc = number(@"pc");
  const uint64_t lr = number(@"lr");
  // The pc, then the link register when the faulting function hadn't saved
  // it in a frame record yet (a leaf), then the frame-pointer walk.
  addresses.insert(addresses.begin(), pc);
  if (lr && (addresses.size() < 2 || addresses[1] != lr)) {
    addresses.insert(addresses.begin() + 1, lr);
  }
  NSMutableArray* frames = [NSMutableArray array];
  for (uint64_t address : addresses) {
    if (address >= image_base && address - image_base < image_size) {
      [frames addObject:@{@"image" : @"XeniOS", @"offset" : HexString(address - image_base)}];
    } else if (jit_size && address >= jit_base && address - jit_base < jit_size) {
      [frames addObject:@{@"image" : @"<jit>", @"offset" : HexString(address - jit_base)}];
    } else {
      [frames addObject:@{@"image" : @"system"}];
    }
  }
  const int signal_number = static_cast<int>(number(@"signal"));
  NSMutableDictionary* record = [NSMutableDictionary dictionary];
  record[@"signal"] = @(SignalName(signal_number));
  record[@"faultAddress"] = HexString(number(@"fault_address"));
  record[@"inJit"] = @(jit_size && pc >= jit_base && pc - jit_base < jit_size);
  record[@"frames"] = frames;
  if (const uint64_t title = number(@"title")) {
    record[@"titleId"] = [NSString stringWithFormat:@"%08llX", static_cast<unsigned long long>(title)];
  }
  if (fields[@"launch"].length) {
    record[@"launchId"] = fields[@"launch"];
  }
  if (fields[@"image_uuid"].length) {
    record[@"binaryUUID"] = fields[@"image_uuid"];
  }
  NSString* access = @{@"r" : @"read", @"w" : @"write", @"x" : @"execute"}[fields[@"access"] ?: @""];
  if (fields[@"access"]) {
    record[@"access"] = access ?: @"unknown";
  }
  const uint64_t guest_thread = number(@"guest_thread");
  if (fields[@"thread_main"]) {
    record[@"thread"] = ThreadRole(fields[@"thread_name"] ?: @"", number(@"thread_main") != 0,
                                   guest_thread != 0);
  }
  if (fields[@"guest_fault"]) {
    record[@"guestFaultAddress"] = Hex8(number(@"guest_fault"));
  }
  if (fields[@"guest_pc"]) {
    NSMutableDictionary* guest = [NSMutableDictionary dictionary];
    if (guest_thread) {
      guest[@"threadId"] = Hex8(guest_thread);
    }
    for (NSString* key in @[ @"function", @"pc", @"lr" ]) {
      if (const uint64_t value = number([@"guest_" stringByAppendingString:key])) {
        guest[key] = Hex8(value);
      }
    }
    if (guest_frames.count) {
      guest[@"frames"] = guest_frames;
    }
    guest[@"viaHelper"] = @(number(@"guest_via_helper") != 0);
    if (guest[@"pc"] || guest[@"lr"] || guest_frames.count) {
      record[@"guest"] = guest;
    }
  }
  if (events.count) {
    record[@"events"] = events;
  }
  return record;
}

void xe_ios_fault_record_install(NSString* launch_id, NSString* endpoint,
                                 NSData* envelope_json) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    mach_timebase_info(&g_timebase);
    g_launch_ticks = mach_absolute_time();
    ReadImageInfo();
    xe_ios_crash_context_t& c = xe_ios_crash_context;
    CopyString(c.launch_id, sizeof(c.launch_id), launch_id);
    CopyString(c.endpoint, sizeof(c.endpoint), endpoint);
    if (envelope_json.length < sizeof(c.envelope_json)) {
      std::memcpy(c.envelope_json, envelope_json.bytes, envelope_json.length);
      c.envelope_json[envelope_json.length] = '\0';
    }
    NSString* path = RecordPath();
    [[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent]
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    g_record_fd = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                       0600);
    xe::ExceptionHandler::SetUnhandledObserver(OnUnhandledFault);
  });
}

void xe_ios_fault_record_note_event(XeIOSEvent event) {
  if (!g_launch_ticks) {
    return;
  }
  // A crash meanwhile may read this slot half written: one event off.
  EventSlot& slot = g_events[g_event_count.fetch_add(1, std::memory_order_acq_rel) % kMaxEvents];
  slot.ms.store(static_cast<uint32_t>(MillisecondsSinceLaunch()), std::memory_order_relaxed);
  slot.event.store(event, std::memory_order_relaxed);
}

void xe_ios_fault_record_note_title(uint32_t title_id) {
  __atomic_store_n(&xe_ios_crash_context.title_id, title_id, __ATOMIC_RELAXED);
}

void xe_ios_fault_record_note_jit_range(uint64_t base, uint64_t size) {
  __atomic_store_n(&xe_ios_crash_context.jit_base, base, __ATOMIC_RELAXED);
  __atomic_store_n(&xe_ios_crash_context.jit_size, size, __ATOMIC_RELAXED);
}

void xe_ios_fault_record_note_upload_allowed(BOOL allowed) {
  __atomic_store_n(&xe_ios_crash_context.upload_allowed, allowed ? 1u : 0u, __ATOMIC_RELAXED);
}

NSString* xe_ios_fault_record_binary_uuid(void) {
  ReadImageInfo();
  return g_image_uuid[0] ? @(g_image_uuid) : nil;
}
