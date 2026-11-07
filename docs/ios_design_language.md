# XeniOS design language

How XeniOS's own screens look, behave and move: the library, the game page, Friends,
Profiles, Public Games and Settings, on iPhone and iPad. The values live in
`src/xenia/ui/ios/shared/apple_theme_tokens.cc`, reached through `XeniaTheme`
(`ios_theme.h`). Use a token, not a number. A value that is missing becomes a new token, not
a private constant in one view controller.

This is **one system for the whole app**. Every screen is built from the same backdrop, the
same two surfaces, the same group and row, and the same header and controls. A screen that
needs something else changes this document first.

> **Status.** This document describes the target. Some of it is built and some is not; the
> [Migration](#migration) section lists what the code still does differently. Where a token
> value here differs from the code, the [Shape and size](#shape-and-size) table shows both.

## The fourteen rules

### Every screen

1. **One backdrop.** Every screen sits on the colour wash of the game in context: the game
   you are looking at, or the library's featured game. The wash is that game's background
   art, blurred and saturated, under a dark gradient. Pages dim it; nothing is flat charcoal.
2. **Two surfaces.** A glass **card** (radius 34) for short things opened from a control:
   Friends, Profiles, Public Games. A full **page** on the wash for everything else: the
   game page, Settings, forms. There is no third kind: no charcoal sheet, no alert-style
   panel for content.
3. **One group, one row.** Content sits in glass groups (radius 22) of 52-pt rows. Hairlines
   start at the text, not the edge. Row symbols are plain 22-pt SF Symbols in white 85%.
   Colour is only for state: the mint accent, compatibility dots, Not Set Up. No coloured
   icon tiles.
4. **One header, no exceptions.** ✕ is always top right and closes whatever was presented:
   Settings, Friends, Profiles, Public Games, the game page and forms. ‹ is always top left,
   only on pages pushed inside something, and those keep ✕ too. Extra actions (⋯, +, search)
   sit just before ✕. A form commits with the one white button at the bottom ("Create
   Profile", "Send Report"), never ✓ or Save in the header.
5. **One set of controls.** 44-pt round glass controls. One white capsule per place, for the
   main action (Play, Join, Quick Join, Create Profile). Glass capsules for everything else.
   Status is a dot and words, never a pill.
6. **Same in landscape.** A leading column for identity (art, title, you) and a trailing
   column for the groups. Never a narrow centred column of portrait UI.
7. **One page, two handles.** Tap a tile, or press A, to play. ⋯ on the library hero opens
   that game into its game page, in place. Hold a tile, or press X, to make it the hero and
   open its game page the same way. There is no long-press menu. Patches, Updates & Add-ons,
   System Link and Convert to ZAR live only on the game page and push inside it.
8. **People and rooms.** The Friends pill opens **people**: online friends, invites and
   parties. The globe opens **Public Games**: a browser of open rooms, and its mint count is
   the number of players on XeniOS. Two jobs, two doors, one card design.

### iPad and resizable windows

9. **Size class, not device.** Regular width gets the iPad layout. Compact width gets the
   phone layout: every iPhone, Split View, Slide Over and small Stage Manager windows. Rotating
   or resizing switches it live.
10. **Cards anchor.** Friends, Profiles and Public Games are the same glass card, anchored
    under their button and at most 820 pt wide. The library stays visible behind, lightly
    dimmed. ✕, a tap outside or Esc closes the card.
11. **Push becomes side by side.** Where iPhone pushes a list into its detail, iPad shows
    both: games and rooms in Public Games, sections and page in Settings. ‹ appears only when
    there is no room for both.
12. **Readable width.** Groups never stretch past 420–680 pt. Extra width goes to art and to
    columns, never to longer rows. The library grid keeps the phone's tile size and adds
    columns (six on an 11-inch iPad in landscape).
13. **Pointer and keyboard.** Rows and controls get the system hover highlight. ⌘F
    searches, ⌘, opens Settings, Esc is ✕ and Return is the white button. Game controllers
    work as on iPhone.
14. **Same parts.** There are no iPad-only components. Only placement changes.

## Surfaces

| Surface | Value | Where |
|---|---|---|
| Wash | The game's background art, blur 46, saturation 2.1, brightness 0.78, under a black gradient (5% at 25% height to 40% at the bottom) | Behind every page and card |
| Hero art | The same art, sharp, masked to transparent from 45% down | Top of the game page and the library hero |
| Page dim | Black 40–45% over the wash | Settings and other pages without hero art |
| Card | iOS 26 Liquid Glass (`UIGlassEffect` regular, tinted `glassCardTint`), its own rim; `XEInstallGlassCardSurface` | Friends, Profiles, Public Games |
| `glassDim` | Black 42% (iPad: 28%) | Behind a card. A tap on it closes the card |
| Group | `glassGroupFill` with a light blur, top highlight white 16% | Every row group |
| Glass control | White 13%, blur 16, 0.5-pt highlight | Round controls, glass capsules, search field, segmented control |

The library's featured game supplies the wash for the library and its cards. Settings uses
the featured game's wash as well, dimmed. A game page uses its own game.

## Shape and size

| Token | Target | Code today | Use |
|---|---|---|---|
| `glassCardRadius` | 34, continuous | 34 | Card |
| `glassGroupRadius` | 22, continuous | 22 | Row group |
| `glassCardInset` | 12 | 12 | Card to the screen edge (iPhone) |
| `glassRowMinHeight` | 52 | 52 | A row in a group |
| `glassTileSize` | 36 | 36 | Avatars in rows (always a circle). Icon tiles are gone: rows use a bare 22-pt symbol |
| `glassControlSize` | 44 | 44 | Round glass controls: ✕, ‹, ⋯, +, search, the globe |
| `glassGroupFill` | White 9% | White 9% | Group fill |
| `glassHairline` | White 14%, **0.5 pt, from the text** | White 14%, 1 px | Between rows |
| Large control (new) | 50 | — | Play and the round controls beside it on the game page |
| Capsule height (new) | 34 (Join), 50 (Play), 52 wide button | — | See Controls |
| Card max width, iPad (new) | 820 | — | Anchored cards |

Every corner uses `kCACornerCurveContinuous`. The Online panel multiplies sizes by its panel
scale (`S()`: 1.0 over a game, 1.15 on the home screen). Tokens go through `S()` there.

## Type

All type is the system font (SF), scaled with Dynamic Type through
`xe_scaled_system_font`. Body text is never below 13 pt.

| Role | Size / weight | Colour |
|---|---|---|
| Page large title | 32 bold (34 on iPad), leading | `textPrimary` |
| Card title | 28 bold, leading, on the ✕ line | `textPrimary` |
| Pushed page title | 17 semibold, centred between ‹ and ✕ | `textPrimary` |
| Game title on the game page | 40 heavy (60 on iPad), soft shadow over art | `textPrimary` |
| Subtitle under a title | 15 regular | white 70% |
| Row name | 16 medium | `textPrimary` |
| Row detail | 13 regular | white 62% |
| Row value (trailing) | 16 regular | white 60% |
| Section caption | 13 semibold, sentence case, outside the group | white 62% |
| Group footer | 13 regular, one sentence | white 55% |
| Capsule title | 14 semibold (17 on Play) | black on white, `textPrimary` on glass |
| Count badge (`XECountBadge`) | 11 bold monospaced digits, 18-pt bubble on the button's 45-degree corner point | `accentFg` on `accent` for online counts; white on #0A84FF for something waiting |

Captions use sentence case ("Online now"), never small caps or uppercase. Live counts use
tabular digits so they don't jitter.

## Colour roles

| Colour | Role |
|---|---|
| `accent` (#34D399 mint) | You (your avatar and profile), a switch that is on, Playable, online counts. Never iOS system green #30D158 for a fill |
| `accent` dot with a 2-pt ring | A friend who is online |
| #0A84FF system blue | Invites, anything you were *sent*, and their count badges |
| `statusWarning` | Not Set Up, needs attention |
| System red | Destructive actions only |
| Avatar palette (six muted hues, hashed by name) | Other players: a steady colour per name everywhere |
| White 85% | Row symbols. The `sectionIcon*` tile hues are retired |

## Headers and controls

- **Header line.** 44 pt high, 16 pt from the sides (14 inside a card). Leading: ‹ on a
  pushed page, or the title on a card. Trailing: extra actions, then ✕ last. A page with
  a large title puts it below the header line; a card puts its title on the line.
- **Round control.** SF Symbol (19 pt, weight 2.2) in a 44-pt glass circle, with an
  accessibility label always ("Close", "Back", "More", "Public Games").
- **White capsule.** The one main action in a place: Play (50 high), Join (34), or a wide
  bottom button (52, radius 26, 16 from the edges) such as Quick Join, Create Profile, Send
  Report. Black text.
- **Glass capsule.** Every other button and filter chip. A selected chip turns white.
- **Segmented control.** Glass track 34 high; the selected half is white 22%.
- **Search field.** Glass, 36 high, radius 18, magnifier and placeholder at white 55%.

## Components

- **Card** (`XeniaIOSOnlineSheetViewController`): radius 34, inset 12. Sized to its content
  up to the safe area, then scrolls under a pinned header. Grows from its button.
- **Page**: the wash, the header line, then content. The game page adds hero art and a
  title block (title, status dot and words, Play with Touch Layout and Game Settings beside
  it) above its groups.
- **Row**: a leading symbol (22) or avatar (36), name over detail, then one trailing thing:
  a value with a chevron, a switch, one capsule, or a round control. A row has at most one
  capsule.
- **Room row** (Public Games): host avatar when a friend hosts, room name (wraps, never
  truncates), a three-bar connection meter with mode and "7 of 16", then Join. A full room
  is dimmed to 45% and shows "Full" in place of Join.
- **Game row** (Public Games): 30-pt cover, game name, "38 rooms · 214 players", chevron.
- **State** (empty, filtered, offline): a 34-pt symbol, one bold 16 line, one plain 14
  line, at most one button. Inside a group, never an empty group.

## Public Games at any size

Public Games has to stay calm at 1,000+ players and hundreds of rooms changing every second.

- **Group by game.** The first page lists games, one row each, busiest first, never every
  room. 2 rooms or 2,000, it is still one row per game.
- **Friends first.** Rooms with friends in them lead, collapsed into one row at the top.
- **Compact numbers.** Full numbers in text (1,284); compact on badges (1.2K, then 99K+).
- **Stable lists.** Live changes never move rows under a finger. New rooms wait behind a
  "↑ 6 new rooms" pill; full rooms dim and sink to the end.
- **Load as you scroll.** Rooms come from the server 50 at a time and the list is
  virtualised, so 10,000 rooms cost the same as 50.
- **One fast path.** Quick Join, the white bottom button on a game's rooms, picks the best
  open room by connection and fit. Browsing is for when you care which room.
- **Filters are chips**, not a form: Mode, Has space, Best connection. A filter that empties
  the list says so and offers the way out ("Show Full Rooms").
- **Offline** says what still works: "Can't reach Public Games. Your games still work
  offline." with Try Again.

## Accessibility

- **Dynamic Type**: everything scales; rows grow taller rather than truncating.
- **Reduce Transparency**: system materials go opaque on their own. Custom fills switch
  through tokens: groups to #2C2C2E, cards to #1C1C1E. The wash becomes a solid tint taken
  from the art; hero art stays.
- **Increase Contrast**: secondary text goes from white 62% to 80%, hairlines get stronger.
- **Reduce Motion**: cards cross-fade instead of growing and shrinking.
- **VoiceOver**: every round control has a label; a room row reads as one element ("Spartan
  Ops nightly, Firefight, 3 of 4, good connection, Join").

## Motion

| Moment | Motion |
|---|---|
| Card opens | Grows from the control that opened it. Spring 0.55 s, damping 0.82. Radius animates from a pill to 34 over 0.45 s. Contents fade in after 0.14 s over 0.25 s. The dim fades in with the spring |
| Card closes | Shrinks evenly to 50% while moving 60% of the way back to the control, and fades out, on a 0.3 s critically damped spring. Contents go first, in 0.12 s. The blur fades by animating its effect to nil, because a blur ignores its parent's alpha. Never a non-uniform squash |
| Game page opens | The hero expands in place into the page; a held tile first becomes the hero, then expands the same way |
| Push inside a page or card | Slides in from the trailing edge; the art and wash stay still |
| Live list change | New rows wait behind the pill; nothing reorders under a finger |

## Migration

What the code still does differently from this document:

- ~~**Tokens**~~ (done 2026-11-06): group radius 22, control size 44, group fill 9%,
  hairline 14%. The `sectionIcon*` palette is gone; rows draw `XERowSymbolImage`, a bare
  symbol in white 85%, coloured only for a state. Still to do: hairlines that start at the
  text, and the 0.5-pt width.
- **Long-press menu** on library tiles (`contextMenuConfigurationForItemAtIndexPath` in
  `ios_launcher_overlay_view.mm`): remove it; hold and X open the game page instead.
- **Sheets on `glassSheet`** (Online Account, Move Profile): move to the wash.
  ~~Profiles~~ (done 2026-11-06): a card from the avatar (`XEPrepareGlassCard`,
  `ios_glass_card.h`), + then ✕, "On this iPhone" and "Online" groups. ~~Settings~~ (done 2026-11-06): on the wash
  (`XEUseArtWash`, `ios_art_wash.h`), large leading title, profile row first, Community last.
- ~~**Friends card**~~ (done 2026-11-06): people only. One "Online now" group, "Play together"
  below it, no Public Games row; empty states use the one shape (symbol, bold line, plain
  line) in a glass group.
- ~~**Public Games**~~ (done 2026-11-06): the glass card from the globe. Subtitle with the
  player count, In My Library / All Games, friends in games as one row, games busiest first
  with full counts, the filtered and offline states with their one button; the globe's badge
  goes compact (1.2K, 99K+). Still to do, and it needs the lobby first: the lobby only
  reports players per game, not rooms, so a game's own rooms page (room rows, Join, Quick
  Join, the new-rooms pill, filter chips, 50 at a time) waits on a rooms API. Until then a
  game row opens that game's System Link sheet.
- ~~**✕ placement**~~ (done 2026-11-06): every sheet sets its ✕ with `XESetSheetClose` and its
  extra actions with `XESetSheetActions` (`ios_view_helpers.h`); pages pushed inside a sheet
  pick up the root's ✕. Forms commit with `XESetFormCommit`, the white bottom button above the
  keyboard (New Profile, Add Friend, the gamertag, code, recovery and approval forms).
- **iPad**: the library uses the phone layout scaled up today (four ~330-pt covers, the globe
  in the search row). Settings already uses `UISplitViewController`.
- **Reduce Transparency and Increase Contrast** have no handling under `src/xenia/ui/ios`
  yet.
