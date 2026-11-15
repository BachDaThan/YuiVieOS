//! Ed25519 (RFC 8032), no_std, no dependencies — clean-room song song với bản
//! C++ tham chiếu `kernel-services/ed25519.cpp` (đã khớp 29/29 vector
//! FIPS 180-4 + RFC 8032 §7.1). Cùng thuật toán, cùng thứ tự phép tính:
//!   SHA-512 → số học GF(p), p = 2^255-19 → twisted Edwards a = -1
//!   → scalar mod L, L = 2^252 + 27742317777372353535851937790883648493.
//!
//! Điểm dùng toạ độ mở rộng (X,Y,Z,T) + công thức **cộng đầy đủ** RFC §5.1.4
//! (add-2008-hwcd-3) — đúng cho mọi cặp điểm kể cả P == Q, nên scalar mult
//! chỉ cần một routine cộng. Số học: limb 32-bit LE, rút gọn mod m bằng trừ
//! dài theo bit — đơn giản, đúng với mọi đầu vào (tốc độ không phải mục tiêu
//! của gói cập nhật ngoại tuyến).

use crate::sha512::{sha512, Digest512, Sha512};

type U256 = [u32; 8];
type U512 = [u32; 16];

/// p = 2^255 − 19 (limb LE 32-bit).
const FE_P: U256 = [
    0xffffffed, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0x7fffffff,
];
/// L = 2^252 + 27742317777372353535851937790883648493 (RFC 8032 §5.1).
const SC_L: U256 = [0x5cf5d3ed, 0x5812631a, 0xa2f79cd6, 0x14def9de, 0, 0, 0, 0x10000000];

// ------------------------------------------------------------- big numbers --
fn bn_cmp(a: &[u32], b: &[u32]) -> core::cmp::Ordering {
    use core::cmp::Ordering::*;
    let n = core::cmp::max(a.len(), b.len());
    for i in (0..n).rev() {
        let av = if i < a.len() { a[i] } else { 0 };
        let bv = if i < b.len() { b[i] } else { 0 };
        if av != bv {
            return if av < bv { Less } else { Greater };
        }
    }
    Equal
}

/// a −= b (a dài hơn b, giả sử a ≥ b về giá trị nguyên).
fn bn_sub_into(a: &mut [u32], b: &[u32]) {
    let mut borrow: i64 = 0;
    for i in 0..a.len() {
        let bv: i64 = if i < b.len() { b[i] as i64 } else { 0 };
        let mut d = (a[i] as i64) - bv - borrow;
        if d < 0 {
            d += 1i64 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a[i] = d as u32;
    }
}

/// r = x mod m (x 512-bit, m 256-bit) — trừ dài theo bit, luôn đúng.
fn bn_mod(x: &U512, m: &U256) -> U256 {
    let mut r = [0u32; 16];
    for i in (0..512).rev() {
        let mut carry = (x[i >> 5] >> (i & 31)) & 1;
        for k in 0..16 {
            let next = r[k] >> 31;
            r[k] = (r[k] << 1) | carry;
            carry = next;
        }
        if bn_cmp(&r, m) != core::cmp::Ordering::Less {
            bn_sub_into(&mut r, m);
        }
    }
    let mut out = [0u32; 8];
    out.copy_from_slice(&r[..8]);
    out
}

fn bn_mul256(a: &U256, b: &U256) -> U512 {
    let mut r = [0u32; 16];
    for i in 0..8 {
        let mut carry: u64 = 0;
        for j in 0..8 {
            let cur = (r[i + j] as u64) + carry + (a[i] as u64) * (b[j] as u64);
            r[i + j] = cur as u32;
            carry = cur >> 32;
        }
        let mut k = i + 8;
        while carry != 0 && k < 16 {
            let cur = (r[k] as u64) + carry;
            r[k] = cur as u32;
            carry = cur >> 32;
            k += 1;
        }
    }
    r
}

// -------------------------------------------------------------------- field --
fn fe_from_u64(v: u64) -> U256 {
    let mut r = [0u32; 8];
    r[0] = v as u32;
    r[1] = (v >> 32) as u32;
    r
}

fn fe_add(a: &U256, b: &U256) -> U256 {
    let mut t = [0u32; 16];
    let mut carry: u64 = 0;
    for i in 0..8 {
        let s = (a[i] as u64) + (b[i] as u64) + carry;
        t[i] = s as u32;
        carry = s >> 32;
    }
    t[8] = carry as u32;
    bn_mod(&t, &FE_P)
}

fn fe_sub(a: &U256, b: &U256) -> U256 {
    // a − b mod p = (a + p − b) mod p — tính trong 512-bit để không tràn.
    let mut t = [0u32; 16];
    let mut carry: u64 = 0;
    for i in 0..8 {
        let s = (a[i] as u64) + (FE_P[i] as u64) + carry;
        t[i] = s as u32;
        carry = s >> 32;
    }
    t[8] = carry as u32;
    bn_sub_into(&mut t, b);
    bn_mod(&t, &FE_P)
}

fn fe_mul(a: &U256, b: &U256) -> U256 {
    bn_mod(&bn_mul256(a, b), &FE_P)
}

fn fe_sq(a: &U256) -> U256 {
    fe_mul(a, a)
}

fn fe_is_zero(a: &U256) -> bool {
    a.iter().all(|&v| v == 0)
}

fn fe_eq(a: &U256, b: &U256) -> bool {
    a == b
}

fn fe_is_odd(a: &U256) -> bool {
    (a[0] & 1) != 0
}

fn fe_neg(a: &U256) -> U256 {
    if fe_is_zero(a) {
        return *a;
    }
    let z = [0u32; 8];
    fe_sub(&z, a)
}

fn fe_pow(a: &U256, e: &U256) -> U256 {
    let mut r = fe_from_u64(1);
    for i in (0..256).rev() {
        r = fe_sq(&r);
        if ((e[i >> 5] >> (i & 31)) & 1) != 0 {
            r = fe_mul(&r, a);
        }
    }
    r
}

fn fe_inv(a: &U256) -> U256 {
    // a^(p−2) mod p — nghịch đảo Fermat (p nguyên tố).
    let mut e = FE_P;
    let two = fe_from_u64(2);
    bn_sub_into(&mut e, &two);
    fe_pow(a, &e)
}

/// Đọc 32 byte LE, **xóa bit 255** (RFC 8032 §5.1.3: tọa độ y).
fn fe_from_bytes(inb: &[u8; 32]) -> U256 {
    let mut r = [0u32; 8];
    for i in 0..8 {
        r[i] = (inb[i * 4] as u32)
            | ((inb[i * 4 + 1] as u32) << 8)
            | ((inb[i * 4 + 2] as u32) << 16)
            | ((inb[i * 4 + 3] as u32) << 24);
    }
    r[7] &= 0x7fffffff;
    r
}

/// Đọc 32 byte LE thành scalar NGUYÊN (không xóa bit — dùng cho S trong chữ ký).
fn scalar_from_bytes(inb: &[u8; 32]) -> U256 {
    let mut r = [0u32; 8];
    for i in 0..8 {
        r[i] = (inb[i * 4] as u32)
            | ((inb[i * 4 + 1] as u32) << 8)
            | ((inb[i * 4 + 2] as u32) << 16)
            | ((inb[i * 4 + 3] as u32) << 24);
    }
    r
}

fn u256_to_bytes(a: &U256, out: &mut [u8; 32]) {
    for i in 0..8 {
        out[i * 4] = a[i] as u8;
        out[i * 4 + 1] = (a[i] >> 8) as u8;
        out[i * 4 + 2] = (a[i] >> 16) as u8;
        out[i * 4 + 3] = (a[i] >> 24) as u8;
    }
}

// ------------------------------------------------------------- hằng số cong --
struct Ctx {
    d: U256,       // d = −121665 · inv(121666) mod p (RFC §5.1)
    sqrt_m1: U256, // căn bậc hai của −1 mod p = 2^((p−1)/4)
}

fn ctx() -> Ctx {
    let d = fe_mul(&fe_neg(&fe_from_u64(121665)), &fe_inv(&fe_from_u64(121666)));
    // (p−1)/4 = 2^253 − 5
    let mut e = [0u32; 8];
    e[7] = 0x20000000;
    let five = fe_from_u64(5);
    bn_sub_into(&mut e, &five);
    let sqrt_m1 = fe_pow(&fe_from_u64(2), &e);
    Ctx { d, sqrt_m1 }
}

// -------------------------------------------------------------------- point --
#[derive(Clone, Copy)]
struct Point {
    x: U256,
    y: U256,
    z: U256,
    t: U256,
}

fn point_identity() -> Point {
    Point {
        x: fe_from_u64(0),
        y: fe_from_u64(1),
        z: fe_from_u64(1),
        t: fe_from_u64(0),
    }
}

/// Cộng đầy đủ trên twisted Edwards a = −1 — RFC 8032 §5.1.4 (add-2008-hwcd-3).
fn point_add(p1: &Point, p2: &Point, d: &U256) -> Point {
    let a = fe_mul(&fe_sub(&p1.y, &p1.x), &fe_sub(&p2.y, &p2.x));
    let b = fe_mul(&fe_add(&p1.y, &p1.x), &fe_add(&p2.y, &p2.x));
    let two = fe_from_u64(2);
    let c = fe_mul(&fe_mul(&fe_mul(&p1.t, &two), d), &p2.t);
    let dd = fe_mul(&fe_mul(&p1.z, &two), &p2.z);
    let e = fe_sub(&b, &a);
    let f = fe_sub(&dd, &c);
    let g = fe_add(&dd, &c);
    let h = fe_add(&b, &a);
    Point {
        x: fe_mul(&e, &f),
        y: fe_mul(&g, &h),
        z: fe_mul(&f, &g),
        t: fe_mul(&e, &h),
    }
}

fn point_equal(p1: &Point, p2: &Point) -> bool {
    // x1/z1 == x2/z2  ⇔  x1·z2 == x2·z1 (tương tự với y).
    if !fe_eq(&fe_mul(&p1.x, &p2.z), &fe_mul(&p2.x, &p1.z)) {
        return false;
    }
    fe_eq(&fe_mul(&p1.y, &p2.z), &fe_mul(&p2.y, &p1.z))
}

fn point_mul(s: &U256, p: &Point, d: &U256) -> Point {
    let mut q = point_identity();
    for i in (0..256).rev() {
        q = point_add(&q, &q, d);
        if ((s[i >> 5] >> (i & 31)) & 1) != 0 {
            q = point_add(&q, p, d);
        }
    }
    q
}

/// recover_x: RFC 8032 §5.1.3 (mũ (p+3)/8, nhân sqrt(−1) nếu cần).
fn recover_x(y: &U256, sign: bool, c: &Ctx) -> Option<U256> {
    if bn_cmp(y, &FE_P) != core::cmp::Ordering::Less {
        return None;
    }
    let y2 = fe_sq(y);
    let u = fe_sub(&y2, &fe_from_u64(1));
    let v = fe_add(&fe_mul(&c.d, &y2), &fe_from_u64(1));
    let x2 = fe_mul(&u, &fe_inv(&v));
    if fe_is_zero(&x2) {
        if sign {
            return None;
        }
        return Some(fe_from_u64(0));
    }
    // (p+3)/8 = 2^252 − 2
    let mut e = [0u32; 8];
    e[7] = 0x10000000;
    let two = fe_from_u64(2);
    bn_sub_into(&mut e, &two);
    let mut x = fe_pow(&x2, &e);
    if !fe_eq(&fe_sq(&x), &x2) {
        x = fe_mul(&x, &c.sqrt_m1);
        if !fe_eq(&fe_sq(&x), &x2) {
            return None;
        }
    }
    if fe_is_odd(&x) != sign {
        x = fe_neg(&x);
    }
    Some(x)
}

fn point_decompress(inb: &[u8; 32], c: &Ctx) -> Option<Point> {
    let sign = ((inb[31] >> 7) & 1) != 0;
    let y = fe_from_bytes(inb);
    let x = recover_x(&y, sign, c)?;
    Some(Point {
        x,
        y,
        z: fe_from_u64(1),
        t: fe_mul(&x, &y),
    })
}

fn point_compress(p: &Point, out: &mut [u8; 32]) {
    let zinv = fe_inv(&p.z);
    let x = fe_mul(&p.x, &zinv);
    let y = fe_mul(&p.y, &zinv);
    u256_to_bytes(&y, out);
    if fe_is_odd(&x) {
        out[31] |= 0x80;
    }
}

/// Điểm cơ sở B: y = 4/5, x = recover_x(y, 0) (RFC §5.1).
fn base_point(c: &Ctx) -> Point {
    let gy = fe_mul(&fe_from_u64(4), &fe_inv(&fe_from_u64(5)));
    let gx = recover_x(&gy, false, c).expect("cơ sở phải tồn tại");
    Point {
        x: gx,
        y: gy,
        z: fe_from_u64(1),
        t: fe_mul(&gx, &gy),
    }
}

// ------------------------------------------------------------------ scalars --
fn sc_reduce64(le: &[u8; 64]) -> U256 {
    let mut t = [0u32; 16];
    for i in 0..16 {
        t[i] = (le[i * 4] as u32)
            | ((le[i * 4 + 1] as u32) << 8)
            | ((le[i * 4 + 2] as u32) << 16)
            | ((le[i * 4 + 3] as u32) << 24);
    }
    bn_mod(&t, &SC_L)
}

/// s = (k·a + r) mod L
fn sc_muladd(k: &U256, a: &U256, r: &U256) -> U256 {
    let mut t = bn_mul256(k, a);
    let mut carry: u64 = 0;
    for i in 0..8 {
        let s = (t[i] as u64) + (r[i] as u64) + carry;
        t[i] = s as u32;
        carry = s >> 32;
    }
    let mut i = 8;
    while carry != 0 && i < 16 {
        let s = (t[i] as u64) + carry;
        t[i] = s as u32;
        carry = s >> 32;
        i += 1;
    }
    bn_mod(&t, &SC_L)
}

/// secret_expand: RFC 8032 §5.1.5 — a = clamp(SHA-512(seed)[0..32]),
/// prefix = SHA-512(seed)[32..64].
fn secret_expand(seed: &[u8; 32]) -> (U256, [u8; 32]) {
    let h = sha512(seed);
    let mut tmp = [0u8; 32];
    tmp.copy_from_slice(&h[..32]);
    tmp[0] &= 248; // xóa 3 bit thấp nhất
    tmp[31] &= 63; // xóa bit cao nhất
    tmp[31] |= 64; // đặt bit cao thứ hai
    let mut prefix = [0u8; 32];
    prefix.copy_from_slice(&h[32..64]);
    (fe_from_bytes(&tmp), prefix)
}

// ------------------------------------------------------------------ public API
/// Khóa công khai A = [clamp(SHA-512(seed))]·B, nén 32 byte.
pub fn public_key(seed: &[u8; 32]) -> [u8; 32] {
    let c = ctx();
    let (a, _) = secret_expand(seed);
    let mut out = [0u8; 32];
    point_compress(&point_mul(&a, &base_point(&c), &c.d), &mut out);
    out
}

/// Ký thông điệp: sig = R ‖ S (64 byte) — RFC 8032 §5.1.6.
pub fn sign(seed: &[u8; 32], msg: &[u8]) -> [u8; 64] {
    let c = ctx();
    let (a, prefix) = secret_expand(seed);

    // A = [a]B
    let mut pubk = [0u8; 32];
    point_compress(&point_mul(&a, &base_point(&c), &c.d), &mut pubk);

    // r = SHA-512(prefix ‖ M) mod L
    let mut h1 = Sha512::new();
    h1.update(&prefix);
    h1.update(msg);
    let d1: Digest512 = h1.finalize();
    let mut d1a = [0u8; 64];
    d1a.copy_from_slice(&d1);
    let r = sc_reduce64(&d1a);

    let mut sig = [0u8; 64];
    let (r_bytes, s_bytes) = sig.split_at_mut(32);
    let mut rb = [0u8; 32];
    point_compress(&point_mul(&r, &base_point(&c), &c.d), &mut rb);
    r_bytes.copy_from_slice(&rb);

    // k = SHA-512(R ‖ A ‖ M) mod L
    let mut h2 = Sha512::new();
    h2.update(&rb);
    h2.update(&pubk);
    h2.update(msg);
    let d2: Digest512 = h2.finalize();
    let mut d2a = [0u8; 64];
    d2a.copy_from_slice(&d2);
    let k = sc_reduce64(&d2a);

    // S = (r + k·a) mod L
    let s = sc_muladd(&k, &a, &r);
    let mut sb = [0u8; 32];
    u256_to_bytes(&s, &mut sb);
    s_bytes.copy_from_slice(&sb);
    sig
}

/// Kiểm chứng chữ ký — RFC 8032 §5.1.7: [S]B == R + [k]A.
pub fn verify(public: &[u8; 32], msg: &[u8], signature: &[u8; 64]) -> bool {
    let c = ctx();
    let a_point = match point_decompress(public, &c) {
        Some(p) => p,
        None => return false,
    };
    let mut r32 = [0u8; 32];
    r32.copy_from_slice(&signature[..32]);
    let r_point = match point_decompress(&r32, &c) {
        Some(p) => p,
        None => return false,
    };
    // S phải < L — "S being out of range" là từ chối (RFC §5.1.7 bước 1).
    let mut s32 = [0u8; 32];
    s32.copy_from_slice(&signature[32..64]);
    let s = scalar_from_bytes(&s32);
    if bn_cmp(&s, &SC_L) != core::cmp::Ordering::Less {
        return false;
    }

    let mut h = Sha512::new();
    h.update(&r32);
    h.update(public);
    h.update(msg);
    let d: Digest512 = h.finalize();
    let mut da = [0u8; 64];
    da.copy_from_slice(&d);
    let k = sc_reduce64(&da);

    let s_b = point_mul(&s, &base_point(&c), &c.d);
    let k_a = point_mul(&k, &a_point, &c.d);
    point_equal(&s_b, &point_add(&r_point, &k_a, &c.d))
}

/// So khớp không rò rỉ thời gian (dùng chung với sha256::ct_eq).
pub fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    crate::sha256::ct_eq(a, b)
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    fn unhex(s: &str) -> Vec<u8> {
        let b = s.as_bytes();
        let mut out = Vec::new();
        let mut i = 0;
        while i + 1 < b.len() {
            let hi = (b[i] as char).to_digit(16).unwrap() as u8;
            let lo = (b[i + 1] as char).to_digit(16).unwrap() as u8;
            out.push(hi * 16 + lo);
            i += 2;
        }
        out
    }

    fn seed_of(s: &str) -> [u8; 32] {
        let v = unhex(s);
        let mut out = [0u8; 32];
        out.copy_from_slice(&v);
        out
    }

    fn hex_of(data: &[u8]) -> Vec<u8> {
        data.to_vec()
    }

    #[test]
    fn rfc8032_test1_empty_message() {
        let seed = seed_of("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        let pubk = public_key(&seed);
        assert_eq!(
            hex_of(&pubk),
            unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        );
        let sig = sign(&seed, &[]);
        assert_eq!(
            hex_of(&sig),
            unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
        );
        assert!(verify(&pubk, &[], &sig));
    }

    #[test]
    fn rfc8032_test2_one_byte() {
        let seed = seed_of("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
        let pubk = public_key(&seed);
        assert_eq!(
            hex_of(&pubk),
            unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c")
        );
        let sig = sign(&seed, &[0x72]);
        assert_eq!(
            hex_of(&sig),
            unhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")
        );
        assert!(verify(&pubk, &[0x72], &sig));
    }

    #[test]
    fn rfc8032_test3_two_bytes() {
        let seed = seed_of("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7");
        let pubk = public_key(&seed);
        assert_eq!(
            hex_of(&pubk),
            unhex("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025")
        );
        let msg = unhex("af82");
        let sig = sign(&seed, &msg);
        assert_eq!(
            hex_of(&sig),
            unhex("6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a")
        );
        assert!(verify(&pubk, &msg, &sig));
    }

    #[test]
    fn rfc8032_test_sha_abc() {
        // Message = SHA-512("abc") — 64 byte.
        let seed = seed_of("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42");
        let pubk = public_key(&seed);
        assert_eq!(
            hex_of(&pubk),
            unhex("ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf")
        );
        let msg = unhex(
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        );
        let sig = sign(&seed, &msg);
        assert_eq!(
            hex_of(&sig),
            unhex("dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704")
        );
        assert!(verify(&pubk, &msg, &sig));
    }

    #[test]
    fn tampering_is_rejected() {
        let seed = seed_of("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        let pubk = public_key(&seed);
        let mut sig = sign(&seed, b"thong diep");
        assert!(verify(&pubk, b"thong diep", &sig));
        // Sửa 1 bit thông điệp, chữ ký, khóa — đều phải từ chối.
        assert!(!verify(&pubk, b"thong diep!", &sig));
        sig[0] ^= 1;
        assert!(!verify(&pubk, b"thong diep", &sig));
        sig[0] ^= 1;
        let mut bad_pub = pubk;
        bad_pub[3] ^= 0x40;
        assert!(!verify(&bad_pub, b"thong diep", &sig));
    }
}
