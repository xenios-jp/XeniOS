/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_FAULT_RECORD_H_
#define XENIA_UI_IOS_SHARED_IOS_FAULT_RECORD_H_

#import <Foundation/Foundation.h>

#include <cstdint>

// Records a crash Xenia's exception handler didn't claim, from inside the
// signal handler: into a file opened in advance, read at the next launch for
// the crash record, and into the crash context (ios_crash_context.h) the iOS 27
// crash reporter extension reads.

// The previous launch's fault, if it recorded one, with code addresses as
// image offsets: {"signal", "faultAddress", "inJit", "titleId", "uptimeS",
// "binaryUUID", "frames": [{"image", "offset"}]}. Deletes the file. Call
// before xe_ios_fault_record_install().
NSDictionary* xe_ios_fault_record_take_previous(void);

// Fills the crash context, opens the record file and registers with
// xe::ExceptionHandler. Once, at launch.
void xe_ios_fault_record_install(NSString* launch_id, NSString* endpoint,
                                 NSData* envelope_json);

// Kept current in the crash context. Thread-safe.
void xe_ios_fault_record_note_title(uint32_t title_id);
void xe_ios_fault_record_note_jit_range(uint64_t base, uint64_t size);
void xe_ios_fault_record_note_upload_allowed(BOOL allowed);

// The app executable's LC_UUID, e.g. "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX".
NSString* xe_ios_fault_record_binary_uuid(void);

#endif  // XENIA_UI_IOS_SHARED_IOS_FAULT_RECORD_H_
