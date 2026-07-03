//! Tiny pure-Rust crypto primitives for NTLMv2 + CredSSP.
//! Uses RustCrypto digest 0.10 generation plus hand-rolled RC4 matching MS-NLMP.

/// MD4 (NT hash of the UTF-16LE password).
pub fn md4(data: &[u8]) -> [u8; 16] {
    use md4::{Digest, Md4};
    let mut h = Md4::new();
    h.update(data);
    let d = h.finalize();
    let mut o = [0u8; 16];
    o.copy_from_slice(&d);
    o
}

pub fn md5(data: &[u8]) -> [u8; 16] {
    use md5::{Digest, Md5};
    let mut h = Md5::new();
    h.update(data);
    let d = h.finalize();
    let mut o = [0u8; 16];
    o.copy_from_slice(&d);
    o
}

/// HMAC-MD5 (RFC 2104) — the NTLMv2 workhorse.
pub fn hmac_md5(key: &[u8], data: &[u8]) -> [u8; 16] {
    use hmac::{Hmac, Mac};
    use md5::Md5;
    let mut m = <Hmac<Md5>>::new_from_slice(key).expect("HMAC accepts any key length");
    m.update(data);
    let d = m.finalize().into_bytes();
    let mut o = [0u8; 16];
    o.copy_from_slice(&d);
    o
}

/// SHA-256 (CredSSP v5+ public-key binding hash).
pub fn sha256(data: &[u8]) -> [u8; 32] {
    use sha2::{Digest, Sha256};
    let mut h = Sha256::new();
    h.update(data);
    let d = h.finalize();
    let mut o = [0u8; 32];
    o.copy_from_slice(&d);
    o
}

/// RC4 stream cipher (KSA + PRGA), matching MS-NLMP sealing semantics: a single
/// handle whose keystream advances across successive `process()` calls.
#[derive(Clone)]
pub struct Rc4 {
    i: usize,
    j: usize,
    s: [u8; 256],
}

impl Rc4 {
    pub fn new(key: &[u8]) -> Self {
        let mut s = [0u8; 256];
        for (i, b) in s.iter_mut().enumerate() {
            *b = i as u8;
        }
        let mut j = 0usize;
        for i in 0..256 {
            j = (j + s[i] as usize + key[i % key.len()] as usize) % 256;
            s.swap(i, j);
        }
        Rc4 { i: 0, j: 0, s }
    }

    pub fn process(&mut self, data: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(data.len());
        for &byte in data {
            self.i = (self.i + 1) % 256;
            self.j = (self.j + self.s[self.i] as usize) % 256;
            self.s.swap(self.i, self.j);
            let k = self.s[(self.s[self.i] as usize + self.s[self.j] as usize) % 256];
            out.push(k ^ byte);
        }
        out
    }
}

/// UTF-16LE encoding of a `str` (NTLM uses UTF-16LE throughout).
pub fn utf16le(s: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(s.len() * 2);
    for u in s.encode_utf16() {
        out.extend_from_slice(&u.to_le_bytes());
    }
    out
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn md4_empty() {
        // RFC 1320 test vector: MD4("") = 31d6cfe0d16ae931b73c59d7e0c089c0
        assert_eq!(
            md4(b""),
            [
                0x31, 0xd6, 0xcf, 0xe0, 0xd1, 0x6a, 0xe9, 0x31, 0xb7, 0x3c, 0x59, 0xd7, 0xe0, 0xc0,
                0x89, 0xc0
            ]
        );
    }

    #[test]
    fn md5_abc() {
        // RFC 1321: MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
        assert_eq!(
            md5(b"abc"),
            [
                0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1,
                0x7f, 0x72
            ]
        );
    }

    #[test]
    fn hmac_md5_rfc2104() {
        // RFC 2104 example: key=0x0b*16, data="Hi There"
        let key = [0x0bu8; 16];
        assert_eq!(
            hmac_md5(&key, b"Hi There"),
            [
                0x92, 0x94, 0x72, 0x7a, 0x36, 0x38, 0xbb, 0x1c, 0x13, 0xf4, 0x8e, 0xf8, 0x15, 0x8b,
                0xfc, 0x9d
            ]
        );
    }

    #[test]
    fn sha256_abc() {
        // SHA-256("abc")
        assert_eq!(
            sha256(b"abc"),
            [
                0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae,
                0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61,
                0xf2, 0x00, 0x15, 0xad
            ]
        );
    }

    #[test]
    fn rc4_known_vector() {
        // RC4 key="Key", data="Plaintext" -> BBF316E8D940AF0AD3
        let out = Rc4::new(b"Key").process(b"Plaintext");
        assert_eq!(out, [0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3]);
    }

    #[test]
    fn utf16le_basic() {
        assert_eq!(utf16le("AB"), [0x41, 0x00, 0x42, 0x00]);
    }
}
