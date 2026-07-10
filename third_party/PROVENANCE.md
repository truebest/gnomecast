# Third-party dependency provenance

This file records native webOS dependencies that are pinned for the native-only
`gnomecast` target. The historical browser app, JavaScript service, browser harnesses,
and browser WASM bundle were removed and are not runtime dependencies of the native package.

## IronRDP

- Path: `third_party/IronRDP` (git submodule — gnomecast fork
  https://github.com/truebest/IronRDP, branch `gnome-rdp-support`)
- Upstream: https://github.com/Devolutions/IronRDP
- Base commit and fork delta provenance: see `third_party/IronRDP/PROVENANCE.md`; a mirror
  patch record is kept at `patches/ironrdp/0001-gnome-rdp-support.patch`
- License: MIT OR Apache-2.0; see `third_party/IronRDP/LICENSE-MIT` and
  `third_party/IronRDP/LICENSE-APACHE`
- Native usage: RDP connector/session/EGFX protocol, AVC420 passthrough hooks, and
  RemoteFX Progressive decode to RGBA bitmap updates for the native renderer.

## ss4s

- Path: `third_party/ss4s` (git submodule)
- Upstream: https://github.com/mariotaku/ss4s.git
- Pinned commit: `60d980a22055acd5393079870737dd35683e8ea7`
- License: GNU Lesser General Public License v3.0; see `third_party/ss4s/LICENSE`
- Native usage: decoder/module abstraction for webOS NDL/directmedia and SMP hardware video paths.
- MVP policy: dummy/software ss4s backends are disabled by the native CMake configuration.

## commons-c

- Path: `third_party/commons` (git submodule)
- Upstream: https://github.com/mariotaku/commons-c.git
- Pinned commit: `e09c6f38ea592e33cdc12d1f1dce509087905d73`
- License: MIT; see `third_party/commons/LICENSE`
- Native usage: pinned companion utility library for future ss4s/native integration helpers.

## LVGL

- Path: `third_party/lvgl` (git submodule)
- Upstream: https://github.com/mariotaku/lvgl.git
- Pinned commit: `185ea1fc61dd01fac61867d2d6b56892e80c6058`
- License: MIT; see `third_party/lvgl/LICENCE.txt`
- Native usage: SDL-rendered pre-connect GUI for host/port entry before native RDP startup.

## libevdev

- Source: https://www.freedesktop.org/software/libevdev/libevdev-1.13.6.tar.xz
- Version: 1.13.6
- SHA-256: `73f215eccbd8233f414737ac06bca2687e67c44b97d2d7576091aa9718551110`
- License: MIT, with the bundled Linux input header GPL-2.0 notice recorded in
  `third_party/libevdev-COPYING`
- Native usage: statically linked into webOS builds for grabbed USB mouse/keyboard evdev input.

## miniaudio

- Path: `third_party/miniaudio` (git submodule)
- Upstream: https://github.com/mackron/miniaudio
- Version/tag: `0.11.25`
- Pinned commit: `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d`
- License choice: MIT No Attribution (upstream Alternative 2); see
  `third_party/miniaudio/LICENSE`
- Native usage: headless 48 kHz float node graph, per-session S16 conversion and dynamic
  resampling. Device I/O, file decoding/encoding, the resource manager, generators, and
  the engine, runtime backend loading, and miniaudio threading primitives are disabled
  at compile time; the graph is wired once before rendering and NDL/ss4s is the selected
  production audio sink.

## Moonlight reference boundary

[Moonlight TV](https://github.com/mariotaku/moonlight-tv) (GPL-3.0) was used as a
read-only reference to identify the pinned `ss4s`, `commons-c`, and LVGL dependency
revisions, webOS toolchain conventions, and general GUI behavior on TV. No Moonlight TV
application code has been copied or adapted into this repository, and none may be:
gnomecast's own code is MIT-licensed, which is incompatible with incorporating GPL
application code. Only the dependency pins above (which carry their own licenses) are
shared with it.
