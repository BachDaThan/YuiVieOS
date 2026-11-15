//! Định dạng gói cập nhật `.bundle` ngoại tuyến — docs/SECURITY.md §6.
//! no_std + alloc. Song song với C++ `yui::bundle` (`kernel-services/bundle.cpp`).
//!
//! Kiểm tra theo đúng thứ tự của SECURITY.md: **giải mã → toàn vẹn → phân
//! tích cú pháp → dùng**. Chữ ký Ed25519 nằm trên **manifest dạng chuẩn**
//! (canonical), SHA-256 cho từng tệp trong manifest; khoá tra trong Keyring.

use alloc::collections::BTreeMap;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

use crate::ed25519;
use crate::keyring::Keyring;
use crate::sha256::sha256;

/// Một tệp trong manifest.
#[derive(Clone)]
pub struct FileEntry {
    pub path: String,
    pub mode: u32,
    pub sha256: [u8; 32],
}

/// Manifest của gói: khoá phát hành, phiên bản, danh sách tệp + hash.
#[derive(Clone)]
pub struct Manifest {
    pub key_id: String,
    pub version: String,
    pub files: Vec<FileEntry>,
}

/// Đường dẫn an toàn: tương đối, không "..", không "//" (như safe_relpath C++).
pub fn safe_relpath(p: &str) -> bool {
    if p.is_empty() || p.starts_with('/') || p.contains("//") {
        return false;
    }
    !p.split('/').any(|part| part == "..")
}

fn hex64(bytes: &[u8; 32]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(64);
    for &b in bytes.iter() {
        s.push(DIGITS[(b >> 4) as usize] as char);
        s.push(DIGITS[(b & 0xf) as usize] as char);
    }
    s
}

/// Chuẩn hoá JSON tối giản (đủ cho khoá/tên tệp ASCII + tiếng Việt UTF-8):
/// nháy kép và backslash được thoát, mọi dấu điều khiển bị từ chối.
fn json_str_ok(s: &str) -> bool {
    !s.chars().any(|c| (c as u32) < 0x20)
}

fn json_str(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            _ => out.push(c),
        }
    }
    out.push('"');
    out
}

/// Dạng chuẩn (canonical) của manifest — đây là thứ được KÝ:
/// `{"files":[{"mode":…,"path":…,"sha256":…},…],"key_id":…,"version":…}`
/// — tệp sắp theo path, thứ tự khoá cố định (files, key_id, version).
pub fn canonical(m: &Manifest) -> Option<Vec<u8>> {
    if m.key_id.is_empty() || m.version.is_empty() || m.files.is_empty() {
        return None;
    }
    if !json_str_ok(&m.key_id) || !json_str_ok(&m.version) {
        return None;
    }
    let mut files: Vec<&FileEntry> = m.files.iter().collect();
    files.sort_by(|a, b| a.path.cmp(&b.path));
    if files.windows(2).any(|w| w[0].path == w[1].path) {
        return None;  // path trùng: từ chối thay vì chọn bừa
    }
    let mut out = String::from("{\"files\":[");
    for (i, f) in files.iter().enumerate() {
        if !safe_relpath(&f.path) || !json_str_ok(&f.path) {
            return None;
        }
        if i > 0 {
            out.push(',');
        }
        out.push_str("{\"mode\":");
        out.push_str(&alloc::format!("{}", f.mode));
        out.push_str(",\"path\":");
        out.push_str(&json_str(&f.path));
        out.push_str(",\"sha256\":");
        out.push_str(&json_str(&hex64(&f.sha256)));
        out.push('}');
    }
    out.push_str("],\"key_id\":");
    out.push_str(&json_str(&m.key_id));
    out.push_str(",\"version\":");
    out.push_str(&json_str(&m.version));
    out.push('}');
    Some(out.into_bytes())
}

/// Kết quả kiểm chứng: Ok(()) hoặc lý do tiếng Việt.
pub type VerifyResult = Result<(), String>;

/// Kiểm chứng gói: khoá trong kho → chữ ký Ed25519 trên manifest chuẩn →
/// SHA-256 từng tệp. `payloads` là đường dẫn → nội dung tệp trong gói.
pub fn verify(
    keyring: &Keyring,
    manifest: &Manifest,
    signature: &[u8; 64],
    payloads: &BTreeMap<String, Vec<u8>>,
) -> VerifyResult {
    // 1. Phân tích cú pháp an toàn (đường dẫn, độ dài).
    for f in &manifest.files {
        if !safe_relpath(&f.path) {
            return Err(alloc::format!("đường dẫn không an toàn trong manifest: {}", f.path));
        }
    }
    // 2. Toàn vẹn: mỗi tệp phải khớp SHA-256 đã khai báo.
    for f in &manifest.files {
        match payloads.get(&f.path) {
            None => return Err(alloc::format!("thiếu tệp trong gói: {}", f.path)),
            Some(data) => {
                if sha256(data) != f.sha256 {
                    return Err(alloc::format!("sai SHA-256 của tệp: {}", f.path));
                }
            }
        }
    }
    // 3. Chữ ký: khoá phát hành phải nằm trong kho khoá.
    let public = match keyring.get(&manifest.key_id) {
        Some(k) => k,
        None => {
            return Err(alloc::format!(
                "từ chối gói: khoá phát hành \"{}\" không có trong kho",
                manifest.key_id
            ))
        }
    };
    // 4. Ký trên dạng chuẩn — xây lại từ các trường đã phân tích.
    let canon = match canonical(manifest) {
        Some(c) => c,
        None => return Err("manifest không dựng được dạng chuẩn".to_string()),
    };
    if !ed25519::verify(&public, &canon, signature) {
        return Err("chữ ký Ed25519 không hợp lệ — gói đã bị sửa hoặc không phải từ nhà phát hành".to_string());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn seed_of() -> [u8; 32] {
        let mut s = [0u8; 32];
        s.copy_from_slice(&crate::sha512::sha512(b"seed cho test bundle")[..32]);
        s
    }

    fn sample(seed: &[u8; 32]) -> (Manifest, BTreeMap<String, Vec<u8>>, [u8; 64]) {
        let payload = b"noi dung goi cap nhat".to_vec();
        let mut files_payloads = BTreeMap::new();
        files_payloads.insert("bin/app".to_string(), payload.clone());
        let mut m = Manifest {
            key_id: "phat-hanh-2026".to_string(),
            version: "0.6.1".to_string(),
            files: Vec::new(),
        };
        let mut e = FileEntry {
            path: "bin/app".to_string(),
            mode: 0o755,
            sha256: [0u8; 32],
        };
        e.sha256 = sha256(&payload);
        m.files.push(e);
        let canon = canonical(&m).unwrap();
        let sig = ed25519::sign(seed, &canon);
        (m, files_payloads, sig)
    }

    #[test]
    fn sign_verify_offline_ok() {
        let seed = seed_of();
        let public = ed25519::public_key(&seed);
        let mut kr = Keyring::new();
        assert!(kr.add("phat-hanh-2026", public));
        let (m, payloads, sig) = sample(&seed);
        assert_eq!(verify(&kr, &m, &sig, &payloads), Ok(()));
    }

    #[test]
    fn unknown_key_is_rejected() {
        let seed = seed_of();
        let kr = Keyring::new();  // kho rỗng: không có khoá tin cậy nào
        let (m, payloads, sig) = sample(&seed);
        let err = verify(&kr, &m, &sig, &payloads).unwrap_err();
        assert!(err.contains("không có trong kho"));
    }

    #[test]
    fn tampering_is_rejected() {
        let seed = seed_of();
        let public = ed25519::public_key(&seed);
        let mut kr = Keyring::new();
        kr.add("phat-hanh-2026", public);
        let (m, payloads, sig) = sample(&seed);

        // Sửa nội dung tệp → sai SHA-256.
        let mut p2 = payloads.clone();
        p2.insert("bin/app".to_string(), b"noi dung goi da bi sua".to_vec());
        assert!(verify(&kr, &m, &sig, &p2).is_err());

        // Sửa chữ ký → không hợp lệ.
        let mut sig2 = sig;
        sig2[0] ^= 1;
        assert!(verify(&kr, &m, &sig2, &payloads).is_err());

        // Đổi phiên bản sau khi ký (manifest không khớp chữ ký).
        let mut m2 = m.clone();
        m2.version = "9.9.9".to_string();
        assert!(verify(&kr, &m2, &sig, &payloads).is_err());

        // Đường dẫn "thoát" khỏi gốc: từ chối ngay ở phân tích cú pháp.
        let mut m3 = m.clone();
        m3.files[0].path = "../etc/passwd".to_string();
        assert!(verify(&kr, &m3, &sig, &payloads).is_err());
    }

    #[test]
    fn canonical_is_stable_and_sorted() {
        let mut m = Manifest {
            key_id: "k".to_string(),
            version: "1".to_string(),
            files: vec![
                FileEntry { path: "b.txt".to_string(), mode: 644, sha256: [2u8; 32] },
                FileEntry { path: "a.txt".to_string(), mode: 644, sha256: [1u8; 32] },
            ],
        };
        let c1 = canonical(&m).unwrap();
        m.files.swap(0, 1);  // thứ tự thêm khác đi…
        let c2 = canonical(&m).unwrap();
        assert_eq!(c1, c2);  // …nhưng dạng chuẩn không đổi (đã sort theo path)
        // path "a.txt" đứng trước "b.txt" trong dạng chuẩn
        let s = String::from_utf8(c1).unwrap();
        let ia = s.find("a.txt").unwrap();
        let ib = s.find("b.txt").unwrap();
        assert!(ia < ib);
    }
}
