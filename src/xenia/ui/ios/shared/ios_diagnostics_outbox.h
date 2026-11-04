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
// XeniOS/Outbox (not backed up): one file per record,
//   {"build", "device", "kind", "id", "body"},
// written already redacted. build and device describe the launch the record
// is about, which for a crash found at launch is the previous one. Crashes are
// kept 14 days, sessions 48 hours; over 50 records or 2 MB, the oldest sessions
// go first, then the oldest crashes. Records are queued and sent only while
// Online Features and Share Diagnostics are both on, never while a game runs,
// and only by a build with an endpoint (Info.plist XeniOSDiagnosticsURL).
// At most 5 crash records a day are queued (a crash loop sends no more), and
// not a crash whose top 3 XeniOS frames in the same binary were queued in the
// last 7 days. Sessions are queued only while this device is in the Worker's
// share: a fixed random 32-bit draw, kept once made, that is a multiple of the
// last X-Diag-Sessions-One-In (N, a power of two up to 1024; 1 until a 204 or
// 503 says). Each session is sent with "weight": N, the N when it goes, and
// queued sessions are dropped if the device leaves the share.
//
// They go out as schema 2 batches (POST, gzip):
//   {"schema": 2, "build", "device", "records": [{"kind", "id", "body"}]},
// one batch per kind and header, at most 50 sessions or 10 crashes and 64 KB
// compressed (256 KB of JSON), several in turn if more are queued.

// Queues a record about this launch. |kind| is session or crash. Strings in
// |body| are redacted for upload. Thread-safe; returns before the file is
// written. A record too large to send even alone is dropped.
void xe_ios_diagnostics_enqueue(NSString* kind, NSDictionary* body);
// The same for a record about another launch: |header| is that launch's
// {"build", "device"}, or nil for this launch's.
void xe_ios_diagnostics_enqueue_with_header(NSString* kind, NSDictionary* body,
                                            NSDictionary* header);

// Sends what is queued, batch after batch, in the background, if allowed now
// and not waiting. Called at launch and on coming to the foreground only.
// Crashes go at once; sessions together, at the first chance after the UTC day
// they were played (their "day"; sooner with 40 queued, the very first at
// once), so the Worker writes one daily row for a day's sessions of a game
// rather than one each. By the Worker's answer:
// - 204 (stored), 400 or 413 (malformed, never resent): the batch is deleted
//   (a 204 or 503's X-Diag-Sessions-One-In is kept);
// - 503 with Retry-After (crashes refused for the day): kept, no crash batch
//   sent for that many seconds, session batches go on;
// - anything else (no network, 429, another 5xx, a proxy or captive portal's
//   200 or redirect): kept, nothing sent for 5 min, then 30 min, 2 h and 12 h
//   after further failures in a row, then not until the next launch.
void xe_ios_diagnostics_upload_pending(void);

// Deletes every queued record.
void xe_ios_diagnostics_clear_outbox(void);

// The batches that would be sent now, as pretty-printed JSON, in sending order
// (Settings > About > Privacy > Pending Reports).
NSArray<NSString*>* xe_ios_diagnostics_pending_reports(void);
// The number of queued records.
NSUInteger xe_ios_diagnostics_pending_count(void);

// The endpoint, or nil if this build has none.
NSURL* xe_ios_diagnostics_endpoint(void);

// {"build": {...}, "device": {...}}: the batch header describing this launch's
// build (appVersion, buildNumber, commitShort, binaryUUID) and the device.
NSDictionary* xe_ios_diagnostics_envelope_header(void);

// The header for a record about an earlier launch: |logged|, that launch's
// own header as the launch log kept it, or nil. |binary_uuid|, the binary the
// record itself names (a fault's, a MetricKit frame's), or nil, wins when it
// differs: the header is then this launch's if it is the same binary, else the
// first of |known| (other launches' headers, newest first) with that binary,
// else a build of only that UUID. Without either, the build is unknown and
// left out ({}) rather than guessed, and so is the device's OS, which may have
// been updated since.
NSDictionary* xe_ios_diagnostics_header_for_earlier_launch(NSDictionary* logged,
                                                           NSString* binary_uuid,
                                                           NSArray<NSDictionary*>* known);

// |text| after upload redaction: strip_pii, then file names under ~/Documents,
// ~/Library and ~/tmp, any other path, game file names, quoted text and e-mail
// addresses.
NSString* xe_ios_diagnostics_redact(NSString* text);

// Creates a directory for diagnostics files, excluded from device backups.
void xe_ios_diagnostics_create_directory(NSString* path);

#endif  // XENIA_UI_IOS_SHARED_IOS_DIAGNOSTICS_OUTBOX_H_
