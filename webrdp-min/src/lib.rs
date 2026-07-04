//! Native Rust RDP core for the gnomecast webOS app.
//!
//! The crate exposes a C ABI static library for the native C shell. It keeps the
//! direct TCP/TLS/CredSSP transport, AVC420/H.264 passthrough for ss4s, and native
//! RemoteFX/bitmap output. Historical WASM/RDCleanPath/browser exports were removed.

mod credssp;
mod crypto;
mod der;
#[cfg(feature = "native")]
pub mod native;
mod ntlm;

pub(crate) fn init_logging() {
    use std::sync::Once;
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        // stderr is what the webOS shell redirects into the on-TV log file. Protocol
        // debug traces (per-PDU, per-RTT-ping) are too chatty for normal operation;
        // opt in with WEBRDP_LOG=debug when triaging.
        let level = match std::env::var("WEBRDP_LOG").as_deref() {
            Ok("debug") => tracing::Level::DEBUG,
            Ok("trace") => tracing::Level::TRACE,
            _ => tracing::Level::INFO,
        };
        let _ = tracing_subscriber::fmt()
            .with_max_level(level)
            .without_time()
            .with_ansi(false)
            .with_writer(std::io::stderr)
            .try_init();
    });
}
