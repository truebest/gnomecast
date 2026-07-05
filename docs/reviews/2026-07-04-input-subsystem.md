# Input subsystem review — mouse, keyboard, pointers (2026-07-04)

Multi-agent adversarially-verified review (xhigh: 5 correctness finders + 1 cleanup
finder + gap sweep; every candidate independently verified; 46 agents total).
Scope: `input_sdl.c/.h`, `cursor_sdl.c/.h`, input/cursor regions of `main.c`,
input-related Rust in `webrdp-min/src/native.rs`, `ui_preconnect.c` plumbing, tests.
Reviewed tree: main @ 8cd8e8a (post cursor fixes, PR #8).

51 verified findings merged by root cause into 15 distinct defects (report cap).
Omitted below the cap: unmapped Pause/ISO keys, horizontal wheel, minor data-race
and cleanup items.

Known limitations excluded from scope by design: keypad-5/NumLock swallowed by webOS;
no server cursor shapes in relative mode; EGFX surface ops intentionally ignored;
IME recenter flash (cosmetic).

Suggested fix waves: **1** = findings 1–4 (crash + input corruption);
**2** = findings 7–11 (cursor-state cluster, interlinked);
**3** = findings 5–6, 12–15 (coordinates and minor).

## Critical

### 1. Out-of-bounds read in the JSON `\u` escape parser

- **Where:** `native/src/main.c:333`
- **Verdict:** CONFIRMED · **Fix wave:** 1

The \u escape parser in json_read_string evaluates hex_value(p[0])..hex_value(p[3]) unconditionally, reading up to 3 bytes past the string's NUL terminator when the JSON text ends at or just after "\u" — an out-of-bounds read (UB) on argv-backed webOS launch-parameter strings, which get no slack bytes (config files get calloc(size+5) slack, argv does not).

**Failure scenario.** The app is launched with a webOS launch argument (or nested params/launchParams JSON) whose text ends in a truncated escape such as {"password":"\u — hex_value(p[1..3]) reads past the argument's terminating NUL; if that argv string sits at the end of a mapped page, the client segfaults during config parsing at startup instead of reporting an invalid-config error.

## Input integrity

### 2. Non-ASCII keyboard input is delivered twice (scancode + unicode)

- **Where:** `native/src/main.c:2125`
- **Verdict:** CONFIRMED · **Fix wave:** 1

Keyboard echo suppression only covers ASCII text (sdl_text_is_ascii_printable), but the mapped scancode is sent for every key regardless, so keys that produce non-ASCII text are delivered twice (scancode + unicode).

**Failure scenario.** User types on an attached keyboard with a non-US layout (French, German, Cyrillic...): pressing the 'é' key sends the RDP scancode for that physical key (server types '2' or its own layout's char) AND the SDL_TEXTINPUT 'é' is non-ASCII so it is never suppressed and is also sent via native_input_text_utf8 — every accented/non-Latin character appears twice (e.g. typing 'café' produces 'caf2é' on a US-layout server), garbling all typed text.

### 3. Input commands are silently dropped when the queue is full

- **Where:** `webrdp-min/src/native.rs:1685`
- **Verdict:** CONFIRMED · **Fix wave:** 1

enqueue silently discards input commands on TrySendError::Full, including non-idempotent button-up/key-up events, leaving the server with a stuck button or auto-repeating key. [same root cause also at: webrdp-min/src/native.rs:1685, webrdp-min/src/native.rs:1685, webrdp-min/src/native.rs:1685]

**Failure scenario.** The rdp-worker thread stalls for a while (blocked in write_all on a congested/zero-window TLS stream, or busy in drive_reactivation after a RESET_GRAPHICS) while the user keeps moving the Magic Remote pointer; the 1024-deep sync_channel fills with PointerMove commands. The user then releases a dragged mouse button (or a held key): the ButtonUp/KeyUp is dropped by the Full arm, so when the worker recovers the server still believes the button is held — the desktop is left in a phantom drag (or a key auto-repeats) until the user clicks/presses that button again.

### 4. Input commands are silently dropped during Deactivation-Reactivation

- **Where:** `webrdp-min/src/native.rs:1610`
- **Verdict:** CONFIRMED · **Fix wave:** 1

poll_stop() silently discards queued InputCommands during an in-session Deactivation-Reactivation: drive_reactivation -> read_activation_pdu calls poll_stop in a loop, whose Input arm drops commands under a comment written for the pre-active connection phase, even though ActiveStage exists and the commands could simply stay queued (1024-deep) until drain_input resumes.

**Failure scenario.** User is holding a mouse button (drag) or a key when the server sends a Deactivate-All (e.g. gnome-remote-desktop resolution change); the SDL thread enqueues the button-up/key-up while the worker is inside drive_reactivation, poll_stop eats it, and after reactivation the server is left with a permanently stuck button/key until the user presses it again.

## Coordinate spaces

### 5. Renderer output size (pixels) fed into a window-points pointer mapping

- **Where:** `native/src/main.c:1552`
- **Verdict:** CONFIRMED · **Fix wave:** 3

native_sync_input_window_size feeds the renderer OUTPUT size (drawable pixels) into the pointer mapping, but SDL mouse events arrive in window points, and the window is created with SDL_WINDOW_ALLOW_HIGHDPI — mixed coordinate spaces whenever drawable != window size. [same root cause also at: native/src/main.c:1552]

**Failure scenario.** On any display where ALLOW_HIGHDPI makes SDL_GetRendererOutputSize differ from SDL_GetWindowSize (2x-scaled desktop used for development, or a webOS build where the drawable is panel-res while the window is 1080p logical): window_width becomes e.g. 3840 while event->motion.x maxes at 1919, so native_input_map_point maps every pointer event to at most half the desktop — clicks land in the top-left quadrant of the remote desktop and the bottom/right half of the screen is unreachable.

### 6. Jump filter's first-motion delta computed against a stale center position

- **Where:** `native/src/main.c:1811`
- **Verdict:** CONFIRMED · **Fix wave:** 3

virtual_mouse_x/y is seeded to the window center once at app startup and never re-seeded from the real pointer position when a session becomes active, so the jump filter's first-motion delta is computed against a stale center position. [same root cause also at: native/src/main.c:1807, native/src/main.c:1811]

**Failure scenario.** Default config (absolute mouse, jumpFilter on): user parks the pointer near a screen corner in the pre-connect UI (preconnect motion events go to LVGL, never updating virtual_mouse) and connects via the Enter key. On the first mouse move in the session, dx = event.x - 960 can exceed NATIVE_INPUT_JUMP_ALWAYS_PX (corner is ~950+ px from center), so the motion is dropped as a 'jump' and SDL_WarpMouseInWindow teleports the visible pointer from the corner to the screen center — the user's pointer visibly jumps and the intended movement is lost.

## Cursor state

### 7. Failed SDL_CreateColorCursor leaves the pointer invisible (broken degrade path)

- **Where:** `native/src/cursor_sdl.c:283`
- **Verdict:** CONFIRMED · **Fix wave:** 2

When SDL_CreateColorCursor fails (the documented 'color cursors unproven on the webOS SDL port' degraded path), native_cursor_apply_shape neither restores visibility nor sets any fallback cursor, so a hidden pointer stays hidden after the server re-shows it via a new shape. [same root cause also at: native/src/cursor_sdl.c:283, native/src/cursor_sdl.c:283]

**Failure scenario.** On an SDL port where SDL_CreateColorCursor returns NULL: server hides its pointer (client hides the image via SDL_webOSCursorVisibility(FALSE)), then re-shows it by sending a pointer bitmap (how gnome-remote-desktop/IronRDP re-show — Cached emits Hidden+Bitmap, never Default). The SHAPE branch calls native_cursor_apply_shape, cursor creation fails, native_cursor_set_visible(cursor, true) is never reached, and cursor->visible stays false — the user permanently loses the local pointer image (server thinks it is visible) until an unlikely SetDefault arrives, despite the comment promising 'degrade to the default arrow'.

### 8. The first server-driven cursor hide of a session is skipped

- **Where:** `native/src/cursor_sdl.c:326`
- **Verdict:** CONFIRMED · **Fix wave:** 2

native_cursor_init leaves cursor->visible=false while the real platform cursor is visible, so the HIDDEN branch's `if (cursor->visible)` guard skips the very first server-driven hide of a session. [same root cause also at: native/src/cursor_sdl.c:32, native/src/cursor_sdl.c:326, native/src/cursor_sdl.c:326, native/src/cursor_sdl.c:326]

**Failure scenario.** First connection of a launch: the remote side has its pointer hidden (e.g. GNOME hides the cursor over fullscreen video or after idle) and the first pointer update the server sends is SetHidden. native_cursor_apply sees desired=HIDDEN but cursor->visible==false (memset default from native_cursor_init), skips SDL_webOSCursorVisibility(FALSE), and the local arrow stays painted on top of the remote content until some later Default/Shape update arrives — the server believes the pointer is hidden while the TV keeps showing a stale arrow.

### 9. Silent reconnect leaks hidden/shaped cursor state into the new session

- **Where:** `webrdp-min/src/native.rs:948`
- **Verdict:** CONFIRMED · **Fix wave:** 2

The silent in-worker reconnect added for grd's disconnect-provider-ultimatum handoff calls reset_session_state (inbuf/gfx/pts only) but never resets or re-emits pointer state, bypassing the previously universal session-teardown path in main.c where native_cursor_reset restores the default visible cursor, so a hidden/shaped cursor from the old session leaks into the new one.

**Failure scenario.** The server hides the pointer (user watching a fullscreen video), then gnome-remote-desktop closes the session for a daemon handover; the worker reconnects silently (no session_failed, so main.c never calls native_cursor_reset) and the new session sends no pointer update until the server-side cursor next changes; meanwhile the local cursor stays hidden (sticky on the non-webOS SDL_ShowCursor path) — after the reconnect the user moves the mouse and sees no pointer at all until the server happens to emit a new pointer bitmap.

### 10. Hide-while-typing bypasses the cursor module's visible flag

- **Where:** `native/src/main.c:1751`
- **Verdict:** CONFIRMED · **Fix wave:** 2

native_hide_webos_cursor_for_key hides the pointer via SDL_webOSCursorVisibility(FALSE) without updating app->cursor.visible, so the next server pointer update (shape or DEFAULT) makes native_cursor_apply/native_cursor_set_visible(cursor, true) (cursor_sdl.c lines 271/344) re-show the pointer image mid-typing.

**Failure scenario.** User types on an attached keyboard (pointer image hidden per keypress as designed); the remote server changes its cursor shape without any local mouse motion — the text caret/I-beam change caused by the typing itself, a busy-spinner ending, a window appearing under the pointer — and the pointer bitmap update arrives on the next cursor tick, which unconditionally re-shows the webOS pointer: the arrow pops back over the text being typed after nearly every keystroke burst, defeating hide-while-typing (the stated purpose of commits 3707e5b/d166e11).

### 11. Server 'invisible' pointer shapes (zero-dimension) are rejected

- **Where:** `native/src/cursor_sdl.c:56`
- **Verdict:** CONFIRMED · **Fix wave:** 2

A server 'invisible' pointer shape (zero-dimension color/new/large pointer, which IronRDP decodes as DecodedPointer::new_invisible with width=0/height=0 and native.rs forwards verbatim as a PointerBitmap) is rejected by native_cursor_submit_bitmap's width==0/height==0 validation instead of being treated as a hide, so the previous cursor shape stays visible.

**Failure scenario.** Server hides the cursor by pushing a 0x0 pointer bitmap (the documented alternative to the null system pointer, explicitly modeled by IronRDP's new_invisible); the C layer silently drops the update, generation never bumps for it, and the stale server cursor shape remains displayed while the remote side believes the pointer is hidden.

## Minor

### 12. Per-session SDL-side input state survives across sessions

- **Where:** `native/src/main.c:1403`
- **Verdict:** CONFIRMED · **Fix wave:** 3

native_start_rdp/native_stop_rdp never reset per-session SDL-side input state: pointer_warp_pending (with pointer_warp_x/y), pointer_clamp_pending, expect_warp_deadline, and wheel_accumulator all survive across sessions, so leftovers from a dead session are drained into the next one on its first ACTIVE tick.

**Failure scenario.** Session A's server sends on_pointer_position right before the connection dies (drain never runs because state leaves ACTIVE); user connects to session B; the first native_drain_pointer_warp warps the local pointer to session A's stale coordinates scaled by session B's dimensions and echoes that position to the new server as a real pointer move.

### 13. Keypad-Enter arms ASCII text suppression with nothing to consume it

- **Where:** `native/src/main.c:1865`
- **Verdict:** CONFIRMED · **Fix wave:** 3

sdl_key_is_plain_printable's keypad range (SDLK_KP_DIVIDE..SDLK_KP_PERIOD) includes SDLK_KP_ENTER, which produces no SDL_TEXTINPUT, so keypad-Enter arms suppress_next_ascii_text with nothing to consume it. [same root cause also at: native/src/main.c:1865]

**Failure scenario.** User presses keypad Enter (scancode sent, suppression armed with a 250 ms deadline) and an ASCII SDL_TEXTINPUT that is not preceded by its own printable keydown arrives within 250 ms — e.g. the webOS on-screen keyboard/IME delivering committed text as text-only events — the handler at line 2125 suppresses it and the character is silently never sent to the server: the first character typed via the screen keyboard right after keypad-Enter is dropped.

### 14. The desktop's last column/row is unreachable (floor mapping)

- **Where:** `native/src/input_sdl.c:82`
- **Verdict:** CONFIRMED · **Fix wave:** 3

native_input_map_point uses floor((wx*dw)/ww) after clamping wx to ww-1, so when the desktop is larger than the window the last desktop column/row (dw-1, dh-1) is unreachable. [same root cause also at: native/src/input_sdl.c:82, native/src/input_sdl.c:82, native/src/input_sdl.c:82]

**Failure scenario.** 4K server desktop (3840x2160, the real EGFX size on a 4K panel per on_desktop_size) with the fixed 1920x1080 SDL window: the maximum mappable coordinate is (1919*3840)/1920 = 3838, so the rightmost pixel column 3839 and bottom row 2159 can never receive pointer events — 1-pixel edge targets on the remote desktop (edge-anchored scrollbars, hot edges, Fitts-law screen-edge clicks) cannot be hit no matter where the user moves the pointer.

### 15. Extended mouse buttons (X1/X2) are never forwarded

- **Where:** `native/src/main.c:1838`
- **Verdict:** CONFIRMED · **Fix wave:** 3

native_button_from_sdl returns 0 for SDL_BUTTON_X1/SDL_BUTTON_X2, and handle_sdl_event drops button==0, so the adapter never forwards the extended mouse buttons even though RDP supports them (PTR_XFLAGS_BUTTON1/2).

**Failure scenario.** A user with a standard 5-button USB mouse attached to the TV presses the back/forward side buttons while browsing on the remote desktop; the events are silently discarded client-side and nothing happens remotely — browser back/forward navigation via mouse is dead with no log or feedback.
