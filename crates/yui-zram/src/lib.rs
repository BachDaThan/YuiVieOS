//! yui-zram — "Nén RAM tại chỗ ZRAM (Zstd)" of Tầng 2, with a pluggable codec.
//!
//! Two pieces:
//!   * [`lz`] — the YUI-LZ block codec. The wire format is byte-identical to the
//!     C++ implementation in `core/src/codec.cpp` (see docs/FORMAT-YUIP.md), so a
//!     page compressed by the kernel can be decompressed by tooling and back.
//!   * [`store`] — the page store that turns compression into a swap device:
//!     write/read pages, keep statistics, and fall back to storing a page
//!     verbatim when it does not compress.
//!
//! Zstd remains the optional backend for cold pages (`Codec::Zstd` is reserved
//! for the FFI build); the built-in codec exists so ZRAM works on a fresh system
//! with no external library at all.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

extern crate alloc;

pub mod lz;
pub mod store;

pub use lz::{compress, decompress, max_compressed_len};
pub use store::{Codec, PageStats, ZramStore};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_page_of_text() {
        let text = b"YuiVieOS la he dieu hanh Viet Nam. YuiVieOS la he dieu hanh Viet Nam. \
                     Tang 2 nen RAM tai cho de may yeu van chay muot."
            .repeat(4);
        let packed = compress(&text).expect("compression must succeed");
        assert!(packed.len() < text.len() / 3, "expected >3x, got {} -> {}", text.len(), packed.len());
        let back = decompress(&packed, text.len()).expect("decompression must succeed");
        assert_eq!(back, text);
    }

    #[test]
    fn roundtrip_random_bytes() {
        let mut state = 0x1234_5678u32;
        let data: alloc::vec::Vec<u8> = (0..4096)
            .map(|_| {
                state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                (state >> 24) as u8
            })
            .collect();
        let packed = compress(&data).unwrap();
        assert!(packed.len() <= max_compressed_len(data.len()));
        let back = decompress(&packed, data.len()).unwrap();
        assert_eq!(back, data);
    }

    #[test]
    fn roundtrip_edge_sizes() {
        for size in [0usize, 1, 2, 3, 4, 5, 7, 31, 32, 33, 63, 64, 65, 127] {
            let data: alloc::vec::Vec<u8> = (0..size).map(|i| b'a' + (i % 26) as u8).collect();
            let packed = compress(&data).unwrap();
            let back = decompress(&packed, size).unwrap();
            assert_eq!(back, data, "size {} failed", size);
        }
    }

    #[test]
    fn corrupted_streams_are_rejected() {
        let data = b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_vec();
        let packed = compress(&data).unwrap();
        // Truncation.
        assert!(decompress(&packed[..packed.len() - 1], data.len()).is_none());
        // A distance larger than the produced output.
        let mut bad = packed.clone();
        if bad.len() > 3 {
            let last = bad.len() - 1;
            bad[last] = 0xff;
            bad[last - 1] = 0xff;
        }
        // Either invalid or producing different bytes: never a silent success.
        if let Some(out) = decompress(&bad, data.len()) {
            assert_ne!(out, data);
        }
    }

    #[test]
    fn store_accounts_for_savings() {
        let mut store = ZramStore::new(64);
        let page = b"lap trinh he thong bang Rust an toan va nhanh. ".repeat(40);
        let saved = store.write(0, &page).expect("write must fit");
        assert!(saved > 0, "a repetitive page must save space");
        let back = store.read(0).expect("page must be readable");
        assert_eq!(back, page);
        let stats = store.stats();
        assert_eq!(stats.pages, 1);
        assert_eq!(stats.bytes_in, page.len() as u64);
        assert!(stats.bytes_stored < stats.bytes_in);
    }

    #[test]
    fn store_overwrites_and_removes() {
        let mut store = ZramStore::new(8);
        store.write(3, b"first version").unwrap();
        store.write(3, b"second version, longer").unwrap();
        assert_eq!(store.read(3).unwrap(), b"second version, longer".to_vec());
        assert_eq!(store.stats().pages, 1);
        assert!(store.remove(3));
        assert!(!store.remove(3));
        assert!(store.read(3).is_none());
        assert_eq!(store.stats().pages, 0);
    }

    #[test]
    fn store_rejects_tiny_capacity() {
        let mut store = ZramStore::new(1);
        assert!(store.write(0, b"hello").is_some());
        assert!(store.write(1, b"hello").is_none(), "capacity must be enforced");
        assert_eq!(store.read(1), None);
    }

    #[test]
    fn incompressible_page_is_stored_verbatim() {
        let mut store = ZramStore::new(8);
        let mut state = 42u32;
        let page: alloc::vec::Vec<u8> = (0..1024)
            .map(|_| {
                state = state.wrapping_mul(1_103_515_245).wrapping_add(12_345);
                (state >> 16) as u8
            })
            .collect();
        store.write(1, &page).unwrap();
        assert_eq!(store.read(1).unwrap(), page);
        assert_eq!(store.stats().verbatim_pages, 1);
    }
}
