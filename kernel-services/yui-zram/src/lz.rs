//! YUI-LZ: byte-oriented LZ77 with a 64 KiB window.
//!
//! Token layout (little-endian, identical to kernel-services/codec.cpp):
//!   token = (literal_len << 4) | (match_len - 4)
//!   either nibble == 15 means "read 0xFF-chained extension bytes"
//!   literals | 2-byte distance | (match extension)
//! The final sequence carries literals only and no distance.

use alloc::vec;
use alloc::vec::Vec;

const MIN_MATCH: usize = 4;
const MAX_DISTANCE: usize = 65_535;
const NIBBLE_MAX: usize = 15;
const HASH_BITS: u32 = 16;
const HASH_SIZE: usize = 1 << HASH_BITS;

/// Worst-case output size for `n` input bytes.
pub fn max_compressed_len(n: usize) -> usize {
    n + n / 255 + 16
}

fn hash4(data: &[u8], pos: usize) -> usize {
    let v = u32::from_le_bytes([data[pos], data[pos + 1], data[pos + 2], data[pos + 3]]);
    let h = v.wrapping_mul(2_654_435_761);
    (h >> (32 - HASH_BITS)) as usize
}

fn put_len(out: &mut Vec<u8>, mut extra: usize) {
    while extra >= 255 {
        out.push(255);
        extra -= 255;
    }
    out.push(extra as u8);
}

fn put_u16(out: &mut Vec<u8>, v: u16) {
    out.push((v & 0xff) as u8);
    out.push((v >> 8) as u8);
}

/// Compresses `input`. Returns `None` only when the input length overflows a
/// `u16` distance window plus the size bound (it never happens for page-sized
/// buffers, but callers get a chance to handle it).
pub fn compress(input: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(max_compressed_len(input.len()));
    if input.is_empty() {
        out.push(0);
        return Some(out);
    }
    if input.len() > (1 << 24) {
        return None;
    }

    let mut head = vec![-1i32; HASH_SIZE];
    let mut prev = vec![-1i32; input.len()];
    let mut anchor = 0usize;
    let mut pos = 0usize;

    while pos + MIN_MATCH <= input.len() {
        let h = hash4(input, pos);
        let candidate_start = head[h];
        head[h] = pos as i32;
        prev[pos] = candidate_start;

        let limit = input.len() - pos;
        let mut best_len = 0usize;
        let mut best_dist = 0usize;
        let mut candidate = candidate_start;
        let mut probes = 4;
        while candidate >= 0 && probes > 0 {
            probes -= 1;
            let cand = candidate as usize;
            let dist = pos.saturating_sub(cand);
            if dist == 0 || dist > MAX_DISTANCE {
                break;
            }
            if best_len < limit {
                let mut len = 0usize;
                while len < limit && input[cand + len] == input[pos + len] {
                    len += 1;
                }
                if len > best_len {
                    best_len = len;
                    best_dist = dist;
                    if len >= 128 {
                        break;
                    }
                }
            }
            candidate = prev[cand];
        }

        if best_len >= MIN_MATCH {
            let lit_len = pos - anchor;
            let match_extra = best_len - MIN_MATCH;
            out.push(((lit_len.min(NIBBLE_MAX) as u8) << 4) | (match_extra.min(NIBBLE_MAX) as u8));
            if lit_len >= NIBBLE_MAX {
                put_len(&mut out, lit_len - NIBBLE_MAX);
            }
            out.extend_from_slice(&input[anchor..pos]);
            put_u16(&mut out, best_dist as u16);
            if match_extra >= NIBBLE_MAX {
                put_len(&mut out, match_extra - NIBBLE_MAX);
            }
            // Register the positions the match skipped over.
            let mut p = pos + 1;
            while p < pos + best_len && p + MIN_MATCH <= input.len() {
                let hh = hash4(input, p);
                prev[p] = head[hh];
                head[hh] = p as i32;
                p += 1;
            }
            pos += best_len;
            anchor = pos;
        } else {
            pos += 1;
        }
    }

    let lit_len = input.len() - anchor;
    out.push((lit_len.min(NIBBLE_MAX) as u8) << 4);
    if lit_len >= NIBBLE_MAX {
        put_len(&mut out, lit_len - NIBBLE_MAX);
    }
    out.extend_from_slice(&input[anchor..]);
    Some(out)
}

/// Decompresses into exactly `out_len` bytes. Returns `None` on any corruption.
pub fn decompress(input: &[u8], out_len: usize) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(out_len);
    let mut ip = 0usize;
    while ip < input.len() {
        let token = input[ip];
        ip += 1;
        let mut lit_len = (token >> 4) as usize;
        if lit_len == NIBBLE_MAX {
            loop {
                let b = *input.get(ip)?;
                ip += 1;
                lit_len += b as usize;
                if b != 255 {
                    break;
                }
            }
        }
        if ip + lit_len > input.len() || out.len() + lit_len > out_len {
            return None;
        }
        out.extend_from_slice(&input[ip..ip + lit_len]);
        ip += lit_len;
        if ip == input.len() {
            break; // final sequence: no match follows
        }
        if ip + 2 > input.len() {
            return None;
        }
        let dist = u16::from_le_bytes([input[ip], input[ip + 1]]) as usize;
        ip += 2;
        let mut match_len = (token & 0x0f) as usize + MIN_MATCH;
        if (token & 0x0f) as usize == NIBBLE_MAX {
            loop {
                let b = *input.get(ip)?;
                ip += 1;
                match_len += b as usize;
                if b != 255 {
                    break;
                }
            }
        }
        if dist == 0 || dist > out.len() || out.len() + match_len > out_len {
            return None;
        }
        // Overlapping copy, byte by byte.
        for _ in 0..match_len {
            let byte = out[out.len() - dist];
            out.push(byte);
        }
    }
    if out.len() != out_len {
        return None;
    }
    Some(out)
}
