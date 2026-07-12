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

#[cfg(feature = "native")]
pub(crate) fn init_logging() {
    use std::sync::Once;
    use tracing_subscriber::prelude::*;

    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let subscriber = tracing_subscriber::registry().with(native::CallbackLogLayer);
        let _ = tracing::subscriber::set_global_default(subscriber);
    });
}
