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
- Pin selected to match Moonlight TV: https://github.com/mariotaku/moonlight-tv (`third_party/ss4s`)
- License: GNU Lesser General Public License v3.0; see `third_party/ss4s/LICENSE`
- Native usage: decoder/module abstraction for webOS NDL/directmedia and SMP hardware video paths.
- MVP policy: dummy/software ss4s backends are disabled by the native CMake configuration.

## commons-c

- Path: `third_party/commons` (git submodule)
- Upstream: https://github.com/mariotaku/commons-c.git
- Pinned commit: `e09c6f38ea592e33cdc12d1f1dce509087905d73`
- Pin selected to match Moonlight TV: https://github.com/mariotaku/moonlight-tv (`third_party/commons`)
- License: MIT; see `third_party/commons/LICENSE`
- Native usage: pinned companion utility library for future ss4s/native integration helpers.

## LVGL

- Path: `third_party/lvgl` (git submodule)
- Upstream: https://github.com/mariotaku/lvgl.git
- Pinned commit: `185ea1fc61dd01fac61867d2d6b56892e80c6058`
- Pin selected to match Moonlight TV: https://github.com/mariotaku/moonlight-tv (`third_party/lvgl`)
- License: MIT; see `third_party/lvgl/LICENCE.txt`
- Native usage: SDL-rendered pre-connect GUI for host/port entry before native RDP startup.

## Moonlight reference boundary

[Moonlight TV](https://github.com/mariotaku/moonlight-tv) (GPL-3.0) was used as a
read-only reference to identify the pinned `ss4s`, `commons-c`, and LVGL dependency
revisions, webOS toolchain conventions, and general GUI behavior on TV. No Moonlight TV
application code has been copied or adapted into this repository, and none may be:
gnomecast's own code is MIT-licensed, which is incompatible with incorporating GPL
application code. Only the dependency pins above (which carry their own licenses) are
shared with it.
