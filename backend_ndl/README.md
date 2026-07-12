# backend_ndl

`backend_ndl` is a small C11 wrapper around the process-global webOS NDL
DirectMedia API. It owns runtime symbol loading, the combined audio/video
pipeline state machine, feed serialization, optional overflow protection, and
firmware callbacks. It has no dependency on gnomecast, RDP, SDL, or an H.264
framing parser.

The public header contains only ISO C types. DirectMedia SDK declarations are
private to the implementation, and the final application does not link the NDL
library: `backend_ndl` resolves `libNDL_directmedia.so.1` with `dlopen`.

## Embedding with CMake

```cmake
set(BACKEND_NDL_ENABLE ON)
set(BACKEND_NDL_BUILD_TESTS OFF)
set(BACKEND_NDL_INSTALL OFF)
add_subdirectory(path/to/backend_ndl)

target_link_libraries(my_app PRIVATE backend_ndl::backend_ndl)
```

`BACKEND_NDL_ENABLE=ON` uses `pkg-config NDL_directmedia` for headers only. To
use a custom SDK, set `BACKEND_NDL_NDK_INCLUDE_DIRS`. The target deliberately
does not link `${NDL_directmedia_LIBRARIES}`, avoiding an NDL `DT_NEEDED` entry.

The library is also buildable and installable on its own:

```sh
cmake -S backend_ndl -B /tmp/backend-ndl-build
cmake --build /tmp/backend-ndl-build
ctest --test-dir /tmp/backend-ndl-build --output-on-failure
cmake --install /tmp/backend-ndl-build --prefix /tmp/backend-ndl-prefix
```

The default standalone build produces a portable stub plus a host-tested full
state machine using private fake SDK headers. A product build enables the real
platform implementation.

## API boundary

- The application supplies a non-empty `app_id`; there is no product-specific
  fallback.
- Logging and resource/media events are delivered through callbacks.
- `backend_ndl_set_media()` atomically replaces the complete DirectMedia
  configuration because NDL loads audio and video as one pipeline.
- Input video must already use the elementary-stream framing expected by NDL.
  Framing conversion and keyframe acquisition policy belong to the caller.
- NDL itself is process-global, so only one context may exist at a time.

Everything is MIT licensed, including the host-test ABI double under
`tests/support/libndl-media/`. See [THIRD_PARTY.md](THIRD_PARTY.md).
