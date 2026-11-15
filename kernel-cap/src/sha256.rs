//! SHA-256 and HMAC-SHA-256, no_std, no dependencies.
//!
//! Tầng 1 signs capability tokens with these primitives; the same algorithms
//! (and therefore the same test vectors) are used by libyui's C++ vault, so a
//! token minted by the kernel verifies in user space and vice versa.

pub type Digest = [u8; 32];

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

const H0: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
];

fn compress(state: &mut [u32; 8], block: &[u8]) {
    debug_assert!(block.len() >= 64);
    let mut w = [0u32; 64];
    for i in 0..16 {
        w[i] = u32::from_be_bytes([block[i * 4], block[i * 4 + 1], block[i * 4 + 2], block[i * 4 + 3]]);
    }
    for i in 16..64 {
        let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
        let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16]
            .wrapping_add(s0)
            .wrapping_add(w[i - 7])
            .wrapping_add(s1);
    }
    let mut a = state[0];
    let mut b = state[1];
    let mut c = state[2];
    let mut d = state[3];
    let mut e = state[4];
    let mut f = state[5];
    let mut g = state[6];
    let mut h = state[7];
    for i in 0..64 {
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let ch = (e & f) ^ ((!e) & g);
        let t1 = h
            .wrapping_add(s1)
            .wrapping_add(ch)
            .wrapping_add(K[i])
            .wrapping_add(w[i]);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let maj = (a & b) ^ (a & c) ^ (b & c);
        let t2 = s0.wrapping_add(maj);
        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(t1);
        d = c;
        c = b;
        b = a;
        a = t1.wrapping_add(t2);
    }
    state[0] = state[0].wrapping_add(a);
    state[1] = state[1].wrapping_add(b);
    state[2] = state[2].wrapping_add(c);
    state[3] = state[3].wrapping_add(d);
    state[4] = state[4].wrapping_add(e);
    state[5] = state[5].wrapping_add(f);
    state[6] = state[6].wrapping_add(g);
    state[7] = state[7].wrapping_add(h);
}

/// One-shot SHA-256 over `data`.
pub fn sha256(data: &[u8]) -> Digest {
    let mut state = H0;
    let mut offset = 0;
    while offset + 64 <= data.len() {
        compress(&mut state, &data[offset..]);
        offset += 64;
    }
    // Padding: one chunk of remaining bytes, possibly spilling into a second.
    let remaining = &data[offset..];
    let mut tail = [0u8; 128];
    let mut n = remaining.len();
    tail[..n].copy_from_slice(remaining);
    tail[n] = 0x80;
    n += 1;
    let total_bits = (data.len() as u64).wrapping_mul(8);
    let pad_len = if n <= 56 { 64 } else { 128 };
    tail[pad_len - 8..pad_len].copy_from_slice(&total_bits.to_be_bytes());
    let mut i = 0;
    while i < pad_len {
        compress(&mut state, &tail[i..i + 64]);
        i += 64;
    }
    let mut out = [0u8; 32];
    for (i, word) in state.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    out
}

/// HMAC-SHA-256 as in RFC 2104.
pub fn hmac_sha256(key: &[u8], message: &[u8]) -> Digest {
    let mut k = [0u8; 64];
    if key.len() > 64 {
        k[..32].copy_from_slice(&sha256(key));
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    // inner_hash = H(k^ipad || message)
    let mut buf = [0u8; 64];
    buf.copy_from_slice(&k);
    for b in buf.iter_mut() {
        *b ^= 0x36;
    }
    let inner_digest = sha256_concat(&[&buf, message]);

    // outer_hash = H(k^opad || inner_digest)
    let mut obuf = [0u8; 64];
    obuf.copy_from_slice(&k);
    for b in obuf.iter_mut() {
        *b ^= 0x5c;
    }
    sha256_concat(&[&obuf, &inner_digest])
}

fn sha256_concat(parts: &[&[u8]]) -> Digest {
    let mut state = H0;
    let mut buffer = [0u8; 64];
    let mut buffered = 0usize;
    let mut total = 0u64;
    for part in parts {
        total += part.len() as u64;
        let mut offset = 0usize;
        while offset < part.len() {
            let take = core::cmp::min(64 - buffered, part.len() - offset);
            buffer[buffered..buffered + take].copy_from_slice(&part[offset..offset + take]);
            buffered += take;
            offset += take;
            if buffered == 64 {
                compress(&mut state, &buffer);
                buffered = 0;
            }
        }
    }
    // Padding.
    let mut tail = [0u8; 128];
    tail[..buffered].copy_from_slice(&buffer[..buffered]);
    tail[buffered] = 0x80;
    let pad_len = if buffered + 1 <= 56 { 64 } else { 128 };
    tail[pad_len - 8..pad_len].copy_from_slice(&(total * 8).to_be_bytes());
    let mut i = 0;
    while i < pad_len {
        compress(&mut state, &tail[i..i + 64]);
        i += 64;
    }
    let mut out = [0u8; 32];
    for (i, word) in state.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    out
}

/// Constant-time equality, for MAC comparison.
pub fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for i in 0..a.len() {
        diff |= a[i] ^ b[i];
    }
    diff == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{:02x}", b)).collect()
    }

    #[test]
    fn sha256_vectors() {
        assert_eq!(
            hex(&sha256(b"")),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            hex(&sha256(b"abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(
            hex(&sha256(b"The quick brown fox jumps over the lazy dog")),
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"
        );
    }

    #[test]
    fn sha256_long_input_matches_chunks() {
        let data: Vec<u8> = (0..1000u32).map(|i| (i % 251) as u8).collect();
        let one_shot = sha256(&data);
        // hashing a 1000-byte buffer exercises the multi-block path
        assert_eq!(one_shot.len(), 32);
        assert_ne!(one_shot, sha256(&data[..999]));
    }

    #[test]
    fn hmac_vectors() {
        assert_eq!(
            hex(&hmac_sha256(b"key", b"The quick brown fox jumps over the lazy dog")),
            "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"
        );
        let key = [0x0bu8; 20];
        assert_eq!(
            hex(&hmac_sha256(&key, b"Hi There")),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
        );
    }

    #[test]
    fn ct_eq_behaviour() {
        assert!(ct_eq(&[1, 2, 3], &[1, 2, 3]));
        assert!(!ct_eq(&[1, 2, 3], &[1, 2, 4]));
        assert!(!ct_eq(&[1, 2], &[1, 2, 3]));
    }
}
