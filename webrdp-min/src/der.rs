//! Minimal DER (definite-length BER) encode/decode for CredSSP `TSRequest` and
//! `TSCredentials`. Hand-rolled so the native staticlib does not pull in the
//! larger picky/asn1 dependency stack. Encoding is built inside-out.

// ----- encode -----

/// DER length octets for a content length `n`.
pub fn der_len(n: usize) -> Vec<u8> {
    if n < 0x80 {
        vec![n as u8]
    } else {
        let mut be = Vec::new();
        let mut v = n;
        while v > 0 {
            be.push((v & 0xff) as u8);
            v >>= 8;
        }
        be.reverse();
        let mut out = Vec::with_capacity(be.len() + 1);
        out.push(0x80 | be.len() as u8);
        out.extend_from_slice(&be);
        out
    }
}

/// Tag-Length-Value with an explicit tag byte.
pub fn tlv(tag: u8, content: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(content.len() + 4);
    out.push(tag);
    out.extend_from_slice(&der_len(content.len()));
    out.extend_from_slice(content);
    out
}

/// DER INTEGER (minimal big-endian, leading 0x00 if the MSB is set).
pub fn integer(value: u32) -> Vec<u8> {
    let mut bytes = value.to_be_bytes().to_vec();
    while bytes.len() > 1 && bytes[0] == 0 {
        bytes.remove(0);
    }
    if bytes[0] & 0x80 != 0 {
        bytes.insert(0, 0x00);
    }
    tlv(0x02, &bytes)
}

pub fn octet_string(data: &[u8]) -> Vec<u8> {
    tlv(0x04, data)
}

pub fn sequence(content: &[u8]) -> Vec<u8> {
    tlv(0x30, content)
}

/// Explicit context tag `[n]` (constructed): 0xA0 | n.
pub fn context(n: u8, content: &[u8]) -> Vec<u8> {
    tlv(0xA0 | n, content)
}

// ----- decode -----

pub struct Der<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Der<'a> {
    pub fn new(buf: &'a [u8]) -> Self {
        Der { buf, pos: 0 }
    }

    pub fn at_end(&self) -> bool {
        self.pos >= self.buf.len()
    }

    pub fn peek_tag(&self) -> Option<u8> {
        self.buf.get(self.pos).copied()
    }

    fn read_len(&mut self) -> Result<usize, &'static str> {
        let b = *self.buf.get(self.pos).ok_or("der: eof at length")?;
        self.pos += 1;
        if b < 0x80 {
            return Ok(b as usize);
        }
        let n = (b & 0x7f) as usize;
        if n == 0 || n > 4 {
            return Err("der: unsupported length form");
        }
        let mut len = 0usize;
        for _ in 0..n {
            let x = *self.buf.get(self.pos).ok_or("der: eof in long length")?;
            self.pos += 1;
            len = (len << 8) | x as usize;
        }
        Ok(len)
    }

    /// Read a TLV with the expected tag; returns the content slice.
    pub fn read_tag(&mut self, tag: u8) -> Result<&'a [u8], &'static str> {
        let t = *self.buf.get(self.pos).ok_or("der: eof at tag")?;
        if t != tag {
            return Err("der: unexpected tag");
        }
        self.pos += 1;
        let len = self.read_len()?;
        let start = self.pos;
        let end = start.checked_add(len).ok_or("der: length overflow")?;
        if end > self.buf.len() {
            return Err("der: content past end");
        }
        self.pos = end;
        Ok(&self.buf[start..end])
    }

    /// Read an optional context tag `[n]`; returns its content if present.
    pub fn try_context(&mut self, n: u8) -> Option<&'a [u8]> {
        if self.peek_tag() == Some(0xA0 | n) {
            self.read_tag(0xA0 | n).ok()
        } else {
            None
        }
    }
}

/// Decode a DER INTEGER content (the value bytes, already unwrapped from 0x02) into u32.
pub fn int_from_bytes(content: &[u8]) -> Result<u32, &'static str> {
    let bytes = if content.len() > 1 && content[0] == 0 {
        &content[1..]
    } else {
        content
    };
    if bytes.len() > 4 {
        return Err("der: integer too large for u32");
    }
    let mut v: u32 = 0;
    for &b in bytes {
        v = (v << 8) | b as u32;
    }
    Ok(v)
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn len_forms() {
        assert_eq!(der_len(5), vec![0x05]);
        assert_eq!(der_len(0xFA), vec![0x81, 0xFA]);
        assert_eq!(der_len(0x100), vec![0x82, 0x01, 0x00]);
    }

    #[test]
    fn integer_encoding() {
        assert_eq!(integer(6), vec![0x02, 0x01, 0x06]);
        assert_eq!(integer(0x80), vec![0x02, 0x02, 0x00, 0x80]); // leading zero (MSB set)
        assert_eq!(integer(0x800000), vec![0x02, 0x04, 0x00, 0x80, 0x00, 0x00]);
    }

    #[test]
    fn sequence_octet_string_shape() {
        // [3] { OCTET STRING "hello" } == A3 07 04 05 68 65 6C 6C 6F  (matches sspi BER)
        let v = context(3, &octet_string(b"hello"));
        assert_eq!(
            v,
            vec![0xA3, 0x07, 0x04, 0x05, 0x68, 0x65, 0x6C, 0x6C, 0x6F]
        );
    }

    #[test]
    fn roundtrip_context_int() {
        let enc = sequence(&context(0, &integer(6)));
        let mut d = Der::new(&enc);
        let body = d.read_tag(0x30).unwrap();
        let mut inner = Der::new(body);
        let c0 = inner.read_tag(0xA0).unwrap();
        let mut c0d = Der::new(c0);
        let iv = c0d.read_tag(0x02).unwrap();
        assert_eq!(int_from_bytes(iv).unwrap(), 6);
    }
}
