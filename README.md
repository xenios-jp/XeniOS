<p align="center">
    <img height="256px" src="assets/apple/xenios-readme-icon.png" alt="XeniOS app icon" />
</p>

<h1 align="center">XeniOS - Xbox 360 Emulator with advanced touch controller support/customizability + touchpad support</h1>

Adds a alternative advanced touch interface with full customizability in mind

-New customize controller section in the in game menu
-Can move, rotate, change size, color, text, shape of any button.
-U can remove the buttons color and hide buttons
-U can fully delete text/symbols on a button
-You can enable touchpad mode which makes entire right side of screen a touchpad (in beta lol). You can adjust the sensitivity of it using most games built in setting for camera sensitivity.
-Some performance optimizations for all the touch stuff/ui


TO BUILD:

git submodule update --init --recursive

./tools/build_apple_release.sh --skip-macos-arm64 --skip-macos-x86_64 --ios-min 16.0
<img width="1210" height="834" alt="IMG_1028" src="https://github.com/user-attachments/assets/fe15455e-3b56-43fd-a88c-440fd5f0f7d3" />

<p align="center">
  <a href="https://xenios.jp">Website</a> ◦
  <a href="https://github.com/xenios-jp/XeniOS/releases">Releases</a> ◦
  <a href="https://xenios.jp/docs">Docs</a> ◦
  <a href="https://xenios.jp/faq">FAQ</a> ◦
  <a href="https://xenios.jp/compatibility">Compatibility</a> ◦
  <a href="https://discord.gg/QwcTtNKTGf">Discord</a> ◦
  <a href="https://github.com/xenios-jp/XeniOS/issues">Issues</a>
</p>

## Current Focus

- iOS
- macOS (Apple Silicon)
- macOS (Intel)

Current published releases focus on iOS and macOS.
For Windows or Linux builds, use [Xenia Edge](https://github.com/has207/xenia-edge) or [Xenia Canary](https://github.com/xenia-canary/xenia-canary).

## Why This Fork Exists

Xenia development is relatively thinly staffed right now, and upstream is not
set up for fast iteration on Apple-specific packaging, documentation, release
flow, and user experience. XeniOS exists so that work can move faster in a
repository where the full product experience can be shaped directly, from the
app itself to releases, docs, compatibility reporting, and the public website
at [xenios.jp](https://xenios.jp).

The goal is not to keep good work siloed here forever. The goal is to iterate
quickly, build a more polished user-facing experience for Apple platforms, and
then contribute the useful technical improvements back upstream into the
broader Xenia community, especially
[Xenia Canary](https://github.com/xenia-canary/xenia-canary).

## Downloads

Download XeniOS from
[GitHub Releases](https://github.com/xenios-jp/XeniOS/releases).

- [Latest GitHub release](https://github.com/xenios-jp/XeniOS/releases/latest)
- [All GitHub releases](https://github.com/xenios-jp/XeniOS/releases)

## Quickstart

Start with the public docs at [xenios.jp/docs](https://xenios.jp/docs).

## FAQ

See the public FAQ at [xenios.jp/faq](https://xenios.jp/faq).

## Game Compatibility

Browse currently tracked games on
[xenios.jp/compatibility](https://xenios.jp/compatibility).

To file a compatibility report, use the
[GitHub compatibility tracker](https://github.com/xenios-jp/game-compatibility/issues/new/choose).

## Building

See [building.md](docs/building.md) for setup and information about the
`xb` script. When writing code, check the [style guide](docs/style_guide.md)
and be sure to run clang-format!

## Contributors Wanted!

Have some spare time, know advanced C++, and want to write an emulator?
Contribute! There's a ton of work that needs to be done, a lot of which
is wide open greenfield fun.

**For general rules and guidelines please see [CONTRIBUTING.md](.github/CONTRIBUTING.md).**

Fixes and optimizations are always welcome, especially around Apple platform
performance, UI polish, compatibility coverage, packaging, and tooling.

Start with the
[XeniOS issue tracker](https://github.com/xenios-jp/XeniOS/issues),
join the [XeniOS Discord](https://discord.gg/QwcTtNKTGf), check
[CONTRIBUTING.md](.github/CONTRIBUTING.md), and coordinate before starting
larger work.

## Disclaimer

The goal of this project is to experiment, research, and educate on the topic
of emulation of modern devices and operating systems. **It is not for enabling
illegal activity**. All information is obtained via reverse engineering of
legally purchased devices and games and information made public on the internet
(you'd be surprised what's indexed on Google...).
