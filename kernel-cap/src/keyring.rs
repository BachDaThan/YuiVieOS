//! Sovereign Keyring — kho khoá phát hành ngoại tuyến cho gói `.bundle`.
//! no_std + alloc, không phụ thuộc. Song song với C++ `yui::bundle::Keyring`
//! (`kernel-services/bundle.cpp`): mỗi khoá là một cặp (key_id, khoá công
//! khai Ed25519 32 byte); kho khoá là danh sách CHO PHÉP (allowlist) —
//! khoá lạ không có trong kho ⇒ gói cập nhật bị từ chối.

use alloc::collections::BTreeMap;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

/// Kho khoá phát hành. Trong nhân, dữ liệu này nằm trong phân vùng bất biến
/// `/etc/yui/trusted-keys/` (xem fsstore) — không bao giờ ghi từ user space.
#[derive(Default)]
pub struct Keyring {
    keys: BTreeMap<String, [u8; 32]>,
}

impl Keyring {
    pub fn new() -> Self {
        Keyring {
            keys: BTreeMap::new(),
        }
    }

    /// Thêm khoá. Trả về `false` nếu key_id đã tồn tại với khoá KHÁC —
    /// không bao giờ ghi đè khoá của người khác một cách âm thầm.
    pub fn add(&mut self, key_id: &str, public: [u8; 32]) -> bool {
        if key_id.is_empty() || key_id.len() > 128 {
            return false;
        }
        match self.keys.get(key_id) {
            Some(&existing) => existing == public,
            None => {
                self.keys.insert(key_id.to_string(), public);
                true
            }
        }
    }

    /// Gỡ khoá — chỉ thành công khi khoá đúng như đã lưu.
    pub fn remove(&mut self, key_id: &str, public: &[u8; 32]) -> bool {
        match self.keys.get(key_id) {
            Some(&existing) if &existing == public => {
                self.keys.remove(key_id);
                true
            }
            _ => false,
        }
    }

    pub fn contains(&self, key_id: &str) -> bool {
        self.keys.contains_key(key_id)
    }

    pub fn get(&self, key_id: &str) -> Option<[u8; 32]> {
        self.keys.get(key_id).copied()
    }

    pub fn ids(&self) -> Vec<String> {
        self.keys.keys().cloned().collect()
    }

    pub fn len(&self) -> usize {
        self.keys.len()
    }

    pub fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crud_roundtrip() {
        let mut kr = Keyring::new();
        assert!(kr.is_empty());
        let k1 = [7u8; 32];
        let k2 = [9u8; 32];
        assert!(kr.add("phat-hanh-2026", k1));
        assert!(kr.add("phat-hanh-thu", k2));
        assert_eq!(kr.len(), 2);
        assert!(kr.contains("phat-hanh-2026"));
        assert_eq!(kr.get("phat-hanh-2026"), Some(k1));
        assert_eq!(kr.get("khong-co"), None);
        let ids = kr.ids();
        assert_eq!(ids.len(), 2);
        assert_eq!(ids[0], "phat-hanh-2026");  // BTreeMap: sắp theo thứ tự từ
        assert!(kr.remove("phat-hanh-2026", &k1));
        assert!(!kr.contains("phat-hanh-2026"));
    }

    #[test]
    fn refuses_silent_overwrite_and_bad_removal() {
        let mut kr = Keyring::new();
        assert!(kr.add("k", [1u8; 32]));
        assert!(!kr.add("k", [2u8; 32]));   // khoá khác dưới cùng id: từ chối
        assert_eq!(kr.get("k"), Some([1u8; 32]));
        assert!(!kr.remove("k", &[2u8; 32]));  // không đúng khoá: không gỡ
        assert!(kr.contains("k"));
        assert!(!kr.add("", [0u8; 32]));  // id rỗng: từ chối
    }
}
