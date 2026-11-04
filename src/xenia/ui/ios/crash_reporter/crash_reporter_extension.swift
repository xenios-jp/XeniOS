/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XeniOS's crash reporter extension (iOS 27): the system runs it after the app
// crashes, with the crashed process's read-only task port. It reads the app's
// crash context (ios_crash_context.h, imported as the bridging header), which
// holds the consent flag, the endpoint and the envelope fields, so it needs no
// App Group and no settings of its own. It sends one schema 2 batch
// (ios_diagnostics_outbox.h) of one "crash" record with on-device symbolicated
// frames, the system's included, under the crashed launch's own build and
// device, and nothing at all unless the app had diagnostics sharing on.

import CrashReportExtension
import Darwin
import Foundation

private let maxThreads = 64
private let maxFrames = 48

// Symbols that mark the thread that brought the process down, when Xenia's
// handler didn't record the fault itself (abort, a failed assertion, a trap).
private let crashingThreadMarkers = [
  "__pthread_kill", "abort", "__abort_with_payload", "_os_crash", "__assert_rtn",
  "swift_reportError", "_assertionFailure",
]

private func readMemory<T>(_ task: mach_port_t, _ address: UInt64, as type: T.Type) -> T? {
  let size = MemoryLayout<T>.size
  let buffer = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: MemoryLayout<T>.alignment)
  defer { buffer.deallocate() }
  var outSize: vm_size_t = 0
  let result = vm_read_overwrite(
    task, vm_address_t(address), vm_size_t(size), vm_address_t(UInt(bitPattern: buffer)), &outSize)
  guard result == KERN_SUCCESS, Int(outSize) == size else {
    return nil
  }
  return buffer.load(as: T.self)
}

private func string<T>(fromTuple tuple: T) -> String {
  withUnsafeBytes(of: tuple) { bytes in
    let end = bytes.firstIndex(of: 0) ?? bytes.count
    return String(decoding: bytes[..<end], as: UTF8.self)
  }
}

// RFC 1952 around the raw deflate stream of Foundation's zlib compression.
private func gzip(_ data: Data) -> Data? {
  guard let deflated = try? (data as NSData).compressed(using: .zlib) as Data else {
    return nil
  }
  var crc: UInt32 = 0xffff_ffff
  for byte in data {
    crc ^= UInt32(byte)
    for _ in 0..<8 {
      crc = (crc >> 1) ^ (0xedb8_8320 & (0 &- (crc & 1)))
    }
  }
  var out = Data([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff])
  out.append(deflated)
  withUnsafeBytes(of: (~crc).littleEndian) { out.append(contentsOf: $0) }
  withUnsafeBytes(of: UInt32(truncatingIfNeeded: data.count).littleEndian) {
    out.append(contentsOf: $0)
  }
  return out
}

private func hex(_ value: UInt64) -> String {
  "0x" + String(value, radix: 16)
}

// Return addresses saved by arm64e system code are signed; user addresses fit
// in 39 bits even with extended virtual addressing, the signature is above.
private func stripPointerAuth(_ value: UInt64) -> UInt64 {
  value & 0x0000_007F_FFFF_FFFF
}

private func walkFrames(_ task: mach_port_t, fp start: UInt64) -> [UInt64] {
  var frames: [UInt64] = []
  var fp = start
  while frames.count < maxFrames, fp != 0, fp & 7 == 0 {
    guard let record = readMemory(task, fp, as: (UInt64, UInt64).self) else {
      break
    }
    let returnAddress = stripPointerAuth(record.1)
    if returnAddress == 0 {
      break
    }
    frames.append(returnAddress)
    if record.0 <= fp || record.0 - fp > (64 << 20) {
      break
    }
    fp = record.0
  }
  return frames
}

private struct ThreadState {
  var pc: UInt64
  var lr: UInt64
  var fp: UInt64
}

private func threadStates(_ task: mach_port_t) -> [ThreadState] {
  var threads: thread_act_array_t?
  var count: mach_msg_type_number_t = 0
  guard task_threads(task, &threads, &count) == KERN_SUCCESS, let threads else {
    return []
  }
  defer {
    vm_deallocate(
      mach_task_self_, vm_address_t(UInt(bitPattern: threads)),
      vm_size_t(Int(count) * MemoryLayout<thread_act_t>.stride))
  }
  var states: [ThreadState] = []
  for index in 0..<min(Int(count), maxThreads) {
    var state = arm_thread_state64_t()
    var stateCount = mach_msg_type_number_t(
      MemoryLayout<arm_thread_state64_t>.size / MemoryLayout<UInt32>.size)
    let result = withUnsafeMutablePointer(to: &state) {
      $0.withMemoryRebound(to: integer_t.self, capacity: Int(stateCount)) {
        thread_get_state(threads[index], ARM_THREAD_STATE64, $0, &stateCount)
      }
    }
    if result == KERN_SUCCESS {
      states.append(
        ThreadState(
          pc: stripPointerAuth(state.__pc), lr: stripPointerAuth(state.__lr), fp: state.__fp))
    }
  }
  return states
}

@main
struct XeniOSCrashReporter: CrashReporterExtension {
  func processCrashReport(process: CrashedProcess) {
    let task = process.corpsePort
    let address = process.symbolAddress(imageName: nil, symbolName: "xe_ios_crash_context")
    guard address != 0,
      let context = readMemory(task, address, as: xe_ios_crash_context_t.self),
      context.magic == XE_IOS_CRASH_CONTEXT_MAGIC,
      context.version >= 1,
      context.upload_allowed == 1
    else {
      return
    }
    guard let url = endpoint(crashedProcessEndpoint: string(fromTuple: context.endpoint)),
      let headerData = string(fromTuple: context.envelope_json).data(using: .utf8),
      let header = try? JSONSerialization.jsonObject(with: headerData) as? [String: Any]
    else {
      return
    }

    // The fault Xenia's handler recorded, or the thread that aborted.
    var addresses: [UInt64] = []
    if context.fault_signal != 0 {
      addresses.append(context.fault_pc)
      let walked = withUnsafeBytes(of: context.fault_frames) { bytes in
        Array(bytes.bindMemory(to: UInt64.self).prefix(Int(context.fault_frame_count)))
      }
      if context.fault_lr != 0, walked.first != context.fault_lr {
        addresses.append(context.fault_lr)
      }
      addresses += walked
    } else {
      for state in threadStates(task) {
        let names = process.symbolicateAddress(state.pc).map(\.symbol)
        if names.contains(where: { name in crashingThreadMarkers.contains { name.contains($0) } }) {
          addresses = [state.pc, state.lr] + walkFrames(task, fp: state.fp)
          break
        }
      }
    }

    let images = process.binaryImages
    let symbolicated = process.symbolicateAddresses(addresses)
    var frames: [[String: Any]] = []
    for (index, address) in addresses.enumerated() {
      var frame: [String: Any] = [:]
      if context.jit_size != 0, address >= context.jit_base,
        address - context.jit_base < context.jit_size
      {
        frame["image"] = "<jit>"
        frame["offset"] = hex(address - context.jit_base)
      } else if let image = images.first(where: {
        address >= $0.baseAddress && address - $0.baseAddress < $0.size
      }) {
        frame["image"] = (image.path as NSString).lastPathComponent
        frame["offset"] = hex(address - image.baseAddress)
        if let symbol = symbolicated[index].last(where: { !$0.isInline }) ?? symbolicated[index].last {
          frame["symbol"] = symbol.symbol
        }
      } else {
        frame["image"] = "unknown"
      }
      frames.append(frame)
    }

    var body: [String: Any] = [
      "source": "extension",
      "type": context.fault_signal != 0 ? "fault" : "exception",
      "exceptionType": Int(process.reason.exception),
      "exceptionCodes": process.reason.codes.map(hex),
      "frames": frames,
    ]
    let launchId = string(fromTuple: context.launch_id)
    if !launchId.isEmpty {
      body["launchId"] = launchId
    }
    if context.title_id != 0 {
      body["titleId"] = String(format: "%08X", context.title_id)
    }
    if context.fault_signal != 0 {
      body["signal"] = Int(context.fault_signal)
      body["faultAddress"] = hex(context.fault_address)
      body["inJit"] =
        context.jit_size != 0 && context.fault_pc >= context.jit_base
        && context.fault_pc - context.jit_base < context.jit_size
    }

    var batch = header
    batch["schema"] = 2
    batch["records"] = [["kind": "crash", "id": UUID().uuidString, "body": body]]
    guard let json = try? JSONSerialization.data(withJSONObject: batch), let compressed = gzip(json)
    else {
      return
    }
    send(compressed, to: url)
  }

  // The app's endpoint, from its Info.plist rather than the crashed process's
  // memory, which may be corrupt. A local test server the process names is
  // used as is: a test harness sets one, and it can't leave the device.
  private func endpoint(crashedProcessEndpoint: String) -> URL? {
    if let local = URL(string: crashedProcessEndpoint),
      local.host == "localhost" || local.host == "127.0.0.1"
    {
      return local
    }
    // XeniOS.app/PlugIns/<extension>.appex (or Extensions/).
    let appURL = Bundle.main.bundleURL.deletingLastPathComponent().deletingLastPathComponent()
    guard let value = Bundle(url: appURL)?.object(forInfoDictionaryKey: "XeniOSDiagnosticsURL")
        as? String,
      let url = URL(string: value), url.scheme == "https"
    else {
      return nil
    }
    return url
  }

  // The extension has seconds to finish; one try, no retries.
  private func send(_ gzipped: Data, to url: URL) {
    let configuration = URLSessionConfiguration.ephemeral
    configuration.allowsConstrainedNetworkAccess = false
    configuration.timeoutIntervalForRequest = 10
    configuration.timeoutIntervalForResource = 15
    let session = URLSession(configuration: configuration)
    var request = URLRequest(url: url)
    request.httpMethod = "POST"
    request.setValue("application/json", forHTTPHeaderField: "Content-Type")
    request.setValue("gzip", forHTTPHeaderField: "Content-Encoding")
    request.httpBody = gzipped
    let done = DispatchSemaphore(value: 0)
    session.dataTask(with: request) { _, _, _ in done.signal() }.resume()
    _ = done.wait(timeout: .now() + 15)
    session.invalidateAndCancel()
  }
}
