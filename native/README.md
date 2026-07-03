# gnomecast native target

This directory contains the native-only webOS target. It intentionally does not install or invoke a web app, JavaScript service, MSE, WebCodecs, RDCleanPath, or browser fallback path.

Current graphics policy:

- AVC420/H.264 is preferred and is fed to ss4s/NDL/SMP for webOS hardware decode.
- RemoteFX/bitmap updates are supported as a native compatibility path for servers without H.264; Rust/IronRDP decodes to RGBA and the native SDL presenter displays the bitmap surface.

Current status:

- C shell, SDL/webOS lifecycle wiring, Rust FFI ABI, and Rust native worker are in place.
- CMake can build with either the local C FFI stub or the real Rust `staticlib`.
- The H.264 decoder boundary accepts RDPEGFX AVC length-prefixed access units and normalizes them to Annex-B.
- The native RGBA surface helper accepts RemoteFX/bitmap dirty rectangles for SDL presentation.
- Build/package and deploy/launch helpers are under `tools/`.
- TV hardware verification is still pending.

Useful local checks:

```sh
cmake -S native -B /tmp/gnomecast-native-build
cmake --build /tmp/gnomecast-native-build
ctest --test-dir /tmp/gnomecast-native-build --output-on-failure
```

webOS cross-build entrypoint:

```sh
./tools/build-native-webos.sh
ARES_DEVICE=<tv-device> ./tools/deploy-native-webos.sh
```
