# Third-party components

None. All sources in this subproject, including the host-test ABI double under
`tests/support/libndl-media/` (see its README for how its layout fidelity is
verified), are original MIT-licensed code of this repository.

At runtime the library dlopens the proprietary `libNDL_directmedia.so.1`
provided by the webOS firmware; nothing from it is copied or redistributed.
Platform builds compile against the webOS NDK sysroot headers via
`pkg-config NDL_directmedia`; those headers are used in place, not vendored.
