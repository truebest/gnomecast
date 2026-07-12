# DirectMedia ABI test double (host tests only)

`NDL_directmedia.h` here is an **independently written test double** of the
webOS NDL DirectMedia v2 SDK header — original MIT-licensed code of this
repository, not a copy of any SDK or third-party file. It records only the ABI
facts `backend_ndl` and its tests consume: struct layouts, enum values, the
strings the firmware matches, and the sample-rate mapping helper. No function
prototypes are modeled (the library reaches the firmware exclusively through
its injected entry-point table).

It exists solely so the state machine compiles and runs under CTest on hosts
without the webOS NDK. It is never installed or packaged; platform builds
compile against the real NDK sysroot headers resolved via
`pkg-config NDL_directmedia`.

Layout fidelity is verified, not assumed: the same `_Static_assert`
(sizeof/offsetof/enum-value) translation unit compiles cleanly with the ARM
cross compiler against BOTH the NDK sysroot headers and this double. Re-run
that check when extending the file, and never extend it from memory — verify
new facts against the sysroot first, and against the device when behavior is
involved. The SDK's HDR structures are deliberately not modeled: backend_ndl
does not use the HDR API and the real ABI there is known to vary between
header sets.
