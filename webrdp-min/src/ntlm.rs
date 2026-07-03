//! Minimal NTLMv2 client for CredSSP NLA (no `sspi`). Ported from the MS-NLMP
//! behavior of the `sspi` crate, which authenticates to this GNOME server.
//!
//! Supports: NEGOTIATE, CHALLENGE parse, AUTHENTICATE (NTLMv2 + MIC), and NTLM2
//! session security (sign + seal) used by CredSSP `pubKeyAuth`/`authInfo`.

use crate::crypto::{hmac_md5, md4, md5, utf16le, Rc4};

// --- NEGOTIATE flags (MS-NLMP 2.2.2.5) ---
const UNICODE: u32 = 0x0000_0001;
const OEM: u32 = 0x0000_0002;
const REQUEST_TARGET: u32 = 0x0000_0004;
const SIGN: u32 = 0x0000_0010;
const SEAL: u32 = 0x0000_0020;
const LM_KEY: u32 = 0x0000_0080;
const NTLM: u32 = 0x0000_0200;
const DOMAIN_SUPPLIED: u32 = 0x0000_1000;
const ALWAYS_SIGN: u32 = 0x0000_8000;
const EXTENDED_SESSION_SECURITY: u32 = 0x0008_0000;
const TARGET_INFO: u32 = 0x0080_0000;
const VERSION: u32 = 0x0200_0000;
const NEGOTIATE_128: u32 = 0x2000_0000;
const KEY_EXCH: u32 = 0x4000_0000;
const NEGOTIATE_56: u32 = 0x8000_0000;

const NTLM_VERSION: [u8; 8] = [0x0a, 0x00, 0x63, 0x45, 0x00, 0x00, 0x00, 0x0f];

const CLIENT_SIGN_MAGIC: &[u8] = b"session key to client-to-server signing key magic constant\0";
const SERVER_SIGN_MAGIC: &[u8] = b"session key to server-to-client signing key magic constant\0";
const CLIENT_SEAL_MAGIC: &[u8] = b"session key to client-to-server sealing key magic constant\0";
const SERVER_SEAL_MAGIC: &[u8] = b"session key to server-to-client sealing key magic constant\0";

/// NTLMv2 hash: HMAC-MD5(MD4(UTF16LE(password)), UTF16LE(UPPER(user) + domain)).
pub fn ntlmv2_hash(user: &str, domain: &str, password: &str) -> [u8; 16] {
    let nt = md4(&utf16le(password));
    let mut ud = utf16le(&user.to_uppercase());
    ud.extend_from_slice(&utf16le(domain));
    hmac_md5(&nt, &ud)
}

fn field(len: u16, offset: u32) -> [u8; 8] {
    let mut f = [0u8; 8];
    f[0..2].copy_from_slice(&len.to_le_bytes()); // Len
    f[2..4].copy_from_slice(&len.to_le_bytes()); // MaxLen
    f[4..8].copy_from_slice(&offset.to_le_bytes()); // BufferOffset
    f
}

fn md5_with(key: &[u8], magic: &[u8]) -> [u8; 16] {
    let mut v = key.to_vec();
    v.extend_from_slice(magic);
    md5(&v)
}

fn timestamp_from_target_info(ti: &[u8]) -> Option<u64> {
    let mut pos = 0;
    while pos + 4 <= ti.len() {
        let t = u16::from_le_bytes([ti[pos], ti[pos + 1]]);
        let l = u16::from_le_bytes([ti[pos + 2], ti[pos + 3]]) as usize;
        pos += 4;
        if t == 0 {
            break; // EOL
        }
        if pos + l > ti.len() {
            break;
        }
        if t == 7 && l == 8 {
            let mut b = [0u8; 8];
            b.copy_from_slice(&ti[pos..pos + 8]);
            return Some(u64::from_le_bytes(b));
        }
        pos += l;
    }
    None
}

fn current_filetime_timestamp() -> u64 {
    const WINDOWS_UNIX_EPOCH_SECONDS: u64 = 11_644_473_600;
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(duration) => duration
            .as_secs()
            .saturating_add(WINDOWS_UNIX_EPOCH_SECONDS)
            .saturating_mul(10_000_000)
            .saturating_add(u64::from(duration.subsec_nanos()) / 100),
        Err(_) => 0,
    }
}

/// Copy the CHALLENGE target info (dropping EOL), append the MIC Flags AV pair,
/// then EOL + 4 reserved bytes — exactly as MS-NLMP requires for NTLMv2.
fn build_authenticate_target_info(challenge_ti: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(challenge_ti.len() + 16);
    let mut pos = 0;
    while pos + 4 <= challenge_ti.len() {
        let t = u16::from_le_bytes([challenge_ti[pos], challenge_ti[pos + 1]]);
        let l = u16::from_le_bytes([challenge_ti[pos + 2], challenge_ti[pos + 3]]) as usize;
        if t == 0 {
            break; // EOL: stop, do not copy
        }
        if pos + 4 + l > challenge_ti.len() {
            break;
        }
        out.extend_from_slice(&challenge_ti[pos..pos + 4 + l]);
        pos += 4 + l;
    }
    // MsvAvFlags (type 6, len 4) = MESSAGE_INTEGRITY_CHECK (0x02)
    out.extend_from_slice(&6u16.to_le_bytes());
    out.extend_from_slice(&4u16.to_le_bytes());
    out.extend_from_slice(&2u32.to_le_bytes());
    // EOL (4 bytes) + reserved (4 bytes)
    out.extend_from_slice(&[0u8; 8]);
    out
}

/// NTLMv2 client state machine + session security.
pub struct NtlmClient {
    negotiate_msg: Vec<u8>,
    challenge_msg: Vec<u8>,
    server_challenge: [u8; 8],
    challenge_target_info: Vec<u8>,
    challenge_flags: u32,
    timestamp: u64,
    pub exported_session_key: [u8; 16],
    send_signing_key: [u8; 16],
    recv_signing_key: [u8; 16],
    send_sealing: Option<Rc4>,
    recv_sealing: Option<Rc4>,
    send_seq: u32,
    recv_seq: u32,
}

impl Default for NtlmClient {
    fn default() -> Self {
        Self::new()
    }
}

impl NtlmClient {
    pub fn new() -> Self {
        NtlmClient {
            negotiate_msg: Vec::new(),
            challenge_msg: Vec::new(),
            server_challenge: [0; 8],
            challenge_target_info: Vec::new(),
            challenge_flags: 0,
            timestamp: 0,
            exported_session_key: [0; 16],
            send_signing_key: [0; 16],
            recv_signing_key: [0; 16],
            send_sealing: None,
            recv_sealing: None,
            send_seq: 0,
            recv_seq: 0,
        }
    }

    /// NTLM NEGOTIATE (type 1). Stores it for the later MIC.
    pub fn build_negotiate(&mut self) -> Vec<u8> {
        let flags = NEGOTIATE_56
            | OEM
            | NEGOTIATE_128
            | ALWAYS_SIGN
            | EXTENDED_SESSION_SECURITY
            | NTLM
            | REQUEST_TARGET
            | UNICODE
            | VERSION
            | LM_KEY
            | SEAL
            | KEY_EXCH
            | SIGN;
        let mut msg = Vec::with_capacity(40);
        msg.extend_from_slice(b"NTLMSSP\0");
        msg.extend_from_slice(&1u32.to_le_bytes());
        msg.extend_from_slice(&flags.to_le_bytes());
        msg.extend_from_slice(&field(0, 40)); // DomainName (empty, offset = header+version)
        msg.extend_from_slice(&field(0, 40)); // Workstation (empty)
        msg.extend_from_slice(&NTLM_VERSION);
        self.negotiate_msg = msg.clone();
        msg
    }

    /// Parse the NTLM CHALLENGE (type 2): server challenge, target info, timestamp, flags.
    pub fn read_challenge(&mut self, msg: &[u8]) -> Result<(), String> {
        if msg.len() < 48 {
            return Err("NTLM challenge too short".into());
        }
        if &msg[0..8] != b"NTLMSSP\0" {
            return Err("bad NTLM signature".into());
        }
        if u32::from_le_bytes([msg[8], msg[9], msg[10], msg[11]]) != 2 {
            return Err("not an NTLM CHALLENGE".into());
        }
        let flags = u32::from_le_bytes([msg[20], msg[21], msg[22], msg[23]]);
        let mut sc = [0u8; 8];
        sc.copy_from_slice(&msg[24..32]);
        let ti_len = u16::from_le_bytes([msg[40], msg[41]]) as usize;
        let ti_off = u32::from_le_bytes([msg[44], msg[45], msg[46], msg[47]]) as usize;
        if ti_off.checked_add(ti_len).map_or(true, |e| e > msg.len()) {
            return Err("NTLM challenge target info out of bounds".into());
        }
        let ti = msg[ti_off..ti_off + ti_len].to_vec();
        self.timestamp = timestamp_from_target_info(&ti).unwrap_or_else(current_filetime_timestamp);
        self.challenge_target_info = ti;
        self.server_challenge = sc;
        self.challenge_flags = flags;
        self.challenge_msg = msg.to_vec();
        Ok(())
    }

    /// Build the NTLM AUTHENTICATE (type 3) and derive session-security keys.
    /// `client_challenge` (8 bytes) and `key_exch_session_key` (16 bytes) must be
    /// fresh random; callers pass them so tests can pin them.
    pub fn build_authenticate(
        &mut self,
        user: &str,
        domain: &str,
        password: &str,
        client_challenge: [u8; 8],
        key_exch_session_key: [u8; 16],
    ) -> Result<Vec<u8>, String> {
        if self.challenge_msg.is_empty() {
            return Err("read_challenge must be called first".into());
        }
        let nt_hash = ntlmv2_hash(user, domain, password);
        let auth_ti = build_authenticate_target_info(&self.challenge_target_info);

        // LM challenge response (24 bytes)
        let mut lm_input = self.server_challenge.to_vec();
        lm_input.extend_from_slice(&client_challenge);
        let mut lm_response = Vec::with_capacity(24);
        lm_response.extend_from_slice(&hmac_md5(&nt_hash, &lm_input));
        lm_response.extend_from_slice(&client_challenge);

        // NTLMv2 response: NTProofStr(16) + temp
        let mut temp = Vec::with_capacity(28 + auth_ti.len());
        temp.push(1); // RespType
        temp.push(1); // HiRespType
        temp.extend_from_slice(&[0u8; 2]); // Reserved1
        temp.extend_from_slice(&[0u8; 4]); // Reserved2
        temp.extend_from_slice(&self.timestamp.to_le_bytes());
        temp.extend_from_slice(&client_challenge);
        temp.extend_from_slice(&[0u8; 4]); // Reserved3
        temp.extend_from_slice(&auth_ti);
        let mut nt_proof_input = self.server_challenge.to_vec();
        nt_proof_input.extend_from_slice(&temp);
        let nt_proof = hmac_md5(&nt_hash, &nt_proof_input);
        let mut nt_response = Vec::with_capacity(16 + temp.len());
        nt_response.extend_from_slice(&nt_proof);
        nt_response.extend_from_slice(&temp);
        let key_exchange_key = hmac_md5(&nt_hash, &nt_proof);

        let mut flags = NEGOTIATE_56
            | NEGOTIATE_128
            | ALWAYS_SIGN
            | EXTENDED_SESSION_SECURITY
            | NTLM
            | REQUEST_TARGET
            | UNICODE
            | TARGET_INFO
            | VERSION
            | SEAL
            | SIGN;
        if self.challenge_flags & KEY_EXCH != 0 {
            flags |= KEY_EXCH;
        }
        if !domain.is_empty() {
            flags |= DOMAIN_SUPPLIED;
        }

        let session_key = if flags & KEY_EXCH != 0 {
            key_exch_session_key
        } else {
            key_exchange_key
        };
        let mut encrypted_session_key = [0u8; 16];
        encrypted_session_key.copy_from_slice(&Rc4::new(&key_exchange_key).process(&session_key));

        let domain_b = utf16le(domain);
        let user_b = utf16le(user);
        let base = 88u32; // 64 header + 8 version + 16 MIC
        let domain_off = base;
        let user_off = domain_off + domain_b.len() as u32;
        let ws_off = user_off + user_b.len() as u32;
        let lm_off = ws_off; // workstation empty
        let nt_off = lm_off + lm_response.len() as u32;
        let esk_off = nt_off + nt_response.len() as u32;
        let esk_len = if flags & KEY_EXCH != 0 { 16u16 } else { 0 };

        let mut msg = Vec::new();
        msg.extend_from_slice(b"NTLMSSP\0");
        msg.extend_from_slice(&3u32.to_le_bytes());
        msg.extend_from_slice(&field(lm_response.len() as u16, lm_off));
        msg.extend_from_slice(&field(nt_response.len() as u16, nt_off));
        msg.extend_from_slice(&field(domain_b.len() as u16, domain_off));
        msg.extend_from_slice(&field(user_b.len() as u16, user_off));
        msg.extend_from_slice(&field(0, ws_off));
        msg.extend_from_slice(&field(esk_len, esk_off));
        msg.extend_from_slice(&flags.to_le_bytes());
        msg.extend_from_slice(&NTLM_VERSION);
        let mic_offset = msg.len(); // 72
        msg.extend_from_slice(&[0u8; 16]); // MIC placeholder
        msg.extend_from_slice(&domain_b);
        msg.extend_from_slice(&user_b);
        // workstation empty
        msg.extend_from_slice(&lm_response);
        msg.extend_from_slice(&nt_response);
        if flags & KEY_EXCH != 0 {
            msg.extend_from_slice(&encrypted_session_key);
        }

        // MIC = HMAC-MD5(session_key, NEGOTIATE + CHALLENGE + AUTHENTICATE[mic=0])
        let mut mic_input = self.negotiate_msg.clone();
        mic_input.extend_from_slice(&self.challenge_msg);
        mic_input.extend_from_slice(&msg);
        let mic = hmac_md5(&session_key, &mic_input);
        msg[mic_offset..mic_offset + 16].copy_from_slice(&mic);

        // Session security keys + RC4 handles.
        self.exported_session_key = session_key;
        self.send_signing_key = md5_with(&session_key, CLIENT_SIGN_MAGIC);
        self.recv_signing_key = md5_with(&session_key, SERVER_SIGN_MAGIC);
        self.send_sealing = Some(Rc4::new(&md5_with(&session_key, CLIENT_SEAL_MAGIC)));
        self.recv_sealing = Some(Rc4::new(&md5_with(&session_key, SERVER_SEAL_MAGIC)));
        self.send_seq = 0;
        self.recv_seq = 0;

        Ok(msg)
    }

    /// NTLM2 EncryptMessage (sign + seal): signature(16) ++ sealed(plaintext).
    pub fn encrypt(&mut self, plaintext: &[u8]) -> Result<Vec<u8>, String> {
        let seq = self.send_seq;
        self.send_seq = self.send_seq.wrapping_add(1);
        let mut digest_input = seq.to_le_bytes().to_vec();
        digest_input.extend_from_slice(plaintext);
        let digest = hmac_md5(&self.send_signing_key, &digest_input);
        let sealing = self.send_sealing.as_mut().ok_or("not authenticated")?;
        let sealed_data = sealing.process(plaintext);
        let sealed_checksum = sealing.process(&digest[0..8]);
        let mut out = Vec::with_capacity(16 + sealed_data.len());
        out.extend_from_slice(&1u32.to_le_bytes()); // version
        out.extend_from_slice(&sealed_checksum); // checksum (8)
        out.extend_from_slice(&seq.to_le_bytes()); // seq (4)
        out.extend_from_slice(&sealed_data);
        Ok(out)
    }

    /// NTLM2 DecryptMessage: verify signature + unseal. Input is signature(16) ++ sealed.
    pub fn decrypt(&mut self, msg: &[u8]) -> Result<Vec<u8>, String> {
        if msg.len() < 16 {
            return Err("NTLM message too short".into());
        }
        let (sig, sealed) = msg.split_at(16);
        let seq = self.recv_seq;
        self.recv_seq = self.recv_seq.wrapping_add(1);
        let sealing = self.recv_sealing.as_mut().ok_or("not authenticated")?;
        let data = sealing.process(sealed);
        let mut digest_input = seq.to_le_bytes().to_vec();
        digest_input.extend_from_slice(&data);
        let digest = hmac_md5(&self.recv_signing_key, &digest_input);
        let sealing = self.recv_sealing.as_mut().ok_or("not authenticated")?;
        let checksum = sealing.process(&digest[0..8]);
        let mut expected = Vec::with_capacity(16);
        expected.extend_from_slice(&1u32.to_le_bytes());
        expected.extend_from_slice(&checksum);
        expected.extend_from_slice(&seq.to_le_bytes());
        if sig != expected.as_slice() {
            return Err("NTLM signature verification failed".into());
        }
        Ok(data)
    }

    /// Test helper: set this client's recv keys to a peer's send keys (derived from
    /// the shared exported session key) so it can decrypt the peer's sealed messages.
    #[cfg(test)]
    pub(crate) fn set_test_recv_keys(&mut self, session_key: &[u8]) {
        self.recv_signing_key = md5_with(session_key, CLIENT_SIGN_MAGIC);
        self.recv_sealing = Some(Rc4::new(&md5_with(session_key, CLIENT_SEAL_MAGIC)));
        self.recv_seq = 0;
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn ntlmv2_hash_msnlmp_vector() {
        // MS-NLMP 4.2.4.1.1: User/Domain/Password
        assert_eq!(
            ntlmv2_hash("User", "Domain", "Password"),
            [
                0x0c, 0x86, 0x8a, 0x40, 0x3b, 0xfd, 0x7a, 0x93, 0xa3, 0x00, 0x1e, 0xf2, 0x2e, 0xf0,
                0x2e, 0x3f
            ]
        );
    }

    #[test]
    fn negotiate_shape() {
        let mut c = NtlmClient::new();
        let m = c.build_negotiate();
        assert_eq!(m.len(), 40);
        assert_eq!(&m[0..8], b"NTLMSSP\0");
        assert_eq!(u32::from_le_bytes([m[8], m[9], m[10], m[11]]), 1);
    }

    fn sample_challenge() -> Vec<u8> {
        // Build a minimal CHALLENGE with a target info containing NbDomainName + Timestamp + EOL.
        let mut ti = Vec::new();
        // NbDomainName (type 2)
        let dom = utf16le("DOMAIN");
        ti.extend_from_slice(&2u16.to_le_bytes());
        ti.extend_from_slice(&(dom.len() as u16).to_le_bytes());
        ti.extend_from_slice(&dom);
        // Timestamp (type 7)
        ti.extend_from_slice(&7u16.to_le_bytes());
        ti.extend_from_slice(&8u16.to_le_bytes());
        ti.extend_from_slice(&0x01d8_0000_0000_0000u64.to_le_bytes());
        // EOL
        ti.extend_from_slice(&[0, 0, 0, 0]);

        let mut m = Vec::new();
        m.extend_from_slice(b"NTLMSSP\0");
        m.extend_from_slice(&2u32.to_le_bytes());
        m.extend_from_slice(&field(0, 56)); // target name (empty)
        let flags = NEGOTIATE_56
            | NEGOTIATE_128
            | KEY_EXCH
            | EXTENDED_SESSION_SECURITY
            | NTLM
            | UNICODE
            | TARGET_INFO
            | VERSION;
        m.extend_from_slice(&flags.to_le_bytes());
        m.extend_from_slice(&[0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]); // server challenge
        m.extend_from_slice(&[0u8; 8]); // reserved
        let ti_off = 56u32; // 48 header + 8 version
        m.extend_from_slice(&field(ti.len() as u16, ti_off));
        m.extend_from_slice(&NTLM_VERSION);
        m.extend_from_slice(&ti);
        m
    }

    fn sample_challenge_without_timestamp() -> Vec<u8> {
        let mut ti = Vec::new();
        let dom = utf16le("DOMAIN");
        ti.extend_from_slice(&2u16.to_le_bytes());
        ti.extend_from_slice(&(dom.len() as u16).to_le_bytes());
        ti.extend_from_slice(&dom);
        ti.extend_from_slice(&[0, 0, 0, 0]);

        let mut m = Vec::new();
        m.extend_from_slice(b"NTLMSSP\0");
        m.extend_from_slice(&2u32.to_le_bytes());
        m.extend_from_slice(&field(0, 56));
        let flags = NEGOTIATE_56
            | NEGOTIATE_128
            | KEY_EXCH
            | EXTENDED_SESSION_SECURITY
            | NTLM
            | UNICODE
            | TARGET_INFO
            | VERSION;
        m.extend_from_slice(&flags.to_le_bytes());
        m.extend_from_slice(&[0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]);
        m.extend_from_slice(&[0u8; 8]);
        let ti_off = 56u32;
        m.extend_from_slice(&field(ti.len() as u16, ti_off));
        m.extend_from_slice(&NTLM_VERSION);
        m.extend_from_slice(&ti);
        m
    }

    #[test]
    fn authenticate_roundtrip_fields() {
        let mut c = NtlmClient::new();
        c.build_negotiate();
        c.read_challenge(&sample_challenge()).unwrap();
        assert_eq!(
            c.server_challenge,
            [0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]
        );
        assert_eq!(c.timestamp, 0x01d8_0000_0000_0000);
        let auth = c
            .build_authenticate("user", "DOMAIN", "pass", [0xaa; 8], [0xbb; 16])
            .unwrap();
        assert_eq!(&auth[0..8], b"NTLMSSP\0");
        assert_eq!(
            u32::from_le_bytes([auth[8], auth[9], auth[10], auth[11]]),
            3
        );
        // NT response field non-empty, located within the message.
        let nt_len = u16::from_le_bytes([auth[20], auth[21]]) as usize;
        let nt_off = u32::from_le_bytes([auth[24], auth[25], auth[26], auth[27]]) as usize;
        assert!(nt_len >= 16 + 28);
        assert!(nt_off + nt_len <= auth.len());
        // session-security established
        assert!(c.send_sealing.is_some());
    }

    #[test]
    fn challenge_without_timestamp_uses_filetime_fallback() {
        let mut c = NtlmClient::new();
        c.build_negotiate();
        c.read_challenge(&sample_challenge_without_timestamp())
            .unwrap();
        assert_ne!(c.timestamp, 0);
    }

    #[test]
    fn sign_seal_roundtrip() {
        // Wire client.send_* to server.recv_* (same derived keys) and verify a sealed
        // message decrypts + its signature checks out — exercises the pubKeyAuth path.
        let mut client = NtlmClient::new();
        client.build_negotiate();
        client.read_challenge(&sample_challenge()).unwrap();
        client
            .build_authenticate("user", "DOMAIN", "pass", [0xaa; 8], [0xbb; 16])
            .unwrap();
        let sk = client.exported_session_key;

        let mut server = NtlmClient::new();
        server.recv_signing_key = md5_with(&sk, CLIENT_SIGN_MAGIC);
        server.recv_sealing = Some(Rc4::new(&md5_with(&sk, CLIENT_SEAL_MAGIC)));

        let plaintext = b"public-key-binding-hash-or-credentials";
        let sealed = client.encrypt(plaintext).unwrap();
        let recovered = server.decrypt(&sealed).unwrap();
        assert_eq!(recovered, plaintext);
    }
}
