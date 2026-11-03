/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_DIAGNOSTICS_OUTBOX_H_
#define XENIA_UI_IOS_SHARED_IOS_DIAGNOSTICS_OUTBOX_H_

#import <Foundation/Foundation.h>

// Diagnostics records waiting to be sent, in Library/Application Support/
// XeniOS/Outbox (not backed up): one JSON envelope per file,
//   {"schema": 1, "kind", "id", "build", "device", "body"},
// written already redacted and capped at 50 records, 2 MB and 14 days. They
// are queued and sent only while Online Features and Share Diagnostics are
// both on, never while a game runs, and only by a build with an endpoint
// (Info.plist XeniOSDiagnosticsURL).

// Queues a record. |kind| is session or crash. Strings in |body| are redacted
// for upload. Thread-safe; returns before the file is written.
void xe_ios_diagnostics_enqueue(NSString* kind, NSDictionary* body);

// Sends what is queued, in the background, if allowed now and not backing off
// after a failure. A record is deleted once the server accepts it or rejects
// it as malformed, and kept for a later try on anything else (no network, a
// server error, a proxy or captive portal answering).
void xe_ios_diagnostics_upload_pending(void);

// Deletes every queued record.
void xe_ios_diagnostics_clear_outbox(void);

// The queued records as pretty-printed JSON, oldest first, exactly as they
// would be sent (Settings > About > Privacy > Pending Reports).
NSArray<NSString*>* xe_ios_diagnostics_pending_reports(void);
NSUInteger xe_ios_diagnostics_pending_count(void);

// The endpoint, or nil if this build has none.
NSURL* xe_ios_diagnostics_endpoint(void);

// {"build": {...}, "device": {...}}: the envelope fields describing this app
// and device.
NSDictionary* xe_ios_diagnostics_envelope_header(void);

// |text| after upload redaction: strip_pii, then file names under ~/Documents,
// ~/Library and ~/tmp, any other path, game file names, quoted text and e-mail
// addresses.
NSString* xe_ios_diagnostics_redact(NSString* text);

// Creates a directory for diagnostics files, excluded from device backups.
void xe_ios_diagnostics_create_directory(NSString* path);

#endif  // XENIA_UI_IOS_SHARED_IOS_DIAGNOSTICS_OUTBOX_H_
