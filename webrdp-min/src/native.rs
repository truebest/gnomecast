//! Native C ABI and direct-TCP RDP driver for the webOS native app.
//!
//! The native target has no Web/RDCleanPath/browser fallback. It connects directly to
//! the RDP server over TCP, upgrades that socket to TLS, performs CredSSP, prefers
//! AVC420/H.264 EGFX for ss4s hardware decode, and also forwards native RemoteFX/bitmap
//! RGBA updates for servers that cannot provide H.264.

use std::collections::HashMap;
use std::ffi::{c_char, CStr, CString};
use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender, TryRecvError, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use ironrdp_connector::connection_activation::ConnectionActivationState;
use ironrdp_connector::{
    ClientConnector, ClientConnectorState, Config, ConnectionResult, Credentials, DesktopSize,
    Sequence as _,
};
use ironrdp_core::{
    impl_as_any, Decode as _, Encode, EncodeResult, ReadCursor, WriteBuf, WriteCursor,
};
use ironrdp_displaycontrol::client::DisplayControlClient;
use ironrdp_displaycontrol::pdu::{
    DisplayControlMonitorLayout, DisplayControlPdu, MonitorLayoutEntry,
};
use ironrdp_dvc::{DrdynvcClient, DvcEncode, DvcMessage, DvcProcessor};
use ironrdp_egfx::client::{BitmapUpdate, GraphicsPipelineClient, GraphicsPipelineHandler};
use ironrdp_egfx::pdu::{
    CacheToSurfacePdu, CapabilitiesV81Flags, CapabilitiesV8Flags, CapabilitySet, Codec1Type,
    GfxPdu, MapSurfaceToOutputPdu, SolidFillPdu, SurfaceToSurfacePdu, WireToSurface2Pdu,
};
use ironrdp_graphics::image_processing::PixelFormat;
use ironrdp_graphics::pointer::DecodedPointer;
use ironrdp_pdu::gcc::KeyboardType;
use ironrdp_pdu::geometry::InclusiveRectangle;
use ironrdp_pdu::geometry::Rectangle as _;
use ironrdp_pdu::input::fast_path::{FastPathInputEvent, KeyboardFlags, SynchronizeFlags};
use ironrdp_pdu::input::mouse::{MousePdu, PointerFlags};
use ironrdp_pdu::rdp::capability_sets::MajorPlatformType;
use ironrdp_pdu::rdp::client_info::{PerformanceFlags, TimezoneInfo};
use ironrdp_pdu::rdp::headers::ShareDataPdu;
use ironrdp_pdu::rdp::refresh_rectangle::RefreshRectanglePdu;
use ironrdp_pdu::rdp::suppress_output::SuppressOutputPdu;
use ironrdp_pdu::PduResult;
use ironrdp_rdpsnd::pdu as sndpdu;
use ironrdp_session::image::DecodedImage;
use ironrdp_session::{fast_path, ActiveStage, ActiveStageOutput};
use x509_cert::der::Decode as _;

use crate::credssp::CredsspClient;

const INPUT_QUEUE_DEPTH: usize = 1024;
const IO_POLL_TIMEOUT: Duration = Duration::from_millis(50);
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);

/// Values shared with the `RdpAudioCodec` enum in native/include/rdp_ffi.h.
pub const RDP_AUDIO_CODEC_OPUS: u32 = 1;
pub const RDP_AUDIO_CODEC_PCM_S16LE: u32 = 2;

/// MS-RDPEA audio output over a dynamic virtual channel. The name is fixed by the spec
/// and matched case-sensitively against the DVC create request; gnome-remote-desktop
/// uses this transport instead of the static "rdpsnd" channel.
const AUDIO_DVC_CHANNEL_NAME: &str = "AUDIO_PLAYBACK_DVC";

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RdpState {
    Idle = 0,
    Connecting = 1,
    Tls = 2,
    Credssp = 3,
    Active = 4,
    NoAvc420 = 5,
    DecoderError = 6,
    NetworkError = 7,
    ProtocolError = 8,
    Stopped = 9,
}

#[repr(C)]
pub struct RdpConfig {
    pub host: *const c_char,
    pub port: u16,
    pub username: *const c_char,
    pub password: *const c_char,
    pub domain: *const c_char,
    pub width: u16,
    pub height: u16,
    pub fps: u16,
    /// Non-zero: advertise only PCM to rdpsnd for a lossless stream (grd would otherwise
    /// pick Opus when both are offered). Fits in the struct's tail padding on both ABIs.
    pub prefer_pcm_audio: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdpCallbacks {
    pub ctx: *mut core::ffi::c_void,
    pub on_state: Option<extern "C" fn(*mut core::ffi::c_void, RdpState, *const c_char)>,
    pub on_log: Option<extern "C" fn(*mut core::ffi::c_void, *const c_char)>,
    pub on_desktop_size: Option<extern "C" fn(*mut core::ffi::c_void, u16, u16)>,
    pub on_video_au: Option<extern "C" fn(*mut core::ffi::c_void, *const u8, usize, bool, u64)>,
    pub on_bitmap_update: Option<
        extern "C" fn(*mut core::ffi::c_void, u16, u32, u32, u32, u32, u32, *const u8, usize),
    >,
    /// (ctx, codec: RDP_AUDIO_CODEC_*, sample_rate, channels). Fired before the first
    /// on_audio_data of a stream and again whenever the negotiated format changes.
    pub on_audio_format: Option<extern "C" fn(*mut core::ffi::c_void, u32, u32, u16)>,
    /// (ctx, data, len, audio_timestamp_ms). One encoded packet (Opus) or PCM chunk per
    /// call; bytes are valid only for the duration of the call.
    pub on_audio_data: Option<extern "C" fn(*mut core::ffi::c_void, *const u8, usize, u32)>,
    /// (ctx, width, height, hotspot_x, hotspot_y, rgba, len). Decoded server cursor shape:
    /// RGBA byte order, top-down rows, tight stride (width * 4), straight (non-premultiplied)
    /// alpha. Bytes are valid only for the duration of the call.
    pub on_pointer_bitmap:
        Option<extern "C" fn(*mut core::ffi::c_void, u16, u16, u16, u16, *const u8, usize)>,
    /// (ctx, x, y). Server-initiated pointer warp, in desktop coordinates.
    pub on_pointer_position: Option<extern "C" fn(*mut core::ffi::c_void, u16, u16)>,
    /// (ctx, state: RDP_POINTER_STATE_*). 0 = hidden, 1 = system default arrow.
    pub on_pointer_state: Option<extern "C" fn(*mut core::ffi::c_void, u32)>,
}

pub const RDP_POINTER_STATE_HIDDEN: u32 = 0;
pub const RDP_POINTER_STATE_DEFAULT: u32 = 1;

pub struct RdpSession {
    stop: Arc<AtomicBool>,
    tx: SyncSender<WorkerCommand>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

#[derive(Clone)]
struct NativeConfig {
    host: String,
    port: u16,
    username: String,
    password: String,
    domain: String,
    width: u16,
    height: u16,
    fps: u16,
    prefer_pcm_audio: bool,
}

#[derive(Clone, Copy)]
struct CallbackSink {
    callbacks: RdpCallbacks,
}

// The C shell owns `ctx` and guarantees that it outlives the session. Callbacks are invoked
// synchronously from the worker thread, and byte/string pointers are valid only for the call.
unsafe impl Send for CallbackSink {}
unsafe impl Sync for CallbackSink {}

impl CallbackSink {
    fn empty() -> Self {
        Self {
            callbacks: RdpCallbacks {
                ctx: ptr::null_mut(),
                on_state: None,
                on_log: None,
                on_desktop_size: None,
                on_video_au: None,
                on_bitmap_update: None,
                on_audio_format: None,
                on_audio_data: None,
                on_pointer_bitmap: None,
                on_pointer_position: None,
                on_pointer_state: None,
            },
        }
    }

    fn new(callbacks: RdpCallbacks) -> Self {
        Self { callbacks }
    }

    fn emit_state(&self, state: RdpState, detail: impl AsRef<str>) {
        if let Some(cb) = self.callbacks.on_state {
            let detail = cstring_lossy(detail.as_ref());
            cb(self.callbacks.ctx, state, detail.as_ptr());
        }
    }

    fn log(&self, line: impl AsRef<str>) {
        if let Some(cb) = self.callbacks.on_log {
            let line = cstring_lossy(line.as_ref());
            cb(self.callbacks.ctx, line.as_ptr());
        }
    }

    fn desktop_size(&self, width: u16, height: u16) {
        if let Some(cb) = self.callbacks.on_desktop_size {
            cb(self.callbacks.ctx, width, height);
        }
    }

    fn video_au(&self, data: &[u8], is_keyframe: bool, pts90k: u64) {
        if let Some(cb) = self.callbacks.on_video_au {
            cb(
                self.callbacks.ctx,
                data.as_ptr(),
                data.len(),
                is_keyframe,
                pts90k,
            );
        }
    }
    fn bitmap_update(&self, update: &NativeBitmapUnit) {
        if let Some(cb) = self.callbacks.on_bitmap_update {
            cb(
                self.callbacks.ctx,
                update.surface_id,
                update.left,
                update.top,
                update.width,
                update.height,
                update.stride,
                update.data.as_ptr(),
                update.data.len(),
            );
        }
    }

    fn audio_format(&self, codec: u32, sample_rate: u32, channels: u16) {
        if let Some(cb) = self.callbacks.on_audio_format {
            cb(self.callbacks.ctx, codec, sample_rate, channels);
        }
    }

    fn pointer_bitmap(&self, pointer: &DecodedPointer) {
        if let Some(cb) = self.callbacks.on_pointer_bitmap {
            cb(
                self.callbacks.ctx,
                pointer.width,
                pointer.height,
                pointer.hotspot_x,
                pointer.hotspot_y,
                pointer.bitmap_data.as_ptr(),
                pointer.bitmap_data.len(),
            );
        }
    }

    fn pointer_position(&self, x: u16, y: u16) {
        if let Some(cb) = self.callbacks.on_pointer_position {
            cb(self.callbacks.ctx, x, y);
        }
    }

    fn pointer_state(&self, state: u32) {
        if let Some(cb) = self.callbacks.on_pointer_state {
            cb(self.callbacks.ctx, state);
        }
    }

    fn audio_data(&self, data: &[u8], ts_ms: u32) {
        if let Some(cb) = self.callbacks.on_audio_data {
            cb(self.callbacks.ctx, data.as_ptr(), data.len(), ts_ms);
        }
    }
}

#[derive(Debug)]
struct NativeError {
    state: RdpState,
    message: String,
}

impl NativeError {
    fn network(message: impl Into<String>) -> Self {
        Self {
            state: RdpState::NetworkError,
            message: message.into(),
        }
    }

    fn protocol(message: impl Into<String>) -> Self {
        Self {
            state: RdpState::ProtocolError,
            message: message.into(),
        }
    }

    fn no_avc420(message: impl Into<String>) -> Self {
        Self {
            state: RdpState::NoAvc420,
            message: message.into(),
        }
    }
}

#[derive(Debug)]
enum WorkerCommand {
    Stop,
    Input(InputCommand),
    Control(ControlCommand),
}

/// Session-level (non-input) client requests, dispatched on the x224/DVC channels rather
/// than the fast-path input channel. Added for multi-session video switching.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ControlCommand {
    /// TS_SUPPRESS_OUTPUT_PDU: `allow_display=false` asks the server to stop sending
    /// graphics (audio DVCs keep flowing), `true` resumes them.
    SuppressOutput { allow_display: bool },
    /// Ask the server for a fresh full frame / keyframe so a hardware decoder that lost
    /// its state (the shared webOS pipeline reloads on every track close) can resync.
    RequestRefresh,
}

#[derive(Debug)]
enum InputCommand {
    PointerMove {
        x: u16,
        y: u16,
    },
    PointerButton {
        x: u16,
        y: u16,
        button: u8,
        down: bool,
    },
    PointerWheel {
        x: u16,
        y: u16,
        delta: i16,
    },
    Key {
        scancode: u8,
        down: bool,
        extended: bool,
    },
    Unicode {
        codepoint: u16,
        down: bool,
    },
    /// Absolute toggle-key state (TS_FP_SYNC_EVENT); bits follow `SynchronizeFlags`.
    SyncLocks {
        flags: u8,
    },
}

impl InputCommand {
    fn into_events(self) -> Vec<FastPathInputEvent> {
        match self {
            InputCommand::PointerMove { x, y } => vec![FastPathInputEvent::MouseEvent(MousePdu {
                flags: PointerFlags::MOVE,
                number_of_wheel_rotation_units: 0,
                x_position: x,
                y_position: y,
            })],
            InputCommand::PointerButton { x, y, button, down } => {
                let mut flags = match button {
                    1 => PointerFlags::LEFT_BUTTON,
                    2 => PointerFlags::RIGHT_BUTTON,
                    3 => PointerFlags::MIDDLE_BUTTON_OR_WHEEL,
                    _ => PointerFlags::LEFT_BUTTON,
                };
                if down {
                    flags |= PointerFlags::DOWN;
                }
                vec![FastPathInputEvent::MouseEvent(MousePdu {
                    flags,
                    number_of_wheel_rotation_units: 0,
                    x_position: x,
                    y_position: y,
                })]
            }
            InputCommand::PointerWheel { x, y, delta } => {
                vec![FastPathInputEvent::MouseEvent(MousePdu {
                    flags: PointerFlags::VERTICAL_WHEEL,
                    number_of_wheel_rotation_units: delta,
                    x_position: x,
                    y_position: y,
                })]
            }
            InputCommand::Key {
                scancode,
                down,
                extended,
            } => {
                let mut flags = KeyboardFlags::empty();
                if !down {
                    flags |= KeyboardFlags::RELEASE;
                }
                if extended {
                    flags |= KeyboardFlags::EXTENDED;
                }
                vec![FastPathInputEvent::KeyboardEvent(flags, scancode)]
            }
            InputCommand::Unicode { codepoint, down } => {
                let mut flags = KeyboardFlags::empty();
                if !down {
                    flags |= KeyboardFlags::RELEASE;
                }
                vec![FastPathInputEvent::UnicodeKeyboardEvent(flags, codepoint)]
            }
            InputCommand::SyncLocks { flags } => vec![FastPathInputEvent::SyncEvent(
                SynchronizeFlags::from_bits_truncate(flags),
            )],
        }
    }
}

#[derive(Default)]
struct NativeGfxState {
    pending_video: Vec<NativeVideoUnit>,
    pending_bitmap: Vec<NativeBitmapUnit>,
    unsupported_graphics: Option<String>,
    // Real graphics output size from the server's RDPGFX_RESET_GRAPHICS_PDU, which can
    // differ from the negotiated MCS/GCC desktop size (e.g. a TV whose hardware decoder
    // always runs at the panel's native resolution regardless of the requested session size).
    // Applies uniformly to both the AVC420/H.264 and RemoteFX bitmap paths, so it is
    // dispatched through the same on_desktop_size callback both codecs already rely on
    // instead of being attached only to AVC420 access units.
    graphics_width: u32,
    graphics_height: u32,
    graphics_size_pending: bool,
    // Per-surface output origin from RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU. BitmapUpdate rectangles
    // are surface-local, so this must be added before a bitmap update is queued, or updates on
    // a surface mapped away from (0,0) land at the wrong desktop position.
    surface_origins: HashMap<u16, (u32, u32)>,
}

struct NativeVideoUnit {
    data: Vec<u8>,
    is_keyframe: bool,
}

struct NativeBitmapUnit {
    surface_id: u16,
    left: u32,
    top: u32,
    width: u32,
    height: u32,
    stride: u32,
    data: Vec<u8>,
}

struct NativeGfxHandler {
    shared: Arc<Mutex<NativeGfxState>>,
}

impl NativeGfxHandler {
    fn mark_unsupported_graphics(&self, detail: impl Into<String>) {
        if let Ok(mut shared) = self.shared.lock() {
            if shared.unsupported_graphics.is_none() {
                shared.unsupported_graphics = Some(detail.into());
            }
        }
    }
}

impl GraphicsPipelineHandler for NativeGfxHandler {
    fn capabilities(&self) -> Vec<CapabilitySet> {
        vec![
            CapabilitySet::V8_1 {
                flags: CapabilitiesV81Flags::AVC420_ENABLED | CapabilitiesV81Flags::SMALL_CACHE,
            },
            CapabilitySet::V8 {
                flags: CapabilitiesV8Flags::SMALL_CACHE,
            },
        ]
    }

    fn on_capabilities_confirmed(&mut self, _caps: &CapabilitySet) {}

    fn on_reset_graphics(&mut self, width: u32, height: u32) {
        if let Ok(mut shared) = self.shared.lock() {
            shared.pending_video.clear();
            shared.pending_bitmap.clear();
            // ResetGraphics implicitly destroys all surfaces, so any tracked mapping origin is
            // stale afterward.
            shared.surface_origins.clear();
            if width != shared.graphics_width || height != shared.graphics_height {
                shared.graphics_width = width;
                shared.graphics_height = height;
                shared.graphics_size_pending = true;
            }
        }
    }

    fn on_map_surface_to_output(&mut self, pdu: &MapSurfaceToOutputPdu) {
        if let Ok(mut shared) = self.shared.lock() {
            shared
                .surface_origins
                .insert(pdu.surface_id, (pdu.output_origin_x, pdu.output_origin_y));
        }
    }

    fn on_surface_deleted(&mut self, surface_id: u16) {
        // DeleteSurface destroys the surface just like ResetGraphics does; a later surface
        // reusing this id must not inherit the stale mapping origin before its own
        // MapSurfaceToOutput arrives.
        if let Ok(mut shared) = self.shared.lock() {
            shared.surface_origins.remove(&surface_id);
        }
    }

    fn on_bitmap_updated(&mut self, update: &BitmapUpdate) {
        if update.data.is_empty() || update.width == 0 || update.height == 0 {
            self.mark_unsupported_graphics(format!(
                "server produced empty bitmap update ({:?})",
                update.codec_id
            ));
            return;
        }
        let stride = u32::from(update.width) * 4;
        // u64: `stride * height` overflows usize on the 32-bit webOS target for
        // dimensions a hostile server can still claim (e.g. 32768x32768).
        let expected = u64::from(stride) * u64::from(update.height);
        if (update.data.len() as u64) < expected {
            self.mark_unsupported_graphics(format!(
                "server produced short bitmap update ({:?}, {} < {})",
                update.codec_id,
                update.data.len(),
                expected
            ));
            return;
        }
        if let Ok(mut shared) = self.shared.lock() {
            // `destination_rectangle` is surface-local; a surface mapped away from (0,0) via
            // RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU needs that origin added before this lands on the
            // desktop-relative native RGBA canvas.
            let (origin_x, origin_y) = shared
                .surface_origins
                .get(&update.surface_id)
                .copied()
                .unwrap_or((0, 0));
            shared.pending_bitmap.push(NativeBitmapUnit {
                surface_id: update.surface_id,
                left: u32::from(update.destination_rectangle.left) + origin_x,
                top: u32::from(update.destination_rectangle.top) + origin_y,
                width: u32::from(update.width),
                height: u32::from(update.height),
                stride,
                data: update.data.clone(),
            });
        }
    }

    fn on_wire_to_surface2(&mut self, _pdu: &WireToSurface2Pdu) {
        // RemoteFX Progressive is decoded by IronRDP into `on_bitmap_updated` RGBA tiles.
    }

    fn on_avc420_frame(
        &mut self,
        _surface_id: u16,
        _left: u16,
        _top: u16,
        _width: u16,
        _height: u16,
        nal: &[u8],
    ) -> bool {
        // `_width`/`_height` above are the frame's destination rectangle on the surface,
        // not the surface's full resolution; the real graphics output size is dispatched
        // separately via on_reset_graphics/on_desktop_size (see drain_gfx).
        if !nal.is_empty() {
            if let Ok(mut shared) = self.shared.lock() {
                shared.pending_video.push(NativeVideoUnit {
                    data: nal.to_vec(),
                    is_keyframe: avc_length_prefixed_has_keyframe(nal),
                });
            }
        }
        true
    }

    fn wants_avc420_passthrough(&self) -> bool {
        true
    }

    // Surface-mutating EGFX operations the client deliberately IGNORES — do not turn these
    // into mark_unsupported_graphics (a review once did; every live session then died with
    // "server used unsupported EGFX SurfaceToSurface" right after Active):
    //
    // - gnome-remote-desktop sends SurfaceToSurface as part of ROUTINE AVC420 sessions.
    // - On the hardware path the TV's video plane shows the complete decoded H.264 stream;
    //   server-side surface composition never reaches the screen, so ignoring these ops is
    //   visually correct there — confirmed by every live session to date.
    // - They matter only for the RemoteFX/RGBA software fallback, where ignoring them CAN
    //   leave stale pixels on servers that use them for that path (grd does not today). If
    //   such a server appears, implement the ops on the RGBA canvas instead of failing.
    //
    // The dispatcher matches these PDUs explicitly, so they never reach on_unhandled_pdu;
    // the trait defaults are silent no-ops. Trace them (WEBRDP_LOG=debug) to stay observable.
    fn on_solid_fill(&mut self, pdu: &SolidFillPdu) {
        tracing::debug!(surface_id = pdu.surface_id, "ignoring EGFX SolidFill");
    }

    fn on_surface_to_surface(&mut self, pdu: &SurfaceToSurfacePdu) {
        tracing::debug!(
            src = pdu.source_surface_id,
            dst = pdu.destination_surface_id,
            "ignoring EGFX SurfaceToSurface"
        );
    }

    fn on_cache_to_surface(&mut self, pdu: &CacheToSurfacePdu) {
        tracing::debug!(
            slot = pdu.cache_slot,
            surface_id = pdu.surface_id,
            "ignoring EGFX CacheToSurface"
        );
    }

    fn on_unhandled_pdu(&mut self, pdu: &GfxPdu) {
        let codec = match pdu {
            GfxPdu::WireToSurface1(pdu) => match pdu.codec_id {
                Codec1Type::Avc444 | Codec1Type::Avc444v2 => "AVC444",
                _ => "unsupported",
            },
            _ => "unsupported",
        };
        self.mark_unsupported_graphics(format!("server produced unsupported {codec} EGFX PDU"));
    }
}

/// Builds the MS-RDPEDISP Display Control client that dictates the server's monitor
/// resolution the moment the channel becomes operational (server capabilities received —
/// this happens before the video stream starts). Headless hosts otherwise come up with
/// virtual-display defaults like 2048x1152 that the TV's hardware video pipeline silently
/// cannot start on. Failures log and send nothing: a DVC processor error is
/// session-fatal, and an unchanged server layout is a working (if suboptimal) session.
fn make_display_control(width: u16, height: u16, sink: CallbackSink) -> DisplayControlClient {
    DisplayControlClient::new(move |caps| {
        let (width, height) =
            MonitorLayoutEntry::adjust_display_size(u32::from(width), u32::from(height));
        if u64::from(width) * u64::from(height) > caps.max_monitor_area() {
            sink.log(format!(
                "display: {width}x{height} exceeds the server's max monitor area; keeping the server layout"
            ));
            return Ok(Vec::new());
        }
        let layout = match DisplayControlMonitorLayout::new_single_primary_monitor(
            width, height, None, None,
        ) {
            Ok(layout) => layout,
            Err(e) => {
                sink.log(format!(
                    "display: failed to build {width}x{height} monitor layout: {e}"
                ));
                return Ok(Vec::new());
            }
        };
        sink.log(format!(
            "display: requesting server resolution {width}x{height}"
        ));
        Ok(vec![Box::new(DisplayControlPdu::from(layout)) as DvcMessage])
    })
}

/// `ClientAudioOutputPdu` implements `SvcEncode` (for the static rdpsnd channel) but not
/// `DvcEncode`, and both the trait and the type are foreign here, so the orphan rule
/// requires this local wrapper to send it over the dynamic channel.
struct RdpsndDvcMessage(sndpdu::ClientAudioOutputPdu);

impl Encode for RdpsndDvcMessage {
    fn encode(&self, dst: &mut WriteCursor<'_>) -> EncodeResult<()> {
        self.0.encode(dst)
    }

    fn name(&self) -> &'static str {
        self.0.name()
    }

    fn size(&self) -> usize {
        self.0.size()
    }
}

impl DvcEncode for RdpsndDvcMessage {}

/// Minimal MS-RDPEA audio output client over the AUDIO_PLAYBACK_DVC dynamic channel — the
/// transport gnome-remote-desktop uses (ironrdp-rdpsnd's own client only implements the
/// static "rdpsnd" channel). Audio is strictly best-effort: every protocol failure logs
/// and swallows instead of returning Err, because DrdynvcClient propagates processor
/// errors into a terminal session error and audio must never take down a working video
/// session. Out-of-order PDUs are handled leniently for the same reason.
struct RdpsndDvcHandler {
    callbacks: CallbackSink,
    /// Latched on an undecodable payload; the channel goes silent instead of erroring.
    stopped: bool,
    /// The filtered format list sent in the Client Audio Formats PDU, in wire order.
    /// Wave2's wFormatNo indexes THIS list (the server echoes the client's index back),
    /// not the server's advertised list.
    client_formats: Vec<sndpdu::AudioFormat>,
    /// (codec, sample_rate, channels) last delivered through on_audio_format.
    last_format: Option<(u32, u32, u16)>,
    /// audioCodec=pcm setting: drop Opus from the client format list so the server has
    /// to send lossless PCM (grd prefers Opus whenever the client offers it).
    prefer_pcm: bool,
    bad_format_logged: bool,
    legacy_wave_logged: bool,
}

impl_as_any!(RdpsndDvcHandler);

impl RdpsndDvcHandler {
    fn new(callbacks: CallbackSink, prefer_pcm: bool) -> Self {
        Self {
            callbacks,
            stopped: false,
            client_formats: Vec::new(),
            last_format: None,
            prefer_pcm,
            bad_format_logged: false,
            legacy_wave_logged: false,
        }
    }

    /// Maps a server-advertised format to the C ABI codec id when the TV can play it:
    /// Opus (NDL hardware decode) and 16-bit stereo PCM only. AAC is deliberately not
    /// accepted — the ndl-webos5 ss4s module cannot play it, and the server prefers AAC
    /// over Opus whenever the client claims AAC support.
    fn rdp_audio_codec_for(format: &sndpdu::AudioFormat) -> Option<u32> {
        match format.format {
            sndpdu::WaveFormat::OPUS
                if format.n_channels == 2 && format.n_samples_per_sec == 48_000 =>
            {
                Some(RDP_AUDIO_CODEC_OPUS)
            }
            sndpdu::WaveFormat::PCM
                if format.n_channels == 2
                    && format.bits_per_sample == 16
                    && matches!(format.n_samples_per_sec, 44_100 | 48_000) =>
            {
                Some(RDP_AUDIO_CODEC_PCM_S16LE)
            }
            _ => None,
        }
    }

    fn reply(pdu: sndpdu::ClientAudioOutputPdu) -> DvcMessage {
        Box::new(RdpsndDvcMessage(pdu))
    }

    fn handle_audio_format(&mut self, pdu: sndpdu::ServerAudioFormatPdu) -> Vec<DvcMessage> {
        let server_count = pdu.formats.len();
        // Echo back the server's own AudioFormat structs for the entries we can play, so
        // auxiliary fields (avg bytes/sec, block align) always match what it advertised.
        let mut formats: Vec<sndpdu::AudioFormat> = pdu
            .formats
            .into_iter()
            .filter(|format| Self::rdp_audio_codec_for(format).is_some())
            .collect();
        // Both Opus and PCM are playable: the C side decodes Opus in-process (libopus)
        // before mixing, so the bandwidth-friendly codec wins whenever the server offers
        // it (gnome-remote-desktop picks by its own preference — AAC > Opus > PCM — among
        // the formats the CLIENT listed, i.e. Opus at ~96kbps instead of ~1.4Mbps PCM).
        // audioCodec=pcm inverts that: list PCM alone so the stream stays lossless. If
        // the server offers no PCM at all, keep the full playable list — degraded audio
        // beats silence.
        if self.prefer_pcm {
            let pcm_only: Vec<sndpdu::AudioFormat> = formats
                .iter()
                .filter(|format| {
                    Self::rdp_audio_codec_for(format) == Some(RDP_AUDIO_CODEC_PCM_S16LE)
                })
                .cloned()
                .collect();
            if pcm_only.is_empty() {
                self.callbacks.log(
                    "audio: PCM requested but the server offers no playable PCM; \
                     keeping the default format list"
                        .to_owned(),
                );
            } else {
                formats = pcm_only;
            }
        }
        if formats.is_empty() {
            self.callbacks.log(format!(
                "audio: server offered {server_count} formats but none are playable; audio stays disabled"
            ));
        } else {
            self.callbacks.log(format!(
                "audio: accepting {} of {} server formats (protocol version {:?})",
                formats.len(),
                server_count,
                pdu.version
            ));
        }
        self.client_formats = formats.clone();
        self.last_format = None;
        self.bad_format_logged = false;

        let mut messages = vec![Self::reply(sndpdu::ClientAudioOutputPdu::AudioFormat(
            sndpdu::ClientAudioFormatPdu {
                version: pdu.version,
                flags: sndpdu::AudioFormatFlags::ALIVE,
                formats,
                volume_left: 0xFFFF,
                volume_right: 0xFFFF,
                pitch: 0x0001_0000,
                dgram_port: 0,
            },
        ))];
        if pdu.version >= sndpdu::Version::V6 {
            messages.push(Self::reply(sndpdu::ClientAudioOutputPdu::QualityMode(
                sndpdu::QualityModePdu {
                    quality_mode: sndpdu::QualityMode::High,
                },
            )));
        }
        messages
    }

    fn handle_wave2(&mut self, pdu: sndpdu::Wave2Pdu<'_>) -> Vec<DvcMessage> {
        if let Some(format) = self.client_formats.get(usize::from(pdu.format_no)) {
            if let Some(codec) = Self::rdp_audio_codec_for(format) {
                let current = (codec, format.n_samples_per_sec, format.n_channels);
                if self.last_format != Some(current) {
                    self.callbacks
                        .audio_format(codec, format.n_samples_per_sec, format.n_channels);
                    self.last_format = Some(current);
                }
                self.callbacks.audio_data(&pdu.data, pdu.audio_timestamp);
            }
        } else if !self.bad_format_logged {
            self.callbacks.log(format!(
                "audio: wave references unknown client format {}; dropping audio data",
                pdu.format_no
            ));
            self.bad_format_logged = true;
        }
        // Confirm even dropped waves, promptly: the server estimates render latency from
        // these confirms and starts discarding audio when it thinks we are >300ms behind.
        vec![Self::reply(sndpdu::ClientAudioOutputPdu::WaveConfirm(
            sndpdu::WaveConfirmPdu {
                timestamp: pdu.timestamp,
                block_no: pdu.block_no,
            },
        ))]
    }
}

impl DvcProcessor for RdpsndDvcHandler {
    fn channel_name(&self) -> &str {
        AUDIO_DVC_CHANNEL_NAME
    }

    fn start(&mut self, _channel_id: u32) -> PduResult<Vec<DvcMessage>> {
        // The server speaks first (Server Audio Formats and Version PDU).
        Ok(Vec::new())
    }

    fn process(&mut self, _channel_id: u32, payload: &[u8]) -> PduResult<Vec<DvcMessage>> {
        if self.stopped {
            return Ok(Vec::new());
        }
        let mut cursor = ReadCursor::new(payload);
        let pdu = match sndpdu::ServerAudioOutputPdu::decode(&mut cursor) {
            Ok(pdu) => pdu,
            Err(e) => {
                self.callbacks.log(format!(
                    "audio: failed to decode audio output PDU: {e}; audio disabled"
                ));
                self.stopped = true;
                return Ok(Vec::new());
            }
        };

        let messages = match pdu {
            sndpdu::ServerAudioOutputPdu::AudioFormat(pdu) => self.handle_audio_format(pdu),
            sndpdu::ServerAudioOutputPdu::Training(pdu) => {
                // The confirm must echo the Training PDU's wPackSize verbatim:
                // gnome-remote-desktop validates it (1024) and ignores mismatched
                // confirms, timing the whole audio protocol out after 10s. IronRDP's
                // decoder consumed the 8 bytes of prolog+header out of wPackSize when
                // sizing `data`, so add them back for a non-empty payload.
                let pack_size = if pdu.data.is_empty() {
                    0
                } else {
                    u16::try_from(pdu.data.len().saturating_add(8)).unwrap_or(0)
                };
                self.callbacks.log(format!(
                    "audio: training received ({} payload bytes)",
                    pdu.data.len()
                ));
                vec![Self::reply(sndpdu::ClientAudioOutputPdu::TrainingConfirm(
                    sndpdu::TrainingConfirmPdu {
                        timestamp: pdu.timestamp,
                        pack_size,
                    },
                ))]
            }
            sndpdu::ServerAudioOutputPdu::Wave2(pdu) => self.handle_wave2(pdu),
            sndpdu::ServerAudioOutputPdu::Volume(pdu) => {
                self.callbacks.log(format!(
                    "audio: ignoring server volume change {:#06x}/{:#06x} (TV remote controls volume)",
                    pdu.volume_left, pdu.volume_right
                ));
                Vec::new()
            }
            sndpdu::ServerAudioOutputPdu::Pitch(pdu) => {
                self.callbacks.log(format!(
                    "audio: ignoring server pitch change {:#010x}",
                    pdu.pitch
                ));
                Vec::new()
            }
            sndpdu::ServerAudioOutputPdu::Close => {
                self.callbacks.log("audio: server closed the audio stream");
                // Re-fire on_audio_format when a new stream starts later.
                self.last_format = None;
                Vec::new()
            }
            sndpdu::ServerAudioOutputPdu::Wave(_)
            | sndpdu::ServerAudioOutputPdu::WaveEncrypt(_)
            | sndpdu::ServerAudioOutputPdu::CryptKey(_) => {
                if !self.legacy_wave_logged {
                    self.callbacks
                        .log("audio: ignoring legacy/encrypted wave PDU (protocol version < 8)");
                    self.legacy_wave_logged = true;
                }
                Vec::new()
            }
        };
        Ok(messages)
    }

    fn close(&mut self, _channel_id: u32) {
        self.callbacks
            .log("audio: AUDIO_PLAYBACK_DVC channel closed");
    }
}

struct NativeWorker {
    config: NativeConfig,
    callbacks: CallbackSink,
    rx: Receiver<WorkerCommand>,
    stop: Arc<AtomicBool>,
    gfx: Arc<Mutex<NativeGfxState>>,
    inbuf: Vec<u8>,
    reactivation:
        Option<Box<ironrdp_connector::connection_activation::ConnectionActivationSequence>>,
    next_pts90k: u64,
    frame_pts_step: u64,
    // Input events drained off the channel by poll_stop while the worker is busy (notably
    // mid-session Deactivate-Reactivate) and not yet dispatched. run_active clears this on
    // entry so pre-connect events are discarded, but events buffered during reactivation
    // survive to the next drain_input rather than being lost (a dropped release would stick).
    pending_input: Vec<InputCommand>,
    // Control commands buffered the same way (suppress-output toggles coalesce to the last
    // one; refresh requests dedupe to one).
    pending_control: Vec<ControlCommand>,
    // Last suppress-output state the client commanded. Every fresh RDP connection starts
    // server-side with display updates ALLOWED, so the silent in-worker reconnect (run's
    // ultimatum retry) must re-assert a commanded suppression or a backgrounded session
    // would silently resume streaming full-rate video nobody displays.
    suppress_display_latched: bool,
}

impl NativeWorker {
    fn new(
        config: NativeConfig,
        callbacks: CallbackSink,
        rx: Receiver<WorkerCommand>,
        stop: Arc<AtomicBool>,
    ) -> Self {
        let fps = u64::from(config.fps.max(1));
        Self {
            config,
            callbacks,
            rx,
            stop,
            gfx: Arc::new(Mutex::new(NativeGfxState::default())),
            inbuf: Vec::new(),
            reactivation: None,
            next_pts90k: 0,
            frame_pts_step: 90_000 / fps,
            pending_input: Vec::new(),
            pending_control: Vec::new(),
            suppress_display_latched: false,
        }
    }

    fn run(&mut self) -> Result<(), NativeError> {
        // gnome-remote-desktop closes the connection with an MCS Disconnect Provider
        // Ultimatum (preceded by ServerSetErrorInfo(RpcInitiatedDisconnect)) as a NORMAL
        // part of e.g. handing a session over between its daemons, and expects the
        // client to reconnect — mstsc/FreeRDP do so automatically. Retry a few times
        // before surfacing the failure.
        const MAX_SESSION_ATTEMPTS: u32 = 3;
        for attempt in 1..=MAX_SESSION_ATTEMPTS {
            match self.run_session() {
                Ok(()) => return Ok(()),
                Err(e)
                    if attempt < MAX_SESSION_ATTEMPTS
                        && !self.stop.load(Ordering::SeqCst)
                        && e.message.contains("disconnect provider ultimatum") =>
                {
                    self.callbacks.log(format!(
                        "server closed the session (attempt {attempt}/{MAX_SESSION_ATTEMPTS}); reconnecting"
                    ));
                    self.reset_session_state();
                    thread::sleep(Duration::from_millis(1000));
                }
                Err(e) => return Err(e),
            }
        }
        unreachable!("loop either returns or retries")
    }

    /// Clears per-session accumulated state so a reconnect starts clean.
    fn reset_session_state(&mut self) {
        self.inbuf.clear();
        self.reactivation = None;
        self.next_pts90k = 0;
        self.pending_input.clear();
        // A suppress/resume queued while the failed session was still handshaking is the
        // newest commanded state and must survive the retry: fold it into the latch
        // (run_active re-asserts a latched suppression on the fresh connection; a fresh
        // connection already starts with display allowed for the resume case). Queued
        // refreshes belong to the OLD encode session — reconnecting yields a new IDR
        // anyway — so those simply drop.
        for control in self.pending_control.drain(..) {
            if let ControlCommand::SuppressOutput { allow_display } = control {
                self.suppress_display_latched = !allow_display;
            }
        }
        if let Ok(mut shared) = self.gfx.lock() {
            *shared = NativeGfxState::default();
        }
        // A silent in-worker reconnect bypasses the C session-teardown path that restores
        // the default cursor, so a pointer the old session left hidden or custom-shaped would
        // leak into the new one until the server next changes it. Reset it to default+visible.
        self.callbacks.pointer_state(RDP_POINTER_STATE_DEFAULT);
    }

    fn run_session(&mut self) -> Result<(), NativeError> {
        self.callbacks.emit_state(
            RdpState::Connecting,
            format!("connecting to {}:{}", self.config.host, self.config.port),
        );

        let mut tcp = match self.connect_tcp()? {
            Some(tcp) => tcp,
            None => return Ok(()),
        };
        let client_addr = tcp
            .local_addr()
            .map_err(|e| NativeError::network(format!("local address: {e}")))?;
        let mut connector = self.new_connector(client_addr);

        self.send_x224_request(&mut tcp, &mut connector)?;
        if self.poll_stop() {
            return Ok(());
        }

        self.callbacks
            .emit_state(RdpState::Tls, "starting TLS security upgrade");
        let (mut tls, public_key) = self.upgrade_tls(tcp)?;
        connector.mark_security_upgrade_as_done();

        if !connector.should_perform_credssp() {
            return Err(NativeError::protocol("server did not select CredSSP/NLA"));
        }
        self.callbacks
            .emit_state(RdpState::Credssp, "performing CredSSP/NLA");
        self.run_credssp(&mut tls, &mut connector, public_key)?;

        let result = self.pump_connector(&mut tls, connector)?;
        self.run_active(tls, result)
    }

    fn connect_tcp(&mut self) -> Result<Option<TcpStream>, NativeError> {
        let addrs: Vec<SocketAddr> = (self.config.host.as_str(), self.config.port)
            .to_socket_addrs()
            .map_err(|e| {
                NativeError::network(format!(
                    "resolve {}:{}: {e}",
                    self.config.host, self.config.port
                ))
            })?
            .collect();
        if addrs.is_empty() {
            return Err(NativeError::network(format!(
                "resolve {}:{} returned no addresses",
                self.config.host, self.config.port
            )));
        }

        let mut last_error = None;
        for addr in addrs {
            if self.poll_stop() {
                return Ok(None);
            }
            match TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT) {
                Ok(stream) => {
                    stream
                        .set_nodelay(true)
                        .map_err(|e| NativeError::network(format!("set TCP_NODELAY: {e}")))?;
                    stream
                        .set_read_timeout(Some(IO_POLL_TIMEOUT))
                        .map_err(|e| NativeError::network(format!("set read timeout: {e}")))?;
                    stream
                        .set_write_timeout(Some(WRITE_TIMEOUT))
                        .map_err(|e| NativeError::network(format!("set write timeout: {e}")))?;
                    self.callbacks.log(format!("connected TCP to {addr}"));
                    return Ok(Some(stream));
                }
                Err(e) => last_error = Some(format!("{addr}: {e}")),
            }
        }

        Err(NativeError::network(format!(
            "TCP connect failed: {}",
            last_error.unwrap_or_else(|| "no attempted address".to_owned())
        )))
    }

    fn new_connector(&self, client_addr: SocketAddr) -> ClientConnector {
        let config = Config {
            credentials: Credentials::UsernamePassword {
                username: self.config.username.clone(),
                password: self.config.password.clone(),
            },
            domain: if self.config.domain.is_empty() {
                None
            } else {
                Some(self.config.domain.clone())
            },
            enable_tls: true,
            enable_credssp: true,
            keyboard_type: KeyboardType::IbmEnhanced,
            keyboard_subtype: 0,
            keyboard_layout: 0,
            keyboard_functional_keys_count: 12,
            ime_file_name: String::new(),
            dig_product_id: String::new(),
            desktop_size: DesktopSize {
                width: self.config.width,
                height: self.config.height,
            },
            bitmap: None,
            client_build: 0,
            client_name: "gnomecast-native".to_owned(),
            client_dir: "C:\\Windows\\System32\\mstscax.dll".to_owned(),
            platform: MajorPlatformType::UNSPECIFIED,
            compression_type: None,
            // The server ships cursor shapes as pointer updates (gnome-remote-desktop never
            // embeds them into the video); they are forwarded to the C shell for rendering.
            enable_server_pointer: true,
            autologon: false,
            // Clears the NO_AUDIO_PLAYBACK client-info flag; without this the server
            // never streams audio regardless of the AUDIO_PLAYBACK_DVC channel below.
            enable_audio_playback: true,
            request_data: None,
            pointer_software_rendering: false,
            multitransport_flags: None,
            performance_flags: PerformanceFlags::ENABLE_FONT_SMOOTHING
                | PerformanceFlags::ENABLE_DESKTOP_COMPOSITION,
            desktop_scale_factor: 0,
            hardware_id: None,
            license_cache: None,
            timezone_info: TimezoneInfo::default(),
            alternate_shell: String::new(),
            work_dir: String::new(),
        };

        let mut connector = ClientConnector::new(config, client_addr);
        let drdynvc = DrdynvcClient::new()
            .with_dynamic_channel(GraphicsPipelineClient::new(
                Box::new(NativeGfxHandler {
                    shared: Arc::clone(&self.gfx),
                }),
                None,
            ))
            .with_dynamic_channel(RdpsndDvcHandler::new(
                self.callbacks,
                self.config.prefer_pcm_audio,
            ))
            .with_dynamic_channel(make_display_control(
                self.config.width,
                self.config.height,
                self.callbacks,
            ));
        connector.attach_static_channel(drdynvc);
        connector
    }

    fn send_x224_request(
        &mut self,
        tcp: &mut TcpStream,
        connector: &mut ClientConnector,
    ) -> Result<(), NativeError> {
        let mut out = WriteBuf::new();
        connector
            .step_no_input(&mut out)
            .map_err(|e| NativeError::protocol(format!("X.224 negotiation request: {e}")))?;
        self.write_all(tcp, out.filled(), "X.224 negotiation request")?;

        let confirm = self.read_connector_pdu(tcp, connector, "X.224 negotiation response")?;
        let mut out = WriteBuf::new();
        connector
            .step(&confirm, &mut out)
            .map_err(|e| NativeError::protocol(format!("X.224 negotiation response: {e}")))?;
        if !out.filled().is_empty() {
            self.write_all(tcp, out.filled(), "X.224 negotiation follow-up")?;
        }
        if !connector.should_perform_security_upgrade() {
            return Err(NativeError::protocol(
                "server did not select enhanced TLS security",
            ));
        }
        Ok(())
    }

    fn upgrade_tls(
        &mut self,
        tcp: TcpStream,
    ) -> Result<
        (
            rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
            Vec<u8>,
        ),
        NativeError,
    > {
        install_rustls_provider();
        let mut config = rustls::ClientConfig::builder()
            .dangerous()
            .with_custom_certificate_verifier(Arc::new(NoCertificateVerification))
            .with_no_client_auth();
        config.key_log = Arc::new(rustls::KeyLogFile::new());
        config.resumption = rustls::client::Resumption::disabled();

        let server_name = rustls::pki_types::ServerName::try_from(self.config.host.clone())
            .map_err(|e| {
                NativeError::network(format!("invalid TLS server name {}: {e}", self.config.host))
            })?;
        let conn = rustls::ClientConnection::new(Arc::new(config), server_name)
            .map_err(|e| NativeError::network(format!("create TLS client: {e}")))?;
        let mut tls = rustls::StreamOwned::new(conn, tcp);

        while tls.conn.is_handshaking() {
            if self.poll_stop() {
                return Err(NativeError::network("TLS handshake stopped"));
            }
            match tls.conn.complete_io(&mut tls.sock) {
                Ok(_) => {}
                Err(e) if is_timeout(&e) => {}
                Err(e) => return Err(NativeError::network(format!("TLS handshake: {e}"))),
            }
        }
        tls.flush()
            .map_err(|e| NativeError::network(format!("TLS flush after handshake: {e}")))?;

        let cert = tls
            .conn
            .peer_certificates()
            .and_then(|certs| certs.first())
            .ok_or_else(|| NativeError::network("TLS peer certificate is missing"))?;
        let parsed = x509_cert::Certificate::from_der(cert.as_ref())
            .map_err(|e| NativeError::network(format!("TLS peer certificate parse: {e}")))?;
        let public_key = parsed
            .tbs_certificate
            .subject_public_key_info
            .subject_public_key
            .as_bytes()
            .ok_or_else(|| NativeError::network("TLS subject public key is not byte-aligned"))?
            .to_vec();
        Ok((tls, public_key))
    }

    fn run_credssp(
        &mut self,
        tls: &mut rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
        connector: &mut ClientConnector,
        public_key: Vec<u8>,
    ) -> Result<(), NativeError> {
        let client_challenge = random_array::<8>()?;
        let key_exch_key = random_array::<16>()?;
        let client_nonce = random_array::<32>()?;
        let mut credssp = CredsspClient::new(
            public_key,
            &self.config.username,
            &self.config.domain,
            &self.config.password,
            client_challenge,
            key_exch_key,
            client_nonce,
        );

        let first = credssp.first_message();
        self.write_all(tls, &first, "CredSSP negotiate")?;

        while !credssp.is_done() {
            let server_ts = self.read_ts_request(tls)?;
            let (next, done) = credssp
                .process_server(&server_ts)
                .map_err(|e| NativeError::protocol(format!("CredSSP: {e}")))?;
            self.write_all(tls, &next, "CredSSP response")?;
            if done {
                connector.mark_credssp_as_done();
                return Ok(());
            }
        }
        Ok(())
    }

    fn pump_connector(
        &mut self,
        tls: &mut rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
        mut connector: ClientConnector,
    ) -> Result<ConnectionResult, NativeError> {
        loop {
            if self.poll_stop() {
                return Err(NativeError::network("stopped before ActiveStage"));
            }
            if matches!(connector.state, ClientConnectorState::Connected { .. }) {
                break;
            }

            let mut out = WriteBuf::new();
            if connector.next_pdu_hint().is_some() {
                let pdu = self.read_connector_pdu(tls, &connector, "connector input")?;
                connector
                    .step(&pdu, &mut out)
                    .map_err(|e| NativeError::protocol(format!("connector step: {e}")))?;
            } else {
                connector
                    .step_no_input(&mut out)
                    .map_err(|e| NativeError::protocol(format!("connector step_no_input: {e}")))?;
            }
            if !out.filled().is_empty() {
                self.write_all(tls, out.filled(), "connector output")?;
            }
        }

        match connector.state {
            ClientConnectorState::Connected { result } => Ok(result),
            other => Err(NativeError::protocol(format!(
                "connector ended in unexpected state {other:?}"
            ))),
        }
    }

    fn run_active(
        &mut self,
        mut tls: rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
        result: ConnectionResult,
    ) -> Result<(), NativeError> {
        let desktop_w = result.desktop_size.width;
        let desktop_h = result.desktop_size.height;
        self.callbacks.desktop_size(desktop_w, desktop_h);
        self.callbacks.emit_state(
            RdpState::Active,
            format!("active {}x{} native AVC420/RemoteFX", desktop_w, desktop_h),
        );

        let mut active = ActiveStage::new(result);
        let mut image = DecodedImage::new(PixelFormat::RgbA32, desktop_w, desktop_h);

        // Discard any input poll_stop buffered during the pre-connect phase; the C side does
        // not send input until the session is active, so this is normally empty, and replaying
        // stale pre-connect events into a fresh session would be wrong.
        self.pending_input.clear();

        // Re-assert a commanded suppression on this fresh connection (see the latch field)
        // — unless ANY suppress toggle is already pending: a queued resume (the user
        // switched to this session mid-reconnect) is newer intent and must not be
        // overridden by the stale latch.
        let suppress_toggle_pending = self
            .pending_control
            .iter()
            .any(|c| matches!(c, ControlCommand::SuppressOutput { .. }));
        if self.suppress_display_latched && !suppress_toggle_pending {
            self.pending_control.push(ControlCommand::SuppressOutput {
                allow_display: false,
            });
        }

        loop {
            if self.stop.load(Ordering::SeqCst) {
                return Ok(());
            }
            self.drain_input(&mut tls, &mut active, &mut image)?;
            if self.stop.load(Ordering::SeqCst) {
                return Ok(());
            }
            self.drain_gfx()?;

            while let Some(info) = ironrdp_pdu::find_size(&self.inbuf)
                .map_err(|e| NativeError::protocol(format!("active PDU size: {e}")))?
            {
                if self.inbuf.len() < info.length {
                    break;
                }
                let frame: Vec<u8> = self.inbuf.drain(..info.length).collect();
                let outputs = active
                    .process(&mut image, info.action, &frame)
                    .map_err(|e| NativeError::protocol(format!("active stage process: {e}")))?;
                let mut deactivate = None;
                for output in outputs {
                    match output {
                        ActiveStageOutput::ResponseFrame(frame) => {
                            self.write_all(&mut tls, &frame, "active response")?
                        }
                        ActiveStageOutput::Terminate(reason) => {
                            // Any Terminate we receive is server-side origin (a client
                            // stop closes the TCP stream without one), and
                            // gnome-remote-desktop sends its handoff ultimatum with the
                            // reason wired as UserRequested — so every reason must reach
                            // the reconnect loop in run(), which matches this message and
                            // is guarded by the stop flag for genuine client stops.
                            return Err(NativeError::protocol(format!(
                                "received disconnect provider ultimatum: {}",
                                reason.description()
                            )));
                        }
                        ActiveStageOutput::DeactivateAll(seq) => deactivate = Some(seq),
                        ActiveStageOutput::GraphicsUpdate(rect) => {
                            // Classic slow-path/fast-path bitmap updates (servers without
                            // EGFX/H.264) are decoded by IronRDP directly into `image`; forward
                            // the changed region to the native presenter the same way EGFX
                            // RemoteFX tiles are. `data_for_rect` returns a slice through the
                            // full image buffer, so the row stride is the image's own stride,
                            // not width * bytes-per-pixel.
                            let width = u32::from(rect.width());
                            let height = u32::from(rect.height());
                            if width > 0 && height > 0 {
                                self.callbacks.bitmap_update(&NativeBitmapUnit {
                                    surface_id: 0,
                                    left: u32::from(rect.left),
                                    top: u32::from(rect.top),
                                    width,
                                    height,
                                    stride: image.stride() as u32,
                                    data: image.data_for_rect(&rect).to_vec(),
                                });
                            }
                        }
                        ActiveStageOutput::PointerBitmap(pointer) => {
                            // A zero-dimension shape is IronRDP's decoded form of an
                            // "invisible" server pointer (DecodedPointer::new_invisible); the
                            // C side rejects empty bitmaps, so translate it to a hide request
                            // rather than dropping it and leaving the cursor visible.
                            if pointer.width == 0 || pointer.height == 0 {
                                self.callbacks.pointer_state(RDP_POINTER_STATE_HIDDEN);
                            } else {
                                self.callbacks.pointer_bitmap(&pointer);
                            }
                        }
                        ActiveStageOutput::PointerPosition { x, y } => {
                            self.callbacks.pointer_position(x, y);
                        }
                        ActiveStageOutput::PointerHidden => {
                            self.callbacks.pointer_state(RDP_POINTER_STATE_HIDDEN);
                        }
                        ActiveStageOutput::PointerDefault => {
                            self.callbacks.pointer_state(RDP_POINTER_STATE_DEFAULT);
                        }
                        _ => {}
                    }
                }
                self.drain_gfx()?;

                if let Some(seq) = deactivate {
                    self.reactivation = Some(seq);
                    self.drive_reactivation(&mut tls, &mut active, &mut image)?;
                }
            }

            let mut buf = [0u8; 8192];
            match tls.read(&mut buf) {
                Ok(0) => return Err(NativeError::network("RDP server closed the TLS stream")),
                Ok(n) => self.inbuf.extend_from_slice(&buf[..n]),
                Err(e) if is_timeout(&e) => {}
                Err(e) => return Err(NativeError::network(format!("active read: {e}"))),
            }
        }
    }

    fn dispatch_input(
        &mut self,
        tls: &mut rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
        input: InputCommand,
    ) -> Result<(), NativeError> {
        let events = input.into_events();
        let outputs = active
            .process_fastpath_input(image, &events)
            .map_err(|e| NativeError::protocol(format!("fast-path input: {e}")))?;
        for output in outputs {
            if let ActiveStageOutput::ResponseFrame(frame) = output {
                self.write_all(tls, &frame, "fast-path input")?;
            }
        }
        Ok(())
    }

    fn drain_input(
        &mut self,
        tls: &mut rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
    ) -> Result<(), NativeError> {
        // Replay anything poll_stop buffered while the worker was busy (e.g. during
        // reactivation) before draining fresh events, preserving order.
        if !self.pending_control.is_empty() {
            for control in std::mem::take(&mut self.pending_control) {
                self.dispatch_control(tls, active, image, control)?;
            }
        }
        if !self.pending_input.is_empty() {
            for input in std::mem::take(&mut self.pending_input) {
                self.dispatch_input(tls, active, image, input)?;
            }
        }
        loop {
            match self.rx.try_recv() {
                Ok(WorkerCommand::Stop) => {
                    self.stop.store(true, Ordering::SeqCst);
                    return Ok(());
                }
                Ok(WorkerCommand::Input(input)) => {
                    self.dispatch_input(tls, active, image, input)?
                }
                Ok(WorkerCommand::Control(control)) => {
                    self.dispatch_control(tls, active, image, control)?
                }
                Err(TryRecvError::Empty) => return Ok(()),
                Err(TryRecvError::Disconnected) => {
                    self.stop.store(true, Ordering::SeqCst);
                    return Ok(());
                }
            }
        }
    }

    /// Sends one session-control request on the live connection. Failures here are
    /// connection failures (the encode paths are infallible for valid state), so they
    /// propagate like any other write error.
    fn dispatch_control(
        &mut self,
        tls: &mut rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
        control: ControlCommand,
    ) -> Result<(), NativeError> {
        let full_rect = InclusiveRectangle {
            left: 0,
            top: 0,
            right: image.width().saturating_sub(1),
            bottom: image.height().saturating_sub(1),
        };
        match control {
            ControlCommand::SuppressOutput { allow_display } => {
                self.suppress_display_latched = !allow_display;
                let pdu = ShareDataPdu::SuppressOutput(SuppressOutputPdu {
                    // Per MS-RDPBCGR the desktop rectangle is present exactly when display
                    // updates are re-allowed.
                    desktop_rect: allow_display.then_some(full_rect),
                });
                let mut buf = WriteBuf::new();
                active
                    .encode_static(&mut buf, pdu)
                    .map_err(|e| NativeError::protocol(format!("suppress output encode: {e}")))?;
                self.write_all(tls, buf.filled(), "suppress output")?;
                self.callbacks.log(format!(
                    "display updates {} by client request",
                    if allow_display {
                        "resumed"
                    } else {
                        "suppressed"
                    }
                ));
            }
            ControlCommand::RequestRefresh => {
                // gnome-remote-desktop disables TS_REFRESH_RECT_PDU (FreeRDP_RefreshRect =
                // FALSE) and does not force a keyframe when suppressed output resumes, but
                // it tears down and recreates its encode sessions on EVERY Display Control
                // monitor-layout submission — even a byte-identical one — ending in a
                // RESET_GRAPHICS and a fresh IDR. So re-submit the current layout over the
                // Display Control DVC (when the server opened it; mirror-mode grd does
                // not), and also send a full-screen Refresh Rect for servers that honor
                // the classic path.
                let layout_messages = {
                    let (width, height) = MonitorLayoutEntry::adjust_display_size(
                        u32::from(image.width()),
                        u32::from(image.height()),
                    );
                    match active.get_dvc::<DisplayControlClient>() {
                        Some(dvc) => match (
                            dvc.channel_id(),
                            dvc.channel_processor_downcast_ref::<DisplayControlClient>(),
                        ) {
                            (Some(channel_id), Some(client)) if client.ready() => client
                                .encode_single_primary_monitor(
                                    channel_id, width, height, None, None,
                                )
                                .map_err(|e| {
                                    NativeError::protocol(format!("refresh layout encode: {e}"))
                                })
                                .map(Some)?,
                            _ => None,
                        },
                        None => None,
                    }
                };
                if let Some(messages) = layout_messages {
                    let frame = active.encode_dvc_messages(messages).map_err(|e| {
                        NativeError::protocol(format!("refresh layout DVC encode: {e}"))
                    })?;
                    self.write_all(tls, &frame, "refresh monitor layout")?;
                    self.callbacks
                        .log("refresh requested: re-submitted monitor layout for a fresh keyframe");
                } else {
                    self.callbacks.log(
                        "refresh requested but the display-control channel is unavailable; \
                         relying on Refresh Rect only",
                    );
                }

                let pdu = ShareDataPdu::RefreshRectangle(RefreshRectanglePdu {
                    areas_to_refresh: vec![full_rect],
                });
                let mut buf = WriteBuf::new();
                active
                    .encode_static(&mut buf, pdu)
                    .map_err(|e| NativeError::protocol(format!("refresh rect encode: {e}")))?;
                self.write_all(tls, buf.filled(), "refresh rect")?;
            }
        }
        Ok(())
    }

    fn drive_reactivation(
        &mut self,
        tls: &mut rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
        active: &mut ActiveStage,
        image: &mut DecodedImage,
    ) -> Result<(), NativeError> {
        let mut seq = match self.reactivation.take() {
            Some(seq) => seq,
            None => return Ok(()),
        };
        let mut out = WriteBuf::new();
        loop {
            if let ConnectionActivationState::Finalized {
                io_channel_id,
                user_channel_id,
                desktop_size,
                share_id,
                enable_server_pointer,
                pointer_software_rendering,
            } = seq.connection_activation_state()
            {
                *image =
                    DecodedImage::new(PixelFormat::RgbA32, desktop_size.width, desktop_size.height);
                active.set_fastpath_processor(
                    fast_path::ProcessorBuilder {
                        io_channel_id,
                        user_channel_id,
                        share_id,
                        enable_server_pointer,
                        pointer_software_rendering,
                        bulk_decompressor: None,
                    }
                    .build(),
                );
                active.set_share_id(share_id);
                active.set_enable_server_pointer(enable_server_pointer);
                self.callbacks
                    .desktop_size(desktop_size.width, desktop_size.height);
                self.write_all(tls, out.filled(), "reactivation finalized")?;
                return Ok(());
            }

            if seq.next_pdu_hint().is_some() {
                let pdu = self.read_activation_pdu(tls, &seq)?;
                seq.step(&pdu, &mut out)
                    .map_err(|e| NativeError::protocol(format!("reactivation step: {e}")))?;
            } else {
                seq.step_no_input(&mut out).map_err(|e| {
                    NativeError::protocol(format!("reactivation step_no_input: {e}"))
                })?;
            }

            if !out.filled().is_empty() {
                self.write_all(tls, out.filled(), "reactivation output")?;
                out = WriteBuf::new();
            }
        }
    }

    fn drain_gfx(&mut self) -> Result<(), NativeError> {
        let (video_units, bitmap_units, unsupported_graphics, graphics_size) = match self.gfx.lock()
        {
            Ok(mut shared) => {
                let graphics_size = if shared.graphics_size_pending {
                    shared.graphics_size_pending = false;
                    Some((shared.graphics_width, shared.graphics_height))
                } else {
                    None
                };
                (
                    std::mem::take(&mut shared.pending_video),
                    std::mem::take(&mut shared.pending_bitmap),
                    shared.unsupported_graphics.take(),
                    graphics_size,
                )
            }
            Err(_) => return Err(NativeError::protocol("native graphics state lock poisoned")),
        };
        if let Some(detail) = unsupported_graphics {
            return Err(NativeError::no_avc420(detail));
        }
        // Dispatched before video/bitmap units so both the ss4s/H.264 and RemoteFX paths
        // see the server's real graphics output size before processing this batch's frames.
        if let Some((width, height)) = graphics_size {
            let width = width.min(u32::from(u16::MAX)) as u16;
            let height = height.min(u32::from(u16::MAX)) as u16;
            self.callbacks.desktop_size(width, height);
        }
        for unit in bitmap_units {
            self.callbacks.bitmap_update(&unit);
        }
        for unit in video_units {
            let pts = self.next_pts90k;
            self.next_pts90k = self.next_pts90k.wrapping_add(self.frame_pts_step.max(1));
            self.callbacks.video_au(&unit.data, unit.is_keyframe, pts);
        }
        Ok(())
    }

    fn read_connector_pdu<T: Read>(
        &mut self,
        stream: &mut T,
        connector: &ClientConnector,
        label: &str,
    ) -> Result<Vec<u8>, NativeError> {
        let hint = connector
            .next_pdu_hint()
            .ok_or_else(|| NativeError::protocol(format!("{label}: connector has no PDU hint")))?;
        loop {
            if self.poll_stop() {
                return Err(NativeError::network(format!("{label}: stopped")));
            }
            match hint
                .find_size(&self.inbuf)
                .map_err(|e| NativeError::protocol(format!("{label}: PDU size: {e}")))?
            {
                Some((_matched, size)) if self.inbuf.len() >= size => {
                    return Ok(self.inbuf.drain(..size).collect())
                }
                _ => self.read_more(stream, label)?,
            }
        }
    }

    fn read_activation_pdu(
        &mut self,
        stream: &mut rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
        seq: &ironrdp_connector::connection_activation::ConnectionActivationSequence,
    ) -> Result<Vec<u8>, NativeError> {
        let hint = seq
            .next_pdu_hint()
            .ok_or_else(|| NativeError::protocol("reactivation has no PDU hint"))?;
        loop {
            if self.poll_stop() {
                return Err(NativeError::network("reactivation stopped"));
            }
            match hint
                .find_size(&self.inbuf)
                .map_err(|e| NativeError::protocol(format!("reactivation PDU size: {e}")))?
            {
                Some((_matched, size)) if self.inbuf.len() >= size => {
                    return Ok(self.inbuf.drain(..size).collect())
                }
                _ => self.read_more(stream, "reactivation input")?,
            }
        }
    }

    fn read_ts_request(
        &mut self,
        stream: &mut rustls::StreamOwned<rustls::ClientConnection, TcpStream>,
    ) -> Result<Vec<u8>, NativeError> {
        loop {
            if self.poll_stop() {
                return Err(NativeError::network("CredSSP stopped"));
            }
            if let Some(total) = ts_request_len(&self.inbuf) {
                if self.inbuf.len() >= total {
                    return Ok(self.inbuf.drain(..total).collect());
                }
            }
            self.read_more(stream, "CredSSP TSRequest")?;
        }
    }

    fn read_more<T: Read>(&mut self, stream: &mut T, label: &str) -> Result<(), NativeError> {
        let mut buf = [0u8; 8192];
        match stream.read(&mut buf) {
            Ok(0) => Err(NativeError::network(format!(
                "{label}: peer closed connection"
            ))),
            Ok(n) => {
                self.inbuf.extend_from_slice(&buf[..n]);
                Ok(())
            }
            Err(e) if is_timeout(&e) => Ok(()),
            Err(e) => Err(NativeError::network(format!("{label}: read: {e}"))),
        }
    }

    fn write_all<T: Write>(
        &mut self,
        stream: &mut T,
        bytes: &[u8],
        label: &str,
    ) -> Result<(), NativeError> {
        if bytes.is_empty() {
            return Ok(());
        }
        stream
            .write_all(bytes)
            .map_err(|e| NativeError::network(format!("{label}: write: {e}")))?;
        stream
            .flush()
            .map_err(|e| NativeError::network(format!("{label}: flush: {e}")))
    }

    fn poll_stop(&mut self) -> bool {
        if self.stop.load(Ordering::SeqCst) {
            return true;
        }
        loop {
            match self.rx.try_recv() {
                Ok(WorkerCommand::Stop) => {
                    self.stop.store(true, Ordering::SeqCst);
                    return true;
                }
                Ok(WorkerCommand::Input(input)) => {
                    // Draining keeps the SDL thread from blocking on a full channel while the
                    // worker is busy. Buffer the events instead of dropping them: pre-connect
                    // this buffer is cleared when run_active starts, but during an in-session
                    // Deactivate-Reactivate ActiveStage still exists and a dropped button/key
                    // release would stick — the next drain_input replays what is buffered here.
                    // Consecutive pointer moves coalesce (idempotent) so a fast pointer cannot
                    // grow the buffer without bound.
                    match (self.pending_input.last_mut(), &input) {
                        (
                            Some(InputCommand::PointerMove { x, y }),
                            InputCommand::PointerMove { x: nx, y: ny },
                        ) => {
                            *x = *nx;
                            *y = *ny;
                        }
                        _ => self.pending_input.push(input),
                    }
                }
                Ok(WorkerCommand::Control(control)) => {
                    // Only the final suppress-output state matters, and one queued refresh
                    // is as good as many; keep the buffer minimal.
                    match control {
                        ControlCommand::SuppressOutput { .. } => {
                            self.pending_control
                                .retain(|c| !matches!(c, ControlCommand::SuppressOutput { .. }));
                            self.pending_control.push(control);
                        }
                        ControlCommand::RequestRefresh => {
                            if !self
                                .pending_control
                                .contains(&ControlCommand::RequestRefresh)
                            {
                                self.pending_control.push(control);
                            }
                        }
                    }
                }
                Err(TryRecvError::Empty) => return false,
                Err(TryRecvError::Disconnected) => {
                    self.stop.store(true, Ordering::SeqCst);
                    return true;
                }
            }
        }
    }
}

unsafe fn cstr_lossy(ptr: *const c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_string_lossy()
        .into_owned()
}

fn cstring_lossy(value: &str) -> CString {
    let bytes: Vec<u8> = value
        .as_bytes()
        .iter()
        .copied()
        .filter(|b| *b != 0)
        .collect();
    CString::new(bytes).expect("interior NUL bytes are removed")
}

unsafe fn copy_config(config: *const RdpConfig) -> Result<NativeConfig, &'static str> {
    let config = unsafe { config.as_ref() }.ok_or("missing config")?;
    let host = unsafe { cstr_lossy(config.host) };
    if host.is_empty() {
        return Err("missing host");
    }
    Ok(NativeConfig {
        host,
        port: if config.port == 0 { 3389 } else { config.port },
        username: unsafe { cstr_lossy(config.username) },
        password: unsafe { cstr_lossy(config.password) },
        domain: unsafe { cstr_lossy(config.domain) },
        width: config.width.max(1),
        height: config.height.max(1),
        fps: config.fps.max(1),
        prefer_pcm_audio: config.prefer_pcm_audio != 0,
    })
}

fn worker_main(
    config: NativeConfig,
    callbacks: CallbackSink,
    rx: Receiver<WorkerCommand>,
    stop: Arc<AtomicBool>,
) {
    crate::init_logging();
    let mut worker = NativeWorker::new(config, callbacks, rx, stop);
    match worker.run() {
        Ok(()) => {}
        Err(err) => {
            if !worker.stop.load(Ordering::SeqCst) {
                worker.callbacks.emit_state(err.state, err.message);
            }
        }
    }
    worker.callbacks.emit_state(RdpState::Stopped, "stopped");
}

fn enqueue_command(session: *mut RdpSession, command: WorkerCommand) {
    let Some(session) = (unsafe { session.as_ref() }) else {
        return;
    };
    match session.tx.try_send(command) {
        Ok(()) => {}
        // The receiver is gone (worker stopped); nothing to deliver to.
        Err(TrySendError::Disconnected(_)) => {}
        Err(TrySendError::Full(cmd)) => {
            // Pointer moves are idempotent — only the latest position matters — so dropping
            // one under backpressure is correct and avoids a stale backlog. Every other
            // event is state-changing: silently dropping a button-up / key-up would leave
            // the server with a stuck button (phantom drag) or an auto-repeating key, and a
            // dropped suppress-output toggle would blank or waste the wrong session. Fall
            // back to a blocking send for those; it is bounded — the worker either drains a
            // slot or drops the receiver on stop / write-timeout, which unblocks with Err.
            if !matches!(cmd, WorkerCommand::Input(InputCommand::PointerMove { .. })) {
                let _ = session.tx.send(cmd);
            }
        }
    }
}

fn enqueue(session: *mut RdpSession, input: InputCommand) {
    enqueue_command(session, WorkerCommand::Input(input));
}

fn random_array<const N: usize>() -> Result<[u8; N], NativeError> {
    let mut a = [0u8; N];
    getrandom::fill(&mut a)
        .map_err(|e| NativeError::network(format!("entropy unavailable: {e}")))?;
    Ok(a)
}

fn ts_request_len(buf: &[u8]) -> Option<usize> {
    if buf.len() < 2 || buf[0] != 0x30 {
        return None;
    }
    let b1 = buf[1];
    if b1 < 0x80 {
        return Some(2 + b1 as usize);
    }
    let n = (b1 & 0x7f) as usize;
    if n == 0 || n > 4 || buf.len() < 2 + n {
        return None;
    }
    let mut len = 0usize;
    for i in 0..n {
        len = (len << 8) | buf[2 + i] as usize;
    }
    Some(2 + n + len)
}

fn avc_length_prefixed_has_keyframe(data: &[u8]) -> bool {
    avc_length_prefixed_has_nal_type(data, |nal_type| matches!(nal_type, 5 | 7))
}

fn avc_length_prefixed_has_nal_type(data: &[u8], mut pred: impl FnMut(u8) -> bool) -> bool {
    let mut pos = 0usize;
    while pos + 4 <= data.len() {
        let len =
            u32::from_be_bytes([data[pos], data[pos + 1], data[pos + 2], data[pos + 3]]) as usize;
        pos += 4;
        if len == 0 || pos + len > data.len() {
            return false;
        }
        let nal_type = data[pos] & 0x1f;
        if pred(nal_type) {
            return true;
        }
        pos += len;
    }
    false
}

fn is_timeout(e: &io::Error) -> bool {
    matches!(
        e.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted
    )
}

fn install_rustls_provider() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
}

#[derive(Debug)]
struct NoCertificateVerification;

impl rustls::client::danger::ServerCertVerifier for NoCertificateVerification {
    fn verify_server_cert(
        &self,
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &[rustls::pki_types::CertificateDer<'_>],
        _: &rustls::pki_types::ServerName<'_>,
        _: &[u8],
        _: rustls::pki_types::UnixTime,
    ) -> Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _: &[u8],
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _: &[u8],
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        vec![
            rustls::SignatureScheme::RSA_PKCS1_SHA1,
            rustls::SignatureScheme::ECDSA_SHA1_Legacy,
            rustls::SignatureScheme::RSA_PKCS1_SHA256,
            rustls::SignatureScheme::ECDSA_NISTP256_SHA256,
            rustls::SignatureScheme::RSA_PKCS1_SHA384,
            rustls::SignatureScheme::ECDSA_NISTP384_SHA384,
            rustls::SignatureScheme::RSA_PKCS1_SHA512,
            rustls::SignatureScheme::ECDSA_NISTP521_SHA512,
            rustls::SignatureScheme::RSA_PSS_SHA256,
            rustls::SignatureScheme::RSA_PSS_SHA384,
            rustls::SignatureScheme::RSA_PSS_SHA512,
            rustls::SignatureScheme::ED25519,
            rustls::SignatureScheme::ED448,
        ]
    }
}

/// Start a native RDP session and return an opaque handle for the C shell.
///
/// # Safety
///
/// `config` must point to a valid `RdpConfig` whose string pointers, when non-null,
/// are valid NUL-terminated C strings for the duration of this call. `callbacks`, when
/// non-null, must point to a valid `RdpCallbacks` table. The callback function pointers
/// must remain callable until `rdp_session_stop` returns.
#[no_mangle]
pub unsafe extern "C" fn rdp_session_start(
    config: *const RdpConfig,
    callbacks: *const RdpCallbacks,
) -> *mut RdpSession {
    let callbacks = unsafe { callbacks.as_ref() }
        .copied()
        .map(CallbackSink::new)
        .unwrap_or_else(CallbackSink::empty);
    let config = match unsafe { copy_config(config) } {
        Ok(config) => config,
        Err(detail) => {
            callbacks.emit_state(RdpState::ProtocolError, detail);
            return ptr::null_mut();
        }
    };

    let (tx, rx) = sync_channel(INPUT_QUEUE_DEPTH);
    let stop = Arc::new(AtomicBool::new(false));
    let worker_stop = Arc::clone(&stop);
    let worker_callbacks = callbacks;
    let worker = match thread::Builder::new()
        .name("rdp-worker".to_owned())
        .spawn(move || worker_main(config, worker_callbacks, rx, worker_stop))
    {
        Ok(worker) => worker,
        Err(e) => {
            callbacks.emit_state(RdpState::NetworkError, format!("spawn RDP worker: {e}"));
            return ptr::null_mut();
        }
    };

    Box::into_raw(Box::new(RdpSession {
        stop,
        tx,
        worker: Mutex::new(Some(worker)),
    }))
}

/// Stop and destroy a native RDP session created by `rdp_session_start`.
///
/// # Safety
///
/// `session` must be either null or a pointer returned by `rdp_session_start` that has
/// not already been passed to `rdp_session_stop`. After this call returns, the pointer is
/// invalid and no further native callbacks will be made by that session.
#[no_mangle]
pub unsafe extern "C" fn rdp_session_stop(session: *mut RdpSession) {
    if session.is_null() {
        return;
    }
    let session = unsafe { Box::from_raw(session) };
    session.stop.store(true, Ordering::SeqCst);
    let _ = session.tx.try_send(WorkerCommand::Stop);
    if let Ok(mut worker) = session.worker.lock() {
        if let Some(worker) = worker.take() {
            let _ = worker.join();
        }
    };
}

#[no_mangle]
pub extern "C" fn rdp_send_pointer_move(session: *mut RdpSession, x: u16, y: u16) {
    enqueue(session, InputCommand::PointerMove { x, y });
}

#[no_mangle]
pub extern "C" fn rdp_send_pointer_button(
    session: *mut RdpSession,
    x: u16,
    y: u16,
    button: u8,
    down: bool,
) {
    enqueue(session, InputCommand::PointerButton { x, y, button, down });
}

#[no_mangle]
pub extern "C" fn rdp_send_pointer_wheel(session: *mut RdpSession, x: u16, y: u16, delta: i16) {
    enqueue(session, InputCommand::PointerWheel { x, y, delta });
}

#[no_mangle]
pub extern "C" fn rdp_send_key(session: *mut RdpSession, scancode: u8, down: bool, extended: bool) {
    enqueue(
        session,
        InputCommand::Key {
            scancode,
            down,
            extended,
        },
    );
}

#[no_mangle]
pub extern "C" fn rdp_send_unicode(session: *mut RdpSession, codepoint: u16, down: bool) {
    enqueue(session, InputCommand::Unicode { codepoint, down });
}

/// Toggle server display updates for this session (TS_SUPPRESS_OUTPUT_PDU).
/// `allow_display=false` pauses graphics (rdpsnd audio keeps flowing on its DVC);
/// `true` resumes them. Note: gnome-remote-desktop resumes with a delta frame, not a
/// keyframe — pair with `rdp_request_refresh` when the local decoder lost its state.
#[no_mangle]
pub extern "C" fn rdp_set_suppress_output(session: *mut RdpSession, allow_display: bool) {
    enqueue_command(
        session,
        WorkerCommand::Control(ControlCommand::SuppressOutput { allow_display }),
    );
}

/// Ask the server for a fresh full frame / keyframe: re-submits the current monitor
/// layout on the Display Control DVC (forces gnome-remote-desktop to recreate its encode
/// session → RESET_GRAPHICS + IDR) and sends a full-screen Refresh Rect for servers that
/// honor the classic path.
#[no_mangle]
pub extern "C" fn rdp_request_refresh(session: *mut RdpSession) {
    enqueue_command(
        session,
        WorkerCommand::Control(ControlCommand::RequestRefresh),
    );
}

#[no_mangle]
pub extern "C" fn rdp_send_sync(
    session: *mut RdpSession,
    scroll_lock: bool,
    num_lock: bool,
    caps_lock: bool,
) {
    let mut flags = SynchronizeFlags::empty();
    if scroll_lock {
        flags |= SynchronizeFlags::SCROLL_LOCK;
    }
    if num_lock {
        flags |= SynchronizeFlags::NUM_LOCK;
    }
    if caps_lock {
        flags |= SynchronizeFlags::CAPS_LOCK;
    }
    enqueue(
        session,
        InputCommand::SyncLocks {
            flags: flags.bits(),
        },
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use ironrdp_pdu::geometry::ExclusiveRectangle;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn state_values_match_header() {
        assert_eq!(RdpState::Idle as u32, 0);
        assert_eq!(RdpState::Connecting as u32, 1);
        assert_eq!(RdpState::Tls as u32, 2);
        assert_eq!(RdpState::Credssp as u32, 3);
        assert_eq!(RdpState::Active as u32, 4);
        assert_eq!(RdpState::NoAvc420 as u32, 5);
        assert_eq!(RdpState::DecoderError as u32, 6);
        assert_eq!(RdpState::NetworkError as u32, 7);
        assert_eq!(RdpState::ProtocolError as u32, 8);
        assert_eq!(RdpState::Stopped as u32, 9);
        assert_eq!(size_of::<RdpState>(), size_of::<u32>());
    }

    #[test]
    fn retry_folds_pending_suppress_into_latch() {
        let (_tx, rx) = std::sync::mpsc::sync_channel::<WorkerCommand>(4);
        let callbacks = RdpCallbacks {
            ctx: core::ptr::null_mut(),
            on_state: None,
            on_log: None,
            on_desktop_size: None,
            on_video_au: None,
            on_bitmap_update: None,
            on_audio_format: None,
            on_audio_data: None,
            on_pointer_bitmap: None,
            on_pointer_position: None,
            on_pointer_state: None,
        };
        let config = NativeConfig {
            host: "test".to_owned(),
            port: 3389,
            username: String::new(),
            password: String::new(),
            domain: String::new(),
            width: 1,
            height: 1,
            fps: 30,
            prefer_pcm_audio: false,
        };
        let mut worker = NativeWorker::new(
            config,
            CallbackSink::new(callbacks),
            rx,
            Arc::new(AtomicBool::new(false)),
        );

        // A suppress queued during the failed session survives the retry as the latch;
        // the stale refresh drops (a fresh connection starts with an IDR anyway).
        worker.pending_control.push(ControlCommand::SuppressOutput {
            allow_display: false,
        });
        worker.pending_control.push(ControlCommand::RequestRefresh);
        worker.reset_session_state();
        assert!(worker.suppress_display_latched);
        assert!(worker.pending_control.is_empty());

        // A queued resume is newer intent and must clear a stale latched suppression.
        worker.pending_control.push(ControlCommand::SuppressOutput {
            allow_display: true,
        });
        worker.reset_session_state();
        assert!(!worker.suppress_display_latched);
        assert!(worker.pending_control.is_empty());
    }

    #[test]
    fn ffi_struct_layout_is_c_compatible() {
        assert_eq!(align_of::<RdpConfig>(), align_of::<*const c_char>());
        assert_eq!(offset_of!(RdpConfig, host), 0);
        assert_eq!(offset_of!(RdpConfig, port), size_of::<*const c_char>());
        assert!(offset_of!(RdpConfig, username) > offset_of!(RdpConfig, port));
        assert!(offset_of!(RdpConfig, password) > offset_of!(RdpConfig, username));
        assert!(offset_of!(RdpConfig, domain) > offset_of!(RdpConfig, password));
        assert!(offset_of!(RdpConfig, width) > offset_of!(RdpConfig, domain));
        assert!(offset_of!(RdpConfig, prefer_pcm_audio) > offset_of!(RdpConfig, fps));
        // prefer_pcm_audio must sit inside the old tail padding: same struct size as the
        // C header's _Static_asserts (48 on 64-bit, 28 on 32-bit) — host + padded port +
        // three more pointers, then 8 bytes of u16s/u8/tail padding.
        assert_eq!(size_of::<RdpConfig>(), 5 * size_of::<*const c_char>() + 8);
        assert_eq!(
            align_of::<RdpCallbacks>(),
            align_of::<*mut core::ffi::c_void>()
        );
        assert_eq!(offset_of!(RdpCallbacks, ctx), 0);
        assert!(offset_of!(RdpCallbacks, on_state) > offset_of!(RdpCallbacks, ctx));
        assert!(offset_of!(RdpCallbacks, on_video_au) > offset_of!(RdpCallbacks, on_desktop_size));
        assert!(offset_of!(RdpCallbacks, on_bitmap_update) > offset_of!(RdpCallbacks, on_video_au));
        assert!(
            offset_of!(RdpCallbacks, on_audio_format) > offset_of!(RdpCallbacks, on_bitmap_update)
        );
        assert!(
            offset_of!(RdpCallbacks, on_audio_data) > offset_of!(RdpCallbacks, on_audio_format)
        );
        assert!(
            offset_of!(RdpCallbacks, on_pointer_bitmap) > offset_of!(RdpCallbacks, on_audio_data)
        );
        assert!(
            offset_of!(RdpCallbacks, on_pointer_position)
                > offset_of!(RdpCallbacks, on_pointer_bitmap)
        );
        assert!(
            offset_of!(RdpCallbacks, on_pointer_state)
                > offset_of!(RdpCallbacks, on_pointer_position)
        );
    }

    #[test]
    fn pointer_state_constants_match_header() {
        assert_eq!(RDP_POINTER_STATE_HIDDEN, 0);
        assert_eq!(RDP_POINTER_STATE_DEFAULT, 1);
    }

    #[test]
    fn pointer_sink_forwards_shape_and_state() {
        use std::sync::Mutex as StdMutex;

        #[derive(Default)]
        struct Seen {
            bitmaps: Vec<(u16, u16, u16, u16, Vec<u8>)>,
            positions: Vec<(u16, u16)>,
            states: Vec<u32>,
        }

        extern "C" fn on_pointer_bitmap(
            ctx: *mut core::ffi::c_void,
            width: u16,
            height: u16,
            hotspot_x: u16,
            hotspot_y: u16,
            rgba: *const u8,
            len: usize,
        ) {
            let seen = unsafe { &*(ctx.cast::<StdMutex<Seen>>()) };
            let bytes = unsafe { core::slice::from_raw_parts(rgba, len) }.to_vec();
            seen.lock()
                .unwrap()
                .bitmaps
                .push((width, height, hotspot_x, hotspot_y, bytes));
        }
        extern "C" fn on_pointer_position(ctx: *mut core::ffi::c_void, x: u16, y: u16) {
            let seen = unsafe { &*(ctx.cast::<StdMutex<Seen>>()) };
            seen.lock().unwrap().positions.push((x, y));
        }
        extern "C" fn on_pointer_state(ctx: *mut core::ffi::c_void, state: u32) {
            let seen = unsafe { &*(ctx.cast::<StdMutex<Seen>>()) };
            seen.lock().unwrap().states.push(state);
        }

        let seen = StdMutex::new(Seen::default());
        let mut callbacks = CallbackSink::empty().callbacks;
        callbacks.ctx = (&seen as *const StdMutex<Seen>).cast_mut().cast();
        callbacks.on_pointer_bitmap = Some(on_pointer_bitmap);
        callbacks.on_pointer_position = Some(on_pointer_position);
        callbacks.on_pointer_state = Some(on_pointer_state);
        let sink = CallbackSink::new(callbacks);

        let pointer = DecodedPointer {
            width: 2,
            height: 1,
            hotspot_x: 1,
            hotspot_y: 0,
            bitmap_data: vec![0x10, 0x20, 0x30, 0xff, 0x40, 0x50, 0x60, 0x80],
        };
        sink.pointer_bitmap(&pointer);
        sink.pointer_position(123, 456);
        sink.pointer_state(RDP_POINTER_STATE_HIDDEN);
        sink.pointer_state(RDP_POINTER_STATE_DEFAULT);

        let seen = seen.lock().unwrap();
        assert_eq!(
            seen.bitmaps,
            vec![(2, 1, 1, 0, pointer.bitmap_data.clone())]
        );
        assert_eq!(seen.positions, vec![(123, 456)]);
        assert_eq!(seen.states, vec![0, 1]);
    }

    #[test]
    fn audio_codec_constants_match_header() {
        assert_eq!(RDP_AUDIO_CODEC_OPUS, 1);
        assert_eq!(RDP_AUDIO_CODEC_PCM_S16LE, 2);
    }

    #[test]
    fn surface_mutating_egfx_ops_stay_non_fatal() {
        use ironrdp_egfx::pdu::{Color, Point};

        let cases: [(&str, fn(&mut NativeGfxHandler)); 3] = [
            ("SolidFill", |handler| {
                handler.on_solid_fill(&SolidFillPdu {
                    surface_id: 1,
                    fill_pixel: Color {
                        b: 0,
                        g: 0,
                        r: 0,
                        xa: 0xff,
                    },
                    rectangles: Vec::new(),
                })
            }),
            ("SurfaceToSurface", |handler| {
                handler.on_surface_to_surface(&SurfaceToSurfacePdu {
                    source_surface_id: 1,
                    destination_surface_id: 2,
                    source_rectangle: ExclusiveRectangle {
                        left: 0,
                        top: 0,
                        right: 1,
                        bottom: 1,
                    },
                    destination_points: Vec::new(),
                })
            }),
            ("CacheToSurface", |handler| {
                handler.on_cache_to_surface(&CacheToSurfacePdu {
                    cache_slot: 1,
                    surface_id: 1,
                    destination_points: vec![Point { x: 0, y: 0 }],
                })
            }),
        ];
        for (name, invoke) in cases {
            let mut handler = NativeGfxHandler {
                shared: Arc::new(Mutex::new(NativeGfxState::default())),
            };
            invoke(&mut handler);
            // gnome-remote-desktop sends these during routine AVC420 sessions; they must
            // never take the session down (regression: they briefly did).
            assert!(
                handler
                    .shared
                    .lock()
                    .unwrap()
                    .unsupported_graphics
                    .is_none(),
                "{name} must not mark graphics unsupported"
            );
        }
    }

    #[test]
    fn reset_graphics_marks_size_pending_once_per_distinct_value() {
        let mut handler = NativeGfxHandler {
            shared: Arc::new(Mutex::new(NativeGfxState::default())),
        };

        // The server's real graphics output can be larger than the negotiated MCS/GCC
        // desktop size (e.g. a TV whose hardware decoder always runs at panel resolution).
        // This must be dispatched to both the ss4s/H.264 and RemoteFX paths uniformly, so
        // it goes through on_desktop_size (see drain_gfx) rather than being attached only
        // to AVC420 access units.
        handler.on_reset_graphics(3840, 2160);
        {
            let shared = handler.shared.lock().unwrap();
            assert!(shared.graphics_size_pending);
            assert_eq!(shared.graphics_width, 3840);
            assert_eq!(shared.graphics_height, 2160);
        }

        // Repeating the same size should not re-flag it as pending (drain_gfx already
        // cleared the flag after dispatching it once).
        {
            let mut shared = handler.shared.lock().unwrap();
            shared.graphics_size_pending = false;
        }
        handler.on_reset_graphics(3840, 2160);
        assert!(!handler.shared.lock().unwrap().graphics_size_pending);

        // A genuinely new size must be re-flagged.
        handler.on_reset_graphics(1920, 1080);
        let shared = handler.shared.lock().unwrap();
        assert!(shared.graphics_size_pending);
        assert_eq!(shared.graphics_width, 1920);
        assert_eq!(shared.graphics_height, 1080);
    }

    #[test]
    fn bitmap_update_offsets_by_mapped_surface_origin() {
        let mut handler = NativeGfxHandler {
            shared: Arc::new(Mutex::new(NativeGfxState::default())),
        };

        // Server maps surface 7 at a non-zero desktop position (e.g. a second monitor).
        handler.on_map_surface_to_output(&MapSurfaceToOutputPdu {
            surface_id: 7,
            output_origin_x: 1920,
            output_origin_y: 100,
        });

        handler.on_bitmap_updated(&BitmapUpdate {
            surface_id: 7,
            destination_rectangle: ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            codec_id: Codec1Type::Uncompressed,
            data: vec![0u8; 2 * 2 * 4],
            width: 2,
            height: 2,
        });

        let shared = handler.shared.lock().unwrap();
        assert_eq!(shared.pending_bitmap.len(), 1);
        assert_eq!(shared.pending_bitmap[0].left, 1920 + 10);
        assert_eq!(shared.pending_bitmap[0].top, 100 + 20);
        drop(shared);

        // A ResetGraphics implicitly destroys all surfaces; a stale origin must not leak into
        // a later bitmap update on a reused surface id that hasn't been re-mapped.
        handler.on_reset_graphics(3840, 2160);
        {
            let mut shared = handler.shared.lock().unwrap();
            shared.pending_bitmap.clear();
        }
        handler.on_bitmap_updated(&BitmapUpdate {
            surface_id: 7,
            destination_rectangle: ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            codec_id: Codec1Type::Uncompressed,
            data: vec![0u8; 2 * 2 * 4],
            width: 2,
            height: 2,
        });
        {
            let shared = handler.shared.lock().unwrap();
            assert_eq!(shared.pending_bitmap[0].left, 10);
            assert_eq!(shared.pending_bitmap[0].top, 20);
        }

        // DeleteSurface destroys a single surface the same way; a reused id must start
        // unmapped rather than inherit the deleted surface's origin.
        handler.on_map_surface_to_output(&MapSurfaceToOutputPdu {
            surface_id: 7,
            output_origin_x: 500,
            output_origin_y: 600,
        });
        handler.on_surface_deleted(7);
        {
            let mut shared = handler.shared.lock().unwrap();
            shared.pending_bitmap.clear();
        }
        handler.on_bitmap_updated(&BitmapUpdate {
            surface_id: 7,
            destination_rectangle: ExclusiveRectangle {
                left: 10,
                top: 20,
                right: 12,
                bottom: 22,
            },
            codec_id: Codec1Type::Uncompressed,
            data: vec![0u8; 2 * 2 * 4],
            width: 2,
            height: 2,
        });
        let shared = handler.shared.lock().unwrap();
        assert_eq!(
            shared.pending_bitmap[0].left, 10,
            "deleted surface's origin must not leak"
        );
        assert_eq!(shared.pending_bitmap[0].top, 20);
    }

    #[test]
    fn gfx_capabilities_do_not_advertise_avc444() {
        let handler = NativeGfxHandler {
            shared: Arc::new(Mutex::new(NativeGfxState::default())),
        };

        let capabilities = handler.capabilities();
        assert_eq!(capabilities.len(), 2);
        assert_eq!(
            capabilities[0],
            CapabilitySet::V8_1 {
                flags: CapabilitiesV81Flags::AVC420_ENABLED | CapabilitiesV81Flags::SMALL_CACHE,
            }
        );
        assert_eq!(
            capabilities[1],
            CapabilitySet::V8 {
                flags: CapabilitiesV8Flags::SMALL_CACHE,
            }
        );
        assert!(!capabilities
            .iter()
            .any(|capability| matches!(capability, CapabilitySet::V10_7 { .. })));
        assert!(handler.wants_avc420_passthrough());
    }

    #[test]
    fn keyframe_detection_reads_length_prefixed_avc() {
        let mut au = Vec::new();
        au.extend_from_slice(&1u32.to_be_bytes());
        au.push(0x41); // non-IDR slice
        assert!(!avc_length_prefixed_has_keyframe(&au));

        au.extend_from_slice(&3u32.to_be_bytes());
        au.extend_from_slice(&[0x67, 0x42, 0x00]); // SPS
        assert!(avc_length_prefixed_has_keyframe(&au));
    }

    #[test]
    fn malformed_avc_is_not_keyframe() {
        assert!(!avc_length_prefixed_has_keyframe(&[0, 0, 0, 8, 0x65]));
        assert!(!avc_length_prefixed_has_keyframe(&[0, 0, 0, 0]));
    }

    #[test]
    fn pointer_button_values_match_c_abi() {
        let cases = [
            (1, PointerFlags::LEFT_BUTTON),
            (2, PointerFlags::RIGHT_BUTTON),
            (3, PointerFlags::MIDDLE_BUTTON_OR_WHEEL),
        ];

        for (button, expected_button_flag) in cases {
            let events = InputCommand::PointerButton {
                x: 10,
                y: 20,
                button,
                down: true,
            }
            .into_events();
            assert_eq!(events.len(), 1);
            match &events[0] {
                FastPathInputEvent::MouseEvent(pdu) => {
                    assert_eq!(pdu.flags, expected_button_flag | PointerFlags::DOWN);
                    assert_eq!(pdu.number_of_wheel_rotation_units, 0);
                    assert_eq!(pdu.x_position, 10);
                    assert_eq!(pdu.y_position, 20);
                }
                other => panic!("expected mouse event for button {button}, got {other:?}"),
            }
        }
    }

    #[test]
    fn sync_locks_command_encodes_toggle_flags() {
        let events = InputCommand::SyncLocks {
            flags: (SynchronizeFlags::NUM_LOCK | SynchronizeFlags::CAPS_LOCK).bits(),
        }
        .into_events();
        assert_eq!(events.len(), 1);
        match &events[0] {
            FastPathInputEvent::SyncEvent(flags) => {
                assert_eq!(
                    *flags,
                    SynchronizeFlags::NUM_LOCK | SynchronizeFlags::CAPS_LOCK
                );
            }
            other => panic!("expected sync event, got {other:?}"),
        }
    }

    #[test]
    fn null_config_reports_protocol_error() {
        extern "C" fn on_state(
            ctx: *mut core::ffi::c_void,
            state: RdpState,
            _detail: *const c_char,
        ) {
            let states = unsafe { &*(ctx.cast::<Mutex<Vec<RdpState>>>()) };
            states.lock().unwrap().push(state);
        }

        let states = Mutex::new(Vec::new());
        let callbacks = RdpCallbacks {
            ctx: (&states as *const Mutex<Vec<RdpState>>).cast_mut().cast(),
            on_state: Some(on_state),
            on_log: None,
            on_desktop_size: None,
            on_video_au: None,
            on_bitmap_update: None,
            on_audio_format: None,
            on_audio_data: None,
            on_pointer_bitmap: None,
            on_pointer_position: None,
            on_pointer_state: None,
        };
        let session = unsafe { rdp_session_start(ptr::null(), &callbacks) };
        assert!(session.is_null());
        assert_eq!(*states.lock().unwrap(), vec![RdpState::ProtocolError]);
    }

    // ---- rdpsnd-over-DVC state machine ----

    /// Captures on_audio_format/on_audio_data invocations behind the C ABI ctx pointer.
    #[derive(Default)]
    struct AudioCapture {
        formats: Vec<(u32, u32, u16)>,
        data: Vec<(Vec<u8>, u32)>,
    }

    extern "C" fn capture_audio_format(
        ctx: *mut core::ffi::c_void,
        codec: u32,
        sample_rate: u32,
        channels: u16,
    ) {
        let capture = unsafe { &*(ctx.cast::<Mutex<AudioCapture>>()) };
        capture
            .lock()
            .unwrap()
            .formats
            .push((codec, sample_rate, channels));
    }

    extern "C" fn capture_audio_data(
        ctx: *mut core::ffi::c_void,
        data: *const u8,
        len: usize,
        ts_ms: u32,
    ) {
        let capture = unsafe { &*(ctx.cast::<Mutex<AudioCapture>>()) };
        let bytes = unsafe { core::slice::from_raw_parts(data, len) }.to_vec();
        capture.lock().unwrap().data.push((bytes, ts_ms));
    }

    fn audio_test_handler(capture: &Mutex<AudioCapture>) -> RdpsndDvcHandler {
        let callbacks = RdpCallbacks {
            ctx: (capture as *const Mutex<AudioCapture>).cast_mut().cast(),
            on_state: None,
            on_log: None,
            on_desktop_size: None,
            on_video_au: None,
            on_bitmap_update: None,
            on_audio_format: Some(capture_audio_format),
            on_audio_data: Some(capture_audio_data),
            on_pointer_bitmap: None,
            on_pointer_position: None,
            on_pointer_state: None,
        };
        RdpsndDvcHandler::new(CallbackSink::new(callbacks), false)
    }

    /// The three formats gnome-remote-desktop advertises, in its order.
    fn grd_server_formats() -> Vec<sndpdu::AudioFormat> {
        vec![
            sndpdu::AudioFormat {
                format: sndpdu::WaveFormat::AAC_MS,
                n_channels: 2,
                n_samples_per_sec: 44_100,
                n_avg_bytes_per_sec: 12_000,
                n_block_align: 4,
                bits_per_sample: 16,
                data: None,
            },
            sndpdu::AudioFormat {
                format: sndpdu::WaveFormat::OPUS,
                n_channels: 2,
                n_samples_per_sec: 48_000,
                n_avg_bytes_per_sec: 12_000,
                n_block_align: 4,
                bits_per_sample: 16,
                data: None,
            },
            sndpdu::AudioFormat {
                format: sndpdu::WaveFormat::PCM,
                n_channels: 2,
                n_samples_per_sec: 44_100,
                n_avg_bytes_per_sec: 176_400,
                n_block_align: 4,
                bits_per_sample: 16,
                data: None,
            },
        ]
    }

    fn encode_server_pdu(pdu: sndpdu::ServerAudioOutputPdu<'_>) -> Vec<u8> {
        ironrdp_core::encode_vec(&pdu).expect("encode server audio PDU")
    }

    fn decode_reply(message: &DvcMessage) -> sndpdu::ClientAudioOutputPdu {
        let bytes = ironrdp_core::encode_vec(message.as_ref()).expect("encode reply");
        let mut cursor = ReadCursor::new(&bytes);
        sndpdu::ClientAudioOutputPdu::decode(&mut cursor).expect("decode reply")
    }

    fn negotiate_and_train(handler: &mut RdpsndDvcHandler) {
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: grd_server_formats(),
            },
        ));
        handler.process(0, &payload).expect("negotiation");
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::Training(
            sndpdu::TrainingPdu {
                timestamp: 42,
                data: Vec::new(),
            },
        ));
        handler.process(0, &payload).expect("training");
    }

    #[test]
    fn rdpsnd_prefer_pcm_drops_opus_from_client_list() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);
        handler.prefer_pcm = true;

        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: grd_server_formats(),
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::AudioFormat(pdu) => {
                // Only the PCM entry survives, so grd cannot pick Opus.
                assert_eq!(pdu.formats, grd_server_formats()[2..].to_vec());
            }
            other => panic!("expected AudioFormat reply, got {other:?}"),
        }

        // A server with no PCM keeps the playable list instead of going silent.
        let mut handler = audio_test_handler(&capture);
        handler.prefer_pcm = true;
        let opus_only = vec![grd_server_formats()[1].clone()];
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: opus_only.clone(),
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::AudioFormat(pdu) => {
                assert_eq!(pdu.formats, opus_only);
            }
            other => panic!("expected AudioFormat reply, got {other:?}"),
        }
    }

    #[test]
    fn rdpsnd_negotiation_filters_grd_formats() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);

        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: grd_server_formats(),
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        assert_eq!(replies.len(), 2, "expected ClientAudioFormat + QualityMode");

        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::AudioFormat(pdu) => {
                // AAC must be filtered out (the TV cannot play it and the server would
                // otherwise prefer it); the OPUS and PCM entries are echoed verbatim —
                // Opus decodes in-process for the mixer, PCM is the fallback.
                assert_eq!(pdu.formats, grd_server_formats()[1..].to_vec());
                assert_eq!(pdu.version, sndpdu::Version::V8);
                assert!(pdu.flags.contains(sndpdu::AudioFormatFlags::ALIVE));
                assert_eq!(pdu.volume_left, 0xFFFF);
                assert_eq!(pdu.volume_right, 0xFFFF);
                assert_eq!(pdu.pitch, 0x0001_0000);
                assert_eq!(pdu.dgram_port, 0);
            }
            other => panic!("expected AudioFormat reply, got {other:?}"),
        }
        match decode_reply(&replies[1]) {
            sndpdu::ClientAudioOutputPdu::QualityMode(pdu) => {
                assert_eq!(pdu.quality_mode, sndpdu::QualityMode::High);
            }
            other => panic!("expected QualityMode reply, got {other:?}"),
        }
        // Negotiation alone must not touch the C callbacks.
        assert!(capture.lock().unwrap().formats.is_empty());
    }

    #[test]
    fn rdpsnd_negotiation_falls_back_to_opus_without_pcm() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);

        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::AudioFormat(
            sndpdu::ServerAudioFormatPdu {
                version: sndpdu::Version::V8,
                formats: grd_server_formats()[..2].to_vec(), // AAC + OPUS, no PCM
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::AudioFormat(pdu) => {
                assert_eq!(pdu.formats, grd_server_formats()[1..2].to_vec());
            }
            other => panic!("expected AudioFormat reply, got {other:?}"),
        }
    }

    #[test]
    fn rdpsnd_training_confirm_echoes_timestamp() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);

        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::Training(
            sndpdu::TrainingPdu {
                timestamp: 0x1234,
                data: Vec::new(),
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        assert_eq!(replies.len(), 1);
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::TrainingConfirm(pdu) => {
                assert_eq!(pdu.timestamp, 0x1234);
                assert_eq!(pdu.pack_size, 0);
            }
            other => panic!("expected TrainingConfirm reply, got {other:?}"),
        }
    }

    #[test]
    fn rdpsnd_training_confirm_restores_wire_pack_size() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);

        // gnome-remote-desktop sends Training with wPackSize=1024 (and validates that the
        // confirm echoes exactly 1024, ignoring it otherwise until a 10s protocol
        // timeout). IronRDP's decoder subtracts the 8 header bytes when sizing `data`,
        // so the confirm must add them back.
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::Training(
            sndpdu::TrainingPdu {
                timestamp: 0,
                data: vec![0u8; 1016],
            },
        ));
        let replies = handler.process(0, &payload).expect("process");
        assert_eq!(replies.len(), 1);
        match decode_reply(&replies[0]) {
            sndpdu::ClientAudioOutputPdu::TrainingConfirm(pdu) => {
                assert_eq!(pdu.timestamp, 0);
                assert_eq!(pdu.pack_size, 1024);
            }
            other => panic!("expected TrainingConfirm reply, got {other:?}"),
        }
    }

    #[test]
    fn rdpsnd_wave2_fires_format_once_then_data_and_confirms() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);
        negotiate_and_train(&mut handler);

        // format_no indexes the CLIENT's filtered list: 0 = OPUS 48k (AAC was dropped).
        for (block_no, payload_byte) in [(7u8, 0xAAu8), (8u8, 0xBBu8)] {
            let payload =
                encode_server_pdu(sndpdu::ServerAudioOutputPdu::Wave2(sndpdu::Wave2Pdu {
                    timestamp: 100 + u16::from(block_no),
                    format_no: 0,
                    block_no,
                    audio_timestamp: 5000 + u32::from(block_no),
                    data: vec![payload_byte; 16].into(),
                }));
            let replies = handler.process(0, &payload).expect("process");
            assert_eq!(replies.len(), 1);
            match decode_reply(&replies[0]) {
                sndpdu::ClientAudioOutputPdu::WaveConfirm(pdu) => {
                    assert_eq!(pdu.timestamp, 100 + u16::from(block_no));
                    assert_eq!(pdu.block_no, block_no);
                }
                other => panic!("expected WaveConfirm reply, got {other:?}"),
            }
        }

        let capture = capture.lock().unwrap();
        assert_eq!(
            capture.formats,
            vec![(RDP_AUDIO_CODEC_OPUS, 48_000, 2)],
            "on_audio_format must fire exactly once for an unchanged format"
        );
        assert_eq!(capture.data.len(), 2);
        assert_eq!(capture.data[0], (vec![0xAA; 16], 5007));
        assert_eq!(capture.data[1], (vec![0xBB; 16], 5008));
    }

    #[test]
    fn rdpsnd_malformed_payload_is_not_fatal() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);
        negotiate_and_train(&mut handler);

        // A decode failure must silence the channel, never error the session.
        let replies = handler
            .process(0, &[0xFF, 0x00, 0x02, 0x00, 0x99])
            .expect("must not error");
        assert!(replies.is_empty());

        // Latched off: even a valid wave afterward is ignored.
        let payload = encode_server_pdu(sndpdu::ServerAudioOutputPdu::Wave2(sndpdu::Wave2Pdu {
            timestamp: 1,
            format_no: 0,
            block_no: 1,
            audio_timestamp: 1,
            data: vec![0u8; 4].into(),
        }));
        let replies = handler.process(0, &payload).expect("must not error");
        assert!(replies.is_empty());
        assert!(capture.lock().unwrap().data.is_empty());
    }

    #[test]
    fn rdpsnd_renegotiation_refires_format() {
        let capture = Mutex::new(AudioCapture::default());
        let mut handler = audio_test_handler(&capture);
        negotiate_and_train(&mut handler);

        let wave = |block_no: u8| {
            encode_server_pdu(sndpdu::ServerAudioOutputPdu::Wave2(sndpdu::Wave2Pdu {
                timestamp: u16::from(block_no),
                format_no: 0,
                block_no,
                audio_timestamp: u32::from(block_no),
                data: vec![0u8; 4].into(),
            }))
        };
        handler.process(0, &wave(1)).expect("wave");
        negotiate_and_train(&mut handler);
        handler
            .process(0, &wave(2))
            .expect("wave after renegotiation");

        let capture = capture.lock().unwrap();
        assert_eq!(
            capture.formats,
            vec![
                (RDP_AUDIO_CODEC_OPUS, 48_000, 2),
                (RDP_AUDIO_CODEC_OPUS, 48_000, 2)
            ],
            "renegotiation must re-fire on_audio_format even for the same format"
        );
        assert_eq!(capture.data.len(), 2);
    }

    // ---- Display Control DVC ----

    use ironrdp_displaycontrol::pdu::DisplayControlCapabilities;

    /// Encodes the full `DISPLAYCONTROL_CAPS_PDU` as a real server puts it on the wire:
    /// the `DISPLAYCONTROL_HEADER` (Type + Length) followed by the capability set
    /// (MS-RDPEDISP 2.2.2.1). Feeding the raw capability body without the header would
    /// exercise a wire shape no server ever sends.
    fn display_caps_payload(max_num_monitors: u32, factor_a: u32, factor_b: u32) -> Vec<u8> {
        let caps: DisplayControlPdu =
            DisplayControlCapabilities::new(max_num_monitors, factor_a, factor_b)
                .expect("caps")
                .into();
        ironrdp_core::encode_vec(&caps).expect("encode caps")
    }

    fn decode_monitor_layout(message: &DvcMessage) -> DisplayControlMonitorLayout {
        let bytes = ironrdp_core::encode_vec(message.as_ref()).expect("encode layout message");
        let mut cursor = ReadCursor::new(&bytes);
        match DisplayControlPdu::decode(&mut cursor).expect("decode DisplayControlPdu") {
            DisplayControlPdu::MonitorLayout(layout) => layout,
            other => panic!("expected MonitorLayout, got {other:?}"),
        }
    }

    #[test]
    fn display_control_pushes_configured_resolution_on_caps() {
        let payload = display_caps_payload(1, 1920, 1080);
        // Golden wire bytes: DISPLAYCONTROL_HEADER (Type=0x05 Caps, Length=0x14) + body.
        // Matches the vendored IronRDP testsuite vector shape and MS-RDPEDISP 2.2.2.1.
        assert_eq!(
            payload,
            [
                0x05, 0x00, 0x00, 0x00, // Header: Type = DISPLAYCONTROL_PDU_TYPE_CAPS
                0x14, 0x00, 0x00, 0x00, // Header: Length = 20
                0x01, 0x00, 0x00, 0x00, // MaxNumMonitors = 1
                0x80, 0x07, 0x00, 0x00, // MaxMonitorAreaFactorA = 1920
                0x38, 0x04, 0x00, 0x00, // MaxMonitorAreaFactorB = 1080
            ]
        );
        let mut client = make_display_control(1920, 1080, CallbackSink::empty());
        let replies = client.process(0, &payload).expect("process caps");
        assert_eq!(replies.len(), 1);
        let layout = decode_monitor_layout(&replies[0]);
        assert_eq!(layout.monitors().len(), 1);
        let monitor = &layout.monitors()[0];
        assert!(monitor.is_primary());
        assert_eq!(monitor.dimensions(), (1920, 1080));
        assert_eq!(monitor.position(), Some((0, 0)));
    }

    #[test]
    fn display_control_skips_layout_exceeding_server_area() {
        let mut client = make_display_control(1920, 1080, CallbackSink::empty());
        // A 640x480 max monitor area cannot fit 1920x1080; must stay silent, never error.
        let replies = client
            .process(0, &display_caps_payload(1, 640, 480))
            .expect("process caps");
        assert!(replies.is_empty());
    }

    #[test]
    fn display_control_evens_odd_width() {
        let mut client = make_display_control(1367, 768, CallbackSink::empty());
        let replies = client
            .process(0, &display_caps_payload(1, 8192, 8192))
            .expect("process caps");
        assert_eq!(replies.len(), 1);
        let layout = decode_monitor_layout(&replies[0]);
        assert_eq!(layout.monitors()[0].dimensions(), (1366, 768));
    }
}
