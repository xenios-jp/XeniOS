/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_LIVE_ACCOUNT_H_
#define XENIA_NET_LIVE_ACCOUNT_H_

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xenia/net/packet_transport.h"

namespace xe::net::live {

// The device's key for the online service (lobby protocol version 2,
// "Accounts"): ECDSA P-256. Signing in is signing the lobby's challenge, so
// there's no password and no sign-up.
class DeviceKey {
 public:
  static DeviceKey Generate();
  // Nothing for a secret that isn't a P-256 private key.
  static std::optional<DeviceKey> FromSecret(const std::array<uint8_t, 32>& d);
  // Reads the key in |path|, or makes one and writes it there (mode 600,
  // whole or not at all; an empty file counts as none). Nothing when neither
  // works, or the file holds something else.
  static std::optional<DeviceKey> LoadOrCreate(const std::filesystem::path& path);

  const std::array<uint8_t, 32>& secret() const { return secret_; }
  // The uncompressed point, 04 || X || Y, as 130 hex digits.
  std::string PublicHex() const;
  // SHA-256 and ECDSA, the signature r || s as 128 hex digits; empty when
  // signing fails.
  std::string Sign(std::string_view text) const;

 private:
  std::array<uint8_t, 32> secret_{};
  std::array<uint8_t, 65> public_{};
};

// Checks a Sign() signature (tests).
bool VerifySignature(std::string_view public_hex, std::string_view text,
                     std::string_view signature_hex);

struct Friend {
  enum class State { kFriend, kIncoming, kOutgoing };
  uint64_t xuid = 0;
  std::string gamertag;
  State state = State::kFriend;
};

// A session friends may join.
struct Joinable {
  std::string room;  // Empty: the title's public lobby.
  std::string session_id;  // 16 hex digits, or empty.
  std::vector<uint8_t> info;  // XSESSION_INFO, or empty.
};

struct Presence {
  uint64_t xuid = 0;
  std::string gamertag;
  bool online = false;
  uint32_t title_id = 0;
  uint32_t context = 0;
  std::string rich;
  std::optional<Joinable> joinable;
};

struct Invite {
  std::string id;  // 16 hex digits.
  bool party = false;
  uint64_t from = 0;
  std::string gamertag;
  uint32_t title_id = 0;
  std::string room;
  std::vector<uint8_t> info;
  std::string message;
};
// The lobby drops an invite nobody answered after this long.
constexpr uint32_t kInviteLifetimeSeconds = 10 * 60;
// What the client keeps of the lobby's word, whatever it sends: an
// XSESSION_INFO is 0x3C bytes, ids and room codes are short, text is a line.
constexpr size_t kSessionInfoSize = 0x3C;
constexpr size_t kMaxRemoteId = 64;
constexpr size_t kMaxRemoteText = 256;
constexpr size_t kMaxPresence = 512;
constexpr size_t kMaxHeldInvites = 64;
// Online XUIDs are 0009xxxxxxxxxxxx.
constexpr bool IsOnlineXuid(uint64_t xuid) { return (xuid >> 48) == 0x0009; }

struct PartyState {
  std::string room;
  uint64_t leader = 0;
  std::vector<std::pair<uint64_t, std::string>> members;
  // The members' presence, for those the lobby says it of (xuid, title and
  // joinable; online). Empty from lobbies that don't.
  std::vector<Presence> presence;
  // What changed: "created", "joined", "left", "renamed" (or empty), and who.
  std::string event;
  uint64_t event_xuid = 0;
};

// What an app tells the lobby about the device it runs on, so the account's
// device list can name it. Either may be empty.
struct DeviceInfo {
  std::string name;  // Up to 32 characters, e.g. "Will's iPhone".
  std::string platform;  // "ios", "macos", "windows" or "linux".
};
// The platform from compile-time macros, and the host name up to its first dot
// (empty on iOS, which only says "localhost"). A UI may give a nicer name.
DeviceInfo DefaultDeviceInfo();

// One of the account's devices (the `devices` message).
struct Device {
  std::string key;  // The public key, as PublicHex().
  std::string name;  // Empty when the device never said.
  std::string platform;
  uint64_t added_ms = 0;  // Since the epoch.
  uint64_t seen_ms = 0;  // Last sign-in; accurate to an hour.
  bool current = false;  // The device of the socket that heard this.
};

// The answer to a name check. |status| is "available", "taken", "reserved",
// "not_allowed" or "invalid". |suggestions| are free names that pass every
// rule, with any status.
struct NameCheck {
  std::string gamertag;
  std::string status;
  std::vector<std::string> suggestions;
};

// Why the lobby didn't change the account's gamertag to the one asked for
// (welcome.refused).
struct Refused {
  std::string gamertag;  // The name asked for.
  // "too_soon", "taken", "invalid", "reserved", "not_allowed" or "banned".
  std::string reason;
  uint64_t next_ms = 0;  // For "too_soon": when a change is allowed again.
};

// Who may send the account friend requests (the lobby's `settings`):
// anyone, or only players who have its friend code (a code, link or QR code).
enum class FriendRequests { kAnyone, kCode };

// What the lobby says to a signed-in app, parsed.
struct AccountMessage {
  enum class Kind {
    kChallenge,
    kWelcome,
    kMoved,
    kReject,
    kFriend,
    kPresence,
    kInvite,
    kInviteState,
    kInviteAnswered,
    kParty,
    kBlocked,
    kProfile,
    kChecked,
    kRecovery,
    kLinkCode,
    kJoinPending,
    kJoinRequest,
    kJoinClosed,
    kSessionToken,
    kDevices,
    kReported,
    kNames,
    kFound,
    kResults,
    kSettings,
    kError,
    kOther,
  };
  Kind kind = Kind::kOther;
  std::string nonce;  // kChallenge.
  // kWelcome, kProfile and kMoved (the XUID to sign in with instead).
  uint64_t xuid = 0;
  std::string gamertag;
  std::string tag;
  std::string code;
  bool assigned = false;
  std::vector<Friend> friends;  // kWelcome; one for kFriend.
  bool removed = false;  // kFriend: the edge is gone.
  // kWelcome, kProfile and kBlocked: everyone this account blocks.
  std::vector<uint64_t> blocked;
  Presence presence;  // kPresence.
  Invite invite;  // kInvite.
  std::string invite_id;  // kInviteState, kInviteAnswered.
  std::string state;  // kInviteState.
  bool accept = false;  // kInviteAnswered.
  std::optional<PartyState> party;  // kParty; nothing once it's left.
  // kParty: the room a `left` names (the party left may not be the current
  // one: the lobby says so after the new party's message).
  std::string party_room;
  // kError: the player an invite was for (`to`).
  uint64_t to = 0;
  std::string reason;  // kError, kReject.
  // kReject "removed": the whole account was deleted, not just this device.
  bool deleted = false;
  // kReject "elsewhere": the name of the device the account signed in on
  // (empty when it gave none).
  std::string device_name;
  // kWelcome, kProfile: asked to pick another gamertag, with the reason
  // ("taken", "invalid", "reserved" or "not_allowed"); empty when not.
  std::string rename;
  // kWelcome, kProfile: when the next gamertag change is allowed (ms since the
  // epoch); 0 for now.
  uint64_t next_rename_ms = 0;
  std::optional<Refused> refused;  // kWelcome.
  // kWelcome (a new account only, once), kMoved (after a recovery) and
  // kRecovery: the recovery code, as the lobby wrote it (with dashes).
  std::string recovery;
  NameCheck checked;  // kChecked.
  uint32_t link_ttl_s = 0;  // kLinkCode, with |code|.
  // kJoinPending (|join_id|, |join_number| unless scan-only, |join_ttl_s|),
  // kJoinRequest (the same, plus |join_device| and |join_scanned|: asked for
  // with PeekJoin, no number) and kJoinClosed (|join_id|, |reason|).
  std::string join_id;
  std::string join_number;
  uint32_t join_ttl_s = 0;
  DeviceInfo join_device;
  bool join_scanned = false;
  // kSessionToken: the token for the sessions socket and how long it lives.
  std::string token;
  uint32_t token_ttl_s = 0;
  std::vector<Device> devices;  // kDevices.
  // kReported: the report's ID (8 hex digits); the player reported is |xuid|,
  // as it is for an error answering a report.
  std::string report_id;
  // kNames, kFound and kResults: the accounts the lobby named, by XUID
  // (gamertags cleaned; unknown XUIDs are left out).
  std::vector<std::pair<uint64_t, std::string>> users;
  // kResults: the text searched for.
  std::string prefix;
  // kSettings, and kWelcome from a lobby that has settings: who may send
  // friend requests. Nothing from a lobby without them (or a value it doesn't
  // know).
  std::optional<FriendRequests> requests;
  // kWelcome: the lobby has settings (and names), so it may be sent them.
  bool settings = false;
  // kError: the request it answers ("rename", "register", ...; may be empty),
  // the gamertag asked for (in |gamertag|), and for "too_soon" when to try
  // again.
  std::string for_request;
  uint64_t next_ms = 0;
};
// Lobby messages longer than this are ignored.
constexpr size_t kMaxLobbyMessage = 1 << 20;
// Nothing for text that isn't a JSON object with a "t", or is too long.
std::optional<AccountMessage> ParseAccountMessage(std::string_view text);

// The host the signatures name, as the lobby sees it in the request: the
// ws:// or wss:// address's host in lower case, with the port unless it's the
// scheme's own. Signing it keeps another lobby from relaying the challenge.
std::string LobbyHost(std::string_view lobby);

// A recovery code as the lobby wants it: what the player typed without
// dashes and spaces, in upper case. Empty unless that's 20 characters.
std::string NormalizeRecoveryCode(std::string_view typed);
// The same for a link code (8 characters).
std::string NormalizeLinkCode(std::string_view typed);

// A gamertag from the lobby, held to the rule the lobby itself holds names to
// (the Xbox 360's: 1 to 15 letters, digits and spaces). Anything else is
// dropped, so a name can't break a layout or pass itself off as another.
std::string CleanGamertag(std::string_view text);
// Other text another player chose (a device name): without control,
// zero-width or bidirectional-override characters, at most max_chars long.
std::string CleanText(std::string_view text, size_t max_chars);

// A name check before going online, for apps with no socket yet: POST
// EncodeNameCheckBody() to NameCheckUrl(lobby) and give the 200's body to
// ParseNameCheckResponse(). There's no HTTP client in this library, so the
// caller does the request (30 a minute per network; 429 means wait; debounce
// typing). Signed in, AccountClient::CheckName does the same.
std::string NameCheckUrl(std::string_view lobby);
std::string EncodeNameCheckBody(std::string_view gamertag);
std::optional<NameCheck> ParseNameCheckResponse(std::string_view gamertag,
                                                std::string_view body);
// The lobby's web address, http:// or https:// and its host, for the ws:// or
// wss:// address it's reached at.
std::string LobbyWebRoot(std::string_view lobby);

// The messages an app sends, as pure functions for tests. |nonce| is the
// challenge's; |host| is LobbyHost of the lobby the app connected to. |device|
// is left out of the message when it has nothing.
// |appear_offline| starts the sign-in appearing offline ("presence":
// {"offline": true}), so friends never see the account online.
std::string EncodeRegister(const DeviceKey& key, std::string_view host,
                           uint64_t xuid, std::string_view gamertag,
                           std::string_view nonce,
                           const DeviceInfo& device = {},
                           bool appear_offline = false);
// |passive|: only to check (AccountOptions::sign_in_only), so the account's
// session on another device goes on.
std::string EncodeAuth(const DeviceKey& key, std::string_view host,
                       uint64_t xuid, std::string_view nonce,
                       const DeviceInfo& device = {},
                       bool appear_offline = false, bool passive = false);
// A new device joining an account, before it is signed in. |code| is
// normalized by the caller (NormalizeLinkCode, NormalizeRecoveryCode).
std::string EncodeLink(const DeviceKey& key, std::string_view host,
                       std::string_view code, std::string_view nonce,
                       const DeviceInfo& device = {});
std::string EncodeRecover(const DeviceKey& key, std::string_view host,
                          std::string_view code, std::string_view nonce,
                          const DeviceInfo& device = {});
// What the signature of link or recover covers; |kind| is "link" or "recover".
std::string JoinSignedText(std::string_view kind, std::string_view host,
                           std::string_view code, std::string_view key,
                           std::string_view nonce);
// A new device asking a signed-in one to let it in. |gamertag| is empty for a
// request that is only for scanning. Signed with JoinSignedText("join", ...).
std::string EncodeJoinRequest(const DeviceKey& key, std::string_view host,
                              std::string_view gamertag, std::string_view nonce,
                              const DeviceInfo& device = {});
// What a signed-in device signs to approve request |id|; |nonce| is the
// challenge of the socket the approval goes on.
std::string ApproveSignedText(std::string_view host, std::string_view id,
                              std::string_view nonce);
std::string EncodeJoinApprove(const DeviceKey& key, std::string_view host,
                              std::string_view id, std::string_view nonce);
std::string EncodeJoinDeny(std::string_view id);
std::string EncodeJoinPeek(std::string_view id);
std::string EncodeCheckName(std::string_view gamertag);
std::string EncodeRename(std::string_view gamertag);
std::string EncodeRecoveryNew();
std::string EncodeLinkCode();
std::string EncodeSessionToken();
std::string EncodeDevices();
std::string EncodeRemoveDevice(std::string_view key);
std::string EncodeDeleteAccount();
// |offline| adds "offline": true: friends see the account as signed out.
std::string EncodePresence(uint32_t title_id, uint32_t context,
                           std::string_view rich,
                           const std::optional<Joinable>& joinable,
                           bool offline = false);
std::string EncodeFriendAdd(uint64_t xuid);
std::string EncodeFriendAddCode(std::string_view code);
std::string EncodeBlock(uint64_t xuid);
std::string EncodeUnblock(uint64_t xuid);
// Why a player may be reported: "cheating", "harassment", "offensive_name",
// "offensive_content", "spam" or "other" (the lobby refuses anything else).
bool IsReportReason(std::string_view reason);
// The most a report's note may hold, in UTF-16 code units (what the lobby
// counts, and what NSString's length is).
constexpr size_t kMaxReportNote = 500;
// A report's note as the lobby takes it: without the characters it calls
// unprintable (line breaks and tabs stay), trimmed, and cut to
// kMaxReportNote. Empty when nothing is left: a note of only spaces is
// refused.
std::string CleanReportNote(std::string_view note);
// A report of |xuid|. |note| is cleaned (CleanReportNote) and left out when
// empty; |title_id| when 0, and |room| unless it's a room code (6 characters
// of the lobby's alphabet). The lobby closes the socket on a field it
// refuses, so nothing it would refuse is sent.
std::string EncodeReport(uint64_t xuid, std::string_view reason,
                         std::string_view note, uint32_t title_id,
                         std::string_view room);
// The most XUIDs one `names` request may hold.
constexpr size_t kMaxNamesPerRequest = 16;
// A `names` request for |xuids|, or empty when the lobby would refuse it (and
// close the socket): none, more than kMaxNamesPerRequest, or one that isn't an
// online XUID (0009...).
std::string EncodeNames(const std::vector<uint64_t>& xuids);
// A search for gamertags starting with |prefix| (3 to 15 characters after
// trimming); empty when it's out of range.
std::string EncodeSearch(std::string_view prefix);
// `settings`: changes who may send friend requests, or with nothing only asks.
std::string EncodeSettings(std::optional<FriendRequests> requests);
// friend_decline or friend_remove for |xuid|.
std::string EncodeFriendEdit(std::string_view type, uint64_t xuid);
std::string EncodeInvite(uint64_t to, uint32_t title_id,
                         const Joinable& joinable, std::string_view message);
std::string EncodeInviteReply(std::string_view id, bool accept);
std::string EncodePartyCreate();
std::string EncodePartyLeave();
// An invite to the sender's party; |title_id| is the title the sender is in
// (the dashboard's when none).
std::string EncodePartyInvite(uint64_t to, uint32_t title_id);

std::string XuidHex(uint64_t xuid);

// What a join request's QR code holds, and the request's ID back from it
// (empty when |text| isn't one).
std::string JoinQrPayload(std::string_view id);
std::string ParseJoinQrPayload(std::string_view text);

// How an AccountClient joins an account that exists on another device, instead
// of registering one: with a link code that device made (RequestLinkCode), or
// the account's recovery code, or by asking its signed-in devices (kRequest).
struct AccountJoin {
  enum class Mode { kLink, kRecover, kRequest };
  Mode mode = Mode::kLink;
  // As typed; normalized when sent. For kRequest the gamertag of the account
  // to ask, or empty for a request that is only for scanning (a QR code).
  std::string code;
};

// A new device asking to join, as a signed-in device hears it.
struct JoinRequest {
  std::string id;
  DeviceInfo device;
  std::string number;  // Two digits to match; empty when |scanned|.
  uint32_t ttl_s = 0;
  bool scanned = false;  // The answer to PeekJoin: a QR code was scanned.
};

struct AccountOptions {
  // Set for a client that joins an account (see AccountJoin).
  std::optional<AccountJoin> join;
  // Sent on every sign-in; the account's device list shows it.
  DeviceInfo device = DefaultDeviceInfo();
  // Starts the client appearing offline (see SetAppearOffline).
  bool appear_offline = false;
  // Only signs in, never registers: a key that isn't one of the account's is
  // refused instead of given a new account or XUID. For a look at the account
  // that mustn't change it (a background check). The sign-in is passive: it
  // doesn't end the account's session on another device.
  bool sign_in_only = false;
};

// One profile's account on the lobby. Registers on every connect, which signs
// in when the device's key already owns the XUID; when another key owns it,
// the lobby moves the account to a new XUID. Connects again after the socket
// closes, and keeps what the lobby says. Thread safe; |Events| come on the
// socket's thread, without the client's lock held. Destroying the client waits
// for an event that's running, unless the event itself drops the last
// reference, so whatever an event touches outlives the client.
//
// For a UI:
// - Name check: CheckName() while signed in; before that, the HTTP form above.
// - Recovery code: Events::recovery_code, once for a new account (show it with
//   "write this down", don't keep it) and for NewRecoveryCode().
// - Devices: RequestDevices(), then Devices() or Events::devices_changed (which
//   also comes when another device links, recovers or is removed).
//   RemoveDevice() can remove the device in use; its client is then refused.
// - Add a device: on a signed-in device RequestLinkCode() and show
//   Events::link_code (8 characters, good for ttl_s, one use). On the new
//   device make a client with AccountOptions::join set (xuid 0) and
//   NormalizeLinkCode() of what was typed: Events::moved(xuid) says it
//   worked; write that XUID into the profile as its online XUID, destroy the
//   joining client and start the profile's own account. Events::join_failed
//   says why not and the client doesn't try again.
//   Recovering with the recovery code is the same with Mode::kRecover, and
//   Events::recovery_code gives the new code.
// - Rename prompt: after Events::signed_in (and Events::profile_changed) read
//   rename_prompt(); when it isn't empty ask for another name, once per
//   sign-in. Rename() answers with profile_changed, or rename_failed.
class AccountClient {
 public:
  struct Events {
    // Signed in, with the XUID the account has (another one when the lobby
    // assigned it) and its friend code.
    std::function<void(uint64_t xuid, const std::string& code)> signed_in;
    std::function<void()> friends_changed;
    std::function<void()> blocked_changed;
    std::function<void(const Presence&)> presence_changed;
    std::function<void(const Invite&)> invited;
    std::function<void()> party_changed;
    // The answer to CheckName. |status| as in NameCheck.
    std::function<void(const std::string& gamertag, const std::string& status,
                       const std::vector<std::string>& suggestions)>
        name_checked;
    // The answer to RequestSessionToken (SessionToken() has it too).
    std::function<void(const std::string& token, uint32_t ttl_s)>
        session_token;
    // A recovery code to show once: the new account's, one from
    // NewRecoveryCode, or the replacement after recovering. With dashes.
    std::function<void(const std::string& code)> recovery_code;
    // The answer to RequestLinkCode.
    std::function<void(const std::string& code, uint32_t ttl_s)> link_code;
    // A joining client (AccountJoin::Mode::kRequest) was heard: |id| is the
    // QR code's content (see JoinQrPayload), |number| what to show next to
    // it (empty for a scan-only request), good for |ttl_s|. Events::moved
    // follows when a device approves it, join_failed otherwise.
    std::function<void(const std::string& id, const std::string& number,
                       uint32_t ttl_s)>
        join_pending;
    // A signed-in device: another device asks to join (or PeekJoin's answer).
    // Answer with ApproveJoin or DenyJoin.
    std::function<void(const JoinRequest&)> join_requested;
    // A signed-in device: request |id| ended, for |reason| "approved",
    // "declined", "full", "expired" or "left"; its prompt can go.
    std::function<void(const std::string& id, const std::string& reason)>
        join_closed;
    // The device list was heard (Devices()).
    std::function<void()> devices_changed;
    // The account's gamertag, friend code, rename prompt or wait changed
    // (Rename() worked, another device renamed it, or the prompt went).
    std::function<void()> profile_changed;
    // Rename() was refused: "taken", "invalid", "reserved", "not_allowed",
    // "too_soon" (|next_ms|: when a change is allowed) or "banned".
    std::function<void(const std::string& reason, uint64_t next_ms)>
        rename_failed;
    // Another request was refused, e.g. request "link_code" and reason
    // "limit", or "remove_device" and "last_device". |request| may be empty.
    std::function<void(const std::string& request, const std::string& reason)>
        request_failed;
    // The lobby moved this client to |xuid|: a joining client (link or
    // recover) became a device of the account; otherwise the XUID asked for
    // belongs to another key and the account got a new one. Signs in on it
    // next.
    std::function<void(uint64_t xuid)> moved;
    // A joining client was refused, with the lobby's reason: "not_found"
    // (wrong, used or expired code), "denied", "full" (8 devices; the code is
    // used up), "limit" (too many wrong codes or requests from this network;
    // wait an hour), "declined" (a device said no), "expired" (nobody answered)
    // or "bad_request".
    std::function<void(const std::string& reason)> join_failed;
    // Going online: the lobby refused the gamertag a new account asked for
    // ("taken", "invalid", "reserved" or "not_allowed"). The socket stays open
    // and nothing was made: rename the profile and call RetryRegister().
    // After 5 refusals the lobby closes the socket; request_failed("register",
    // "limit") says so and the client stops.
    std::function<void(const std::string& gamertag, const std::string& reason)>
        register_refused;
    // Another device removed this one from the account. The client stops
    // connecting; the device needs a link or recovery code to come back.
    std::function<void()> removed;
    // The account was deleted, by this device or another. The client stops
    // connecting; the XUID can't be used again.
    std::function<void()> account_deleted;
    // The account signed in on another device (named |device|, or empty), which
    // ends its session here: one device at a time, as on the console. The
    // client stops connecting; a new client signs in again, with no code, and
    // ends the other device's session in turn.
    std::function<void(const std::string& device)> signed_in_elsewhere;
    // An invite the account had is gone: answered by one of its apps.
    std::function<void(const std::string& id)> invite_answered;
    // What became of an invite this app sent: "delivered", "accepted",
    // "declined" or "expired".
    std::function<void(const std::string& id, const std::string& state)>
        invite_state;
    // Signed in or out, or refused.
    std::function<void()> connection_changed;
    // The lobby stored a report of |xuid| (Report()), as |id|.
    std::function<void(const std::string& id, uint64_t xuid)> reported;
    // A report of |xuid| was refused: "duplicate" (reported already today),
    // "limit" (5 reports a day), "not_found" (no such account) or "invalid"
    // (yourself).
    std::function<void(uint64_t xuid, const std::string& reason)>
        report_failed;
    // The lobby named players asked for with RequestNames (GamertagOf() has
    // them from now on).
    std::function<void(
        const std::vector<std::pair<uint64_t, std::string>>& names)>
        names;
    // Who may send friend requests changed, or was heard for the first time
    // (FriendRequestsSetting()).
    std::function<void()> settings_changed;
    // A friend request (AddFriend) was refused for a reason about the player
    // asked: "not_allowed" (they take requests by friend code only). |gamertag|
    // is their name when this client knows it, else empty. Other refusals of
    // friend_add come as request_failed.
    std::function<void(uint64_t xuid, const std::string& gamertag,
                       const std::string& reason)>
        friend_add_refused;
    // Players whose gamertag starts with |prefix| (SearchPlayers), up to 20.
    // The lobby leaves out this account, blocked players and players who take
    // friend requests by code only.
    std::function<void(
        const std::string& prefix,
        const std::vector<std::pair<uint64_t, std::string>>& users)>
        search_results;
  };

  // |lobby| is the lobby's ws:// or wss:// address without a path.
  AccountClient(std::string lobby, DeviceKey key, uint64_t xuid,
                std::string gamertag, Events events,
                AccountOptions options = {});
  ~AccountClient();

  bool signed_in() const;
  // Why the lobby last refused or closed the connection; empty once signed
  // in.
  std::string problem() const;
  // Stopped for good: removed, signed in elsewhere, or a join that failed.
  bool halted() const;
  uint64_t xuid() const;
  std::string friend_code() const;
  std::vector<Friend> Friends() const;
  bool IsFriend(uint64_t xuid) const;
  std::optional<Presence> FriendPresence(uint64_t xuid) const;
  bool IsBlocked(uint64_t xuid) const;
  std::vector<uint64_t> Blocked() const;
  std::optional<PartyState> Party() const;
  // The account's gamertag as the lobby has it (empty before signing in).
  std::string gamertag() const;
  // Why the account is asked to pick another gamertag ("taken", "invalid",
  // "reserved" or "not_allowed"), or empty.
  std::string rename_prompt() const;
  // When the next gamertag change is allowed (ms since the epoch); 0: now.
  uint64_t next_rename_ms() const;
  // The gamertag the lobby refused at sign-in, if it did.
  std::optional<Refused> refused() const;
  // The account's devices, as last heard (RequestDevices).
  std::vector<Device> Devices() const;
  // Invites waiting for an answer, oldest first (expired ones left out).
  std::vector<Invite> Invites() const;

  // Names. The client keeps every gamertag it hears with its XUID: friends and
  // requests, party members, presence, invites and `names` answers.
  // GamertagOf() is empty for a player it never heard of.
  std::string GamertagOf(uint64_t xuid) const;
  // Asks the lobby for the gamertags of |xuids| it doesn't know yet
  // (Events::names). Batches of kMaxNamesPerRequest, at most
  // kNamesRequestsPerMinute a minute (the lobby allows 30 with friend requests
  // by gamertag); the rest wait their turn. A player asked for isn't asked
  // again for 10 minutes (the lobby leaves out XUIDs that have no account).
  // Sent only to a lobby that has names (one whose welcome carries settings);
  // others would close the socket.
  void RequestNames(const std::vector<uint64_t>& xuids);
  // Looks players up by the start of their gamertag (3 or more characters),
  // answered by search_results. Sent only to a lobby that has names, like
  // RequestNames.
  void SearchPlayers(const std::string& prefix);
  static constexpr uint32_t kNamesRequestsPerMinute = 10;

  // Who may send the account friend requests, as the lobby last said; nothing
  // before signing in, or from a lobby that has no settings.
  std::optional<FriendRequests> FriendRequestsSetting() const;
  // Changes it (Events::settings_changed comes with the lobby's answer, on
  // every app of the account). False, and nothing sent, when not signed in to
  // a lobby that has settings.
  bool SetFriendRequests(FriendRequests requests);

  void SetPresence(uint32_t title_id, uint32_t context, std::string rich,
                   std::optional<Joinable> joinable);
  // Appearing offline: friends see the account as signed out, while it still
  // signs in, joins games and gets invites. Kept by the client and sent with
  // every presence (whoever sets it: the app or a title) and with every
  // sign-in, so it lasts through title changes and reconnects.
  void SetAppearOffline(bool appear_offline);
  bool appear_offline() const;
  void AddFriend(uint64_t xuid);
  void AddFriendByCode(std::string code);
  // Declines their request.
  void DeclineFriend(uint64_t xuid);
  // Removes a friend, or cancels or declines a request.
  void RemoveFriend(uint64_t xuid);
  // Blocking also removes the friend or the request. The lobby answers with
  // the new list (Events::blocked_changed).
  void Block(uint64_t xuid);
  void Unblock(uint64_t xuid);
  // Reports a player (Events::reported or Events::report_failed). False, and
  // nothing sent, for a reason IsReportReason refuses. |title_id| and |room|
  // are the game and session it happened in (0 and empty for none). Blocking
  // them too is a separate Block().
  bool Report(uint64_t xuid, std::string reason, std::string note,
              uint32_t title_id, std::string room);
  void SendInvite(uint64_t to, uint32_t title_id, Joinable joinable,
                  std::string message);
  // The session this account last invited |xuid| to, if that was within
  // |window|: someone invited is likely on the way to it.
  std::optional<std::string> InvitedTo(
      uint64_t xuid, std::chrono::steady_clock::duration window) const;
  // Answers an invite, and forgets it. One the lobby no longer has (answered
  // here or by another app, replaced by a newer one from the same friend, or
  // expired) isn't answered.
  void ReplyInvite(std::string id, bool accept);
  // Party: a new one led by this account (leaving the one it's in), leaving
  // it, and inviting a friend to it. The lobby answers with a `party`
  // (Events::party_changed); accepting a party invite is ReplyInvite. An
  // invite goes to the party the lobby has for the account; when it has none
  // (it answers no_party), the client starts one and invites into it. A
  // relaunched app's party comes back from the lobby after signing in, so
  // the client never starts a party just because it hasn't heard of one yet.
  void CreateParty();
  void LeaveParty();
  void InviteToParty(uint64_t to, uint32_t title_id);

  // Asks whether |gamertag| is free (Events::name_checked). False when not
  // signed in: the check isn't kept for later.
  bool CheckName(std::string gamertag);
  // Changes the account's gamertag (Events::profile_changed, or
  // Events::rename_failed). A change of capitals only is free; any other waits
  // 30 days (next_rename_ms), unless the account was asked to pick another.
  void Rename(std::string gamertag);
  // Makes a code for another device to link with (Events::link_code); a newer
  // code replaces the older one. 10 an hour.
  void RequestLinkCode();
  // Answer a device's request to join (Events::join_requested). Only when
  // signed in: the approval is signed for this socket, so it isn't kept for
  // later. Events::join_closed follows; a refusal comes as request_failed
  // ("join_approve", "not_found", "denied", "full" or "limit").
  void ApproveJoin(std::string id);
  void DenyJoin(std::string id);
  // Asks about the request a scanned QR code names (Events::join_requested
  // with scanned set, or request_failed("join_peek", "not_found")).
  void PeekJoin(std::string id);
  // Asks for a token to sign in to the session directory with
  // (Events::session_token). 20 an hour; each is good for ttl_s (an hour).
  void RequestSessionToken();
  // The newest token, while it has at least 5 minutes left.
  std::optional<std::string> SessionToken() const;
  // Makes a new recovery code (Events::recovery_code) and kills the old one.
  // 5 an hour.
  void NewRecoveryCode();
  // After Events::register_refused: registers again on the same socket with
  // |gamertag| (also the name used from now on). Does nothing when signed in
  // or before the lobby's challenge.
  void RetryRegister(std::string gamertag);
  // Asks for the device list (Events::devices_changed).
  void RequestDevices();
  // Removes a device by its key (Device::key), but never the last one. Its
  // sockets are closed; everyone hears the new list.
  void RemoveDevice(std::string key);
  // The same for this device: the lobby closes this connection (Events::removed),
  // or refuses with request_failed("remove_device", "last_device").
  void RemoveThisDevice();
  // Deletes the whole account for good: account_deleted says when it's done,
  // request_failed("delete_account", ...) if nothing changed.
  void DeleteAccount();

 private:
  struct HeldInvite {
    Invite invite;
    std::chrono::steady_clock::time_point received;
  };

  void EnsureOpenLocked();
  void SendLocked(std::string text);
  // Makes |presence_message_| from the presence and appear-offline flag, and
  // sends it when it changed and the client is signed in.
  void UpdatePresenceLocked();
  // Keeps a gamertag heard from the lobby.
  void RememberNameLocked(uint64_t xuid, const std::string& gamertag);
  // Sends what RequestNames queued, as far as the rate allows.
  void SendNamesLocked();
  // How long until SendNamesLocked can send more of the queue; nothing when
  // there's nothing to send.
  std::optional<std::chrono::steady_clock::duration> NamesWaitLocked() const;
  void OnMessage(std::string text);
  void OnClosed(std::string reason);
  void Run();

  // Counts the socket's events that are running, so the destructor can wait
  // for them. Shared with the events, which can outlive the client.
  struct Calls {
    std::mutex mutex;
    std::condition_variable done;
    uint32_t running = 0;
    bool closed = false;
  };
  // Runs |call| unless the client is going away.
  static void Call(const std::shared_ptr<Calls>& calls,
                   const std::function<void()>& call);

  const std::string lobby_;
  const DeviceKey key_;
  std::string gamertag_;
  const Events events_;
  const DeviceInfo device_;
  const std::shared_ptr<Calls> calls_ = std::make_shared<Calls>();

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::unique_ptr<secure::LobbySocket> socket_;
  uint64_t generation_ = 0;
  bool signed_in_ = false;
  bool closed_ = false;
  bool stopping_ = false;
  // Sockets in a row that closed before staying signed in for 30 s; the wait
  // before connecting again grows with them.
  uint32_t failures_ = 0;
  std::chrono::steady_clock::time_point signed_in_at_;
  // The join this client still has to do, and whether it did or was refused.
  std::optional<AccountJoin> join_;
  bool joined_ = false;
  bool halted_ = false;
  // The challenge's nonce, for registering again on the same socket, and how
  // many gamertags the lobby refused on it.
  std::string nonce_;
  uint32_t register_refusals_ = 0;
  uint64_t xuid_;
  std::string code_;
  std::string account_gamertag_;
  std::string rename_;
  uint64_t next_rename_ms_ = 0;
  std::optional<Refused> refused_;
  std::vector<Device> devices_;
  // The newest session token and when it expires.
  std::string session_token_;
  std::chrono::steady_clock::time_point session_token_expires_;
  std::vector<Friend> friends_;
  std::vector<uint64_t> blocked_;
  std::vector<Presence> presence_;
  std::optional<PartyState> party_;
  // Party invites (to, title) waiting for the party they asked to create.
  std::vector<std::pair<uint64_t, uint32_t>> party_invites_waiting_;
  // Party invites sent while the party isn't known (after signing in, before
  // the lobby says the account's party): one the lobby answers with no_party
  // starts a party. Creating one at once instead would leave the party the
  // lobby still has for the account (a relaunch).
  std::vector<std::pair<uint64_t, uint32_t>> party_invites_unsure_;
  void PartyInviteRefusedLocked(uint64_t to);
  // Invite IDs the lobby no longer has (newest last).
  static constexpr size_t kGoneInvitesKept = 64;
  std::vector<std::string> gone_invites_;
  void ForgetInviteLocked(const std::string& id);
  // Game invites sent: the invitee, the session and when.
  struct SentInvite {
    std::string session_id;
    std::chrono::steady_clock::time_point at;
  };
  std::map<uint64_t, SentInvite> sent_invites_;
  std::vector<HeldInvite> invites_;
  std::string problem_;
  // The presence last set and whether to appear offline, which
  // |presence_message_| is made of. Sent again after signing in.
  uint32_t presence_title_id_ = 0;
  uint32_t presence_context_ = 0;
  std::string presence_rich_;
  std::optional<Joinable> presence_joinable_;
  bool appear_offline_ = false;
  // AccountOptions::sign_in_only.
  const bool sign_in_only_ = false;
  std::string presence_message_;
  std::vector<std::string> pending_;
  // Names: every gamertag heard, by XUID; the XUIDs waiting to be asked for,
  // in order; when each was asked (so it isn't asked again soon); when each of
  // the last minute's requests went, the last request's XUIDs (asked again
  // after a `limit`), and when the lobby said `limit`.
  std::unordered_map<uint64_t, std::string> names_;
  std::vector<uint64_t> names_queue_;
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point>
      names_asked_;
  std::vector<std::chrono::steady_clock::time_point> names_sent_;
  std::vector<uint64_t> names_last_;
  std::chrono::steady_clock::time_point names_paused_until_;
  // Set when the queue changed, so Run() looks at its wait again.
  bool names_wake_ = false;
  // The device's network changed (Run acts on it).
  bool network_changed_ = false;
  uint64_t network_listener_ = 0;
  // Whether the lobby has settings and names (its welcome said settings), and
  // what it said of them.
  bool lobby_has_settings_ = false;
  std::optional<FriendRequests> friend_requests_;
  std::chrono::steady_clock::time_point last_ping_;
  std::thread thread_;
};

}  // namespace xe::net::live

#endif  // XENIA_NET_LIVE_ACCOUNT_H_
