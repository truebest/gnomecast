//! Minimal CredSSP (MS-CSSP) client over a proxy-terminated TLS channel.
//!
//! Drives NTLMv2 inside `TSRequest`s and protects the server's public key with
//! CredSSP v5+ `pubKeyAuth` (SHA-256 over magic+nonce+pubkey, NTLM-sealed). No
//! `picky`/asn1 deps — DER is hand-rolled in `der.rs`.

use crate::crypto::sha256;
use crate::der::{self, Der};
use crate::ntlm::NtlmClient;

const CLIENT_SERVER_HASH_MAGIC: &[u8] = b"CredSSP Client-To-Server Binding Hash\0";
const SERVER_CLIENT_HASH_MAGIC: &[u8] = b"CredSSP Server-To-Client Binding Hash\0";
const TS_REQUEST_VERSION: u32 = 6;
const TS_PASSWORD_CREDS: u32 = 1;

/// Encode a `TSRequest` (only the fields we ever send).
pub fn encode_ts_request(
    version: u32,
    nego_token: Option<&[u8]>,
    auth_info: Option<&[u8]>,
    pub_key_auth: Option<&[u8]>,
    client_nonce: Option<&[u8]>,
) -> Vec<u8> {
    let mut content = der::context(0, &der::integer(version)); // [0] version
    if let Some(nt) = nego_token {
        // [1] NegoData ::= SEQUENCE OF SEQUENCE { negoToken [0] OCTET STRING }
        let item = der::sequence(&der::context(0, &der::octet_string(nt)));
        let nego_data = der::sequence(&item);
        content.extend_from_slice(&der::context(1, &nego_data));
    }
    if let Some(ai) = auth_info {
        content.extend_from_slice(&der::context(2, &der::octet_string(ai))); // [2] authInfo
    }
    if let Some(pk) = pub_key_auth {
        content.extend_from_slice(&der::context(3, &der::octet_string(pk))); // [3] pubKeyAuth
    }
    if version >= 5 {
        if let Some(nonce) = client_nonce {
            content.extend_from_slice(&der::context(5, &der::octet_string(nonce)));
            // [5] clientNonce
        }
    }
    der::sequence(&content)
}

#[derive(Debug, Default)]
pub struct ParsedTsRequest {
    pub version: u32,
    pub nego_token: Option<Vec<u8>>,
    pub auth_info: Option<Vec<u8>>,
    pub pub_key_auth: Option<Vec<u8>>,
    pub error_code: Option<u32>,
}

/// Decode a `TSRequest` (the fields a server sends back).
pub fn parse_ts_request(buf: &[u8]) -> Result<ParsedTsRequest, String> {
    let mut d = Der::new(buf);
    let body = d.read_tag(0x30).map_err(str::to_string)?;
    let mut s = Der::new(body);
    let v = s.read_tag(0xA0).map_err(str::to_string)?;
    let version = der::int_from_bytes(Der::new(v).read_tag(0x02).map_err(str::to_string)?)
        .map_err(str::to_string)?;
    let mut out = ParsedTsRequest {
        version,
        ..Default::default()
    };
    if let Some(c1) = s.try_context(1) {
        let seq1 = Der::new(c1).read_tag(0x30).map_err(str::to_string)?;
        let seq2 = Der::new(seq1).read_tag(0x30).map_err(str::to_string)?;
        let t0 = Der::new(seq2).read_tag(0xA0).map_err(str::to_string)?;
        let os = Der::new(t0).read_tag(0x04).map_err(str::to_string)?;
        out.nego_token = Some(os.to_vec());
    }
    if let Some(c2) = s.try_context(2) {
        out.auth_info = Some(
            Der::new(c2)
                .read_tag(0x04)
                .map_err(str::to_string)?
                .to_vec(),
        );
    }
    if let Some(c3) = s.try_context(3) {
        out.pub_key_auth = Some(
            Der::new(c3)
                .read_tag(0x04)
                .map_err(str::to_string)?
                .to_vec(),
        );
    }
    if let Some(c4) = s.try_context(4) {
        let iv = Der::new(c4).read_tag(0x02).map_err(str::to_string)?;
        out.error_code = Some(der::int_from_bytes(iv).map_err(str::to_string)?);
    }
    Ok(out)
}

/// TSCredentials wrapping TSPasswordCreds (domain/user/password, UTF-16LE).
pub fn encode_ts_credentials(domain: &str, user: &str, password: &str) -> Vec<u8> {
    use crate::crypto::utf16le;
    let mut pwd = der::context(0, &der::octet_string(&utf16le(domain)));
    pwd.extend_from_slice(&der::context(1, &der::octet_string(&utf16le(user))));
    pwd.extend_from_slice(&der::context(2, &der::octet_string(&utf16le(password))));
    let ts_password_creds = der::sequence(&pwd);

    let mut creds = der::context(0, &der::integer(TS_PASSWORD_CREDS));
    creds.extend_from_slice(&der::context(1, &der::octet_string(&ts_password_creds)));
    der::sequence(&creds)
}

#[derive(PartialEq, Eq, Debug)]
enum State {
    Challenge,
    PubKey,
    Done,
}

/// CredSSP client state machine. The caller relays the returned bytes over the
/// direct TLS channel and feeds back the server's `TSRequest`s.
pub struct CredsspClient {
    ntlm: NtlmClient,
    public_key: Vec<u8>,
    client_nonce: [u8; 32],
    client_challenge: [u8; 8],
    key_exch_key: [u8; 16],
    user: String,
    domain: String,
    password: String,
    peer_version: u32,
    state: State,
}

impl CredsspClient {
    /// `public_key` is the server TLS cert SubjectPublicKey.
    /// The three random inputs must come from a native CSPRNG.
    pub fn new(
        public_key: Vec<u8>,
        user: &str,
        domain: &str,
        password: &str,
        client_challenge: [u8; 8],
        key_exch_key: [u8; 16],
        client_nonce: [u8; 32],
    ) -> Self {
        CredsspClient {
            ntlm: NtlmClient::new(),
            public_key,
            client_nonce,
            client_challenge,
            key_exch_key,
            user: user.to_string(),
            domain: domain.to_string(),
            password: password.to_string(),
            peer_version: TS_REQUEST_VERSION,
            state: State::Challenge,
        }
    }

    /// First client message: TSRequest carrying the NTLM NEGOTIATE.
    pub fn first_message(&mut self) -> Vec<u8> {
        let nego = self.ntlm.build_negotiate();
        encode_ts_request(TS_REQUEST_VERSION, Some(&nego), None, None, None)
    }

    pub fn is_done(&self) -> bool {
        self.state == State::Done
    }

    fn pubkey_hash(&self, magic: &[u8]) -> [u8; 32] {
        let mut data = magic.to_vec();
        data.extend_from_slice(&self.client_nonce);
        data.extend_from_slice(&self.public_key);
        sha256(&data)
    }

    /// Process a server `TSRequest`. Returns the next client message and whether
    /// CredSSP is complete after sending it.
    pub fn process_server(&mut self, server_ts: &[u8]) -> Result<(Vec<u8>, bool), String> {
        let parsed = parse_ts_request(server_ts)?;
        if let Some(code) = parsed.error_code {
            if code != 0 {
                return Err(format!("CredSSP server returned NTSTATUS 0x{code:08x}"));
            }
        }
        match self.state {
            State::Challenge => {
                self.peer_version = parsed.version;
                let challenge = parsed
                    .nego_token
                    .ok_or("CredSSP: server TSRequest missing negoTokens (challenge)")?;
                self.ntlm.read_challenge(&challenge)?;
                let auth = self.ntlm.build_authenticate(
                    &self.user,
                    &self.domain,
                    &self.password,
                    self.client_challenge,
                    self.key_exch_key,
                )?;
                // pubKeyAuth: v5+ uses SHA256(magic + nonce + pubkey); legacy uses the raw key.
                let plain = if self.peer_version >= 5 {
                    self.pubkey_hash(CLIENT_SERVER_HASH_MAGIC).to_vec()
                } else {
                    self.public_key.clone()
                };
                let pub_key_auth = self.ntlm.encrypt(&plain)?;
                let nonce = if self.peer_version >= 5 {
                    Some(&self.client_nonce[..])
                } else {
                    None
                };
                self.state = State::PubKey;
                Ok((
                    encode_ts_request(
                        TS_REQUEST_VERSION,
                        Some(&auth),
                        None,
                        Some(&pub_key_auth),
                        nonce,
                    ),
                    false,
                ))
            }
            State::PubKey => {
                let server_pub = parsed
                    .pub_key_auth
                    .ok_or("CredSSP: server TSRequest missing pubKeyAuth")?;
                let decrypted = self.ntlm.decrypt(&server_pub)?;
                if self.peer_version >= 5 {
                    let expected = self.pubkey_hash(SERVER_CLIENT_HASH_MAGIC);
                    if decrypted != expected {
                        return Err(
                            "CredSSP: server public-key hash mismatch (possible MITM)".into()
                        );
                    }
                } else {
                    // legacy: server returns pubkey with first byte incremented
                    let mut expected = self.public_key.clone();
                    if let Some(b) = expected.first_mut() {
                        *b = b.wrapping_add(1);
                    }
                    if decrypted != expected {
                        return Err(
                            "CredSSP: server public-key echo mismatch (possible MITM)".into()
                        );
                    }
                }
                // Final message: encrypted TSCredentials.
                let ts_creds = encode_ts_credentials(&self.domain, &self.user, &self.password);
                let auth_info = self.ntlm.encrypt(&ts_creds)?;
                self.state = State::Done;
                Ok((
                    encode_ts_request(TS_REQUEST_VERSION, None, Some(&auth_info), None, None),
                    true,
                ))
            }
            State::Done => Err("CredSSP already complete".into()),
        }
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::crypto::utf16le;

    #[test]
    fn ts_request_roundtrip() {
        let token = vec![0x11u8; 200]; // > 127 to exercise long-form length
        let pka = vec![0x22u8; 48];
        let nonce = [0x33u8; 32];
        let enc = encode_ts_request(6, Some(&token), None, Some(&pka), Some(&nonce));
        let p = parse_ts_request(&enc).unwrap();
        assert_eq!(p.version, 6);
        assert_eq!(p.nego_token.unwrap(), token);
        assert_eq!(p.pub_key_auth.unwrap(), pka);
    }

    #[test]
    fn ts_request_auth_info_and_error() {
        let enc = encode_ts_request(6, None, Some(&[0xab; 64]), None, None);
        let p = parse_ts_request(&enc).unwrap();
        assert_eq!(p.auth_info.unwrap(), vec![0xab; 64]);
        assert!(p.error_code.is_none());
    }

    #[test]
    fn ts_credentials_shape() {
        // TSCredentials = SEQ { [0] INTEGER 1, [1] OCTET STRING TSPasswordCreds }
        let c = encode_ts_credentials("DOM", "user", "pw");
        let mut d = Der::new(&c);
        let body = d.read_tag(0x30).unwrap();
        let mut s = Der::new(body);
        let ct = der::int_from_bytes(Der::new(s.read_tag(0xA0).unwrap()).read_tag(0x02).unwrap())
            .unwrap();
        assert_eq!(ct, TS_PASSWORD_CREDS);
        let inner_os = Der::new(s.read_tag(0xA1).unwrap()).read_tag(0x04).unwrap();
        // inner TSPasswordCreds: SEQ { [0] domain, [1] user, [2] password }
        let mut pw = Der::new(inner_os);
        let pwbody = pw.read_tag(0x30).unwrap();
        let mut f = Der::new(pwbody);
        let dom = Der::new(f.read_tag(0xA0).unwrap()).read_tag(0x04).unwrap();
        let usr = Der::new(f.read_tag(0xA1).unwrap()).read_tag(0x04).unwrap();
        let pass = Der::new(f.read_tag(0xA2).unwrap()).read_tag(0x04).unwrap();
        assert_eq!(dom, utf16le("DOM"));
        assert_eq!(usr, utf16le("user"));
        assert_eq!(pass, utf16le("pw"));
    }

    // Build a CHALLENGE the same way ntlm::test does (kept local for the flow test).
    fn make_challenge() -> Vec<u8> {
        let mut ti = Vec::new();
        ti.extend_from_slice(&2u16.to_le_bytes()); // NbDomainName
        let dom = utf16le("DOMAIN");
        ti.extend_from_slice(&(dom.len() as u16).to_le_bytes());
        ti.extend_from_slice(&dom);
        ti.extend_from_slice(&7u16.to_le_bytes()); // Timestamp
        ti.extend_from_slice(&8u16.to_le_bytes());
        ti.extend_from_slice(&0x01d8_0000_0000_0000u64.to_le_bytes());
        ti.extend_from_slice(&[0, 0, 0, 0]); // EOL

        let mut m = Vec::new();
        m.extend_from_slice(b"NTLMSSP\0");
        m.extend_from_slice(&2u32.to_le_bytes());
        m.extend_from_slice(&[0, 0, 0, 0, 56, 0, 0, 0]); // target name field (empty, offset 56)
        let flags: u32 = 0x8000_0000
            | 0x2000_0000
            | 0x4000_0000
            | 0x0008_0000
            | 0x0000_0200
            | 0x0000_0001
            | 0x0080_0000
            | 0x0200_0000;
        m.extend_from_slice(&flags.to_le_bytes());
        m.extend_from_slice(&[0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]); // server challenge
        m.extend_from_slice(&[0u8; 8]); // reserved
        m.extend_from_slice(&(ti.len() as u16).to_le_bytes());
        m.extend_from_slice(&(ti.len() as u16).to_le_bytes());
        m.extend_from_slice(&56u32.to_le_bytes()); // target info offset
        m.extend_from_slice(&[0x0a, 0x00, 0x63, 0x45, 0x00, 0x00, 0x00, 0x0f]); // version
        m.extend_from_slice(&ti);
        m
    }

    #[test]
    fn pubkeyauth_verifiable_by_peer() {
        // Client produces the v5+ pubKeyAuth; a peer holding the client's send keys
        // (as its recv keys) decrypts it and reproduces the SHA-256 binding hash.
        let public_key = vec![0x09u8; 270];
        let nonce = [0x42u8; 32];
        let mut client = CredsspClient::new(
            public_key.clone(),
            "user",
            "DOMAIN",
            "pw",
            [0xaa; 8],
            [0xbb; 16],
            nonce,
        );
        let _nego = client.first_message();
        let challenge_ts = encode_ts_request(6, Some(&make_challenge()), None, None, None);
        let (auth_ts, done) = client.process_server(&challenge_ts).unwrap();
        assert!(!done);
        let parsed = parse_ts_request(&auth_ts).unwrap();
        let pub_key_auth = parsed.pub_key_auth.expect("pubKeyAuth present");

        // Reconstruct the peer's recv keys from the client's exported session key.
        let sk = client.ntlm.exported_session_key;
        let mut peer = NtlmClient::new();
        // Set the peer's recv_* to the client's send_* via the public helper-less path:
        peer.set_test_recv_keys(&sk);
        let recovered = peer.decrypt(&pub_key_auth).unwrap();

        let mut expected = CLIENT_SERVER_HASH_MAGIC.to_vec();
        expected.extend_from_slice(&nonce);
        expected.extend_from_slice(&public_key);
        assert_eq!(recovered, sha256(&expected));
    }
}
