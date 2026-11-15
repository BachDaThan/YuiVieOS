//! The ZRAM page store: compression plus accounting.

use alloc::collections::BTreeMap;
use alloc::vec::Vec;

use crate::lz;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Codec {
    /// Built-in YUI-LZ (always available).
    YuiLz,
    /// Page was incompressible and is held verbatim.
    Verbatim,
    /// Reserved for the optional Zstd FFI backend (cold pages).
    Zstd,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PageStats {
    pub pages: usize,
    pub bytes_in: u64,
    pub bytes_stored: u64,
    pub verbatim_pages: usize,
}

impl PageStats {
    /// Fraction of the original bytes that compression saved (0.0..=1.0).
    pub fn saved_ratio(&self) -> f64 {
        if self.bytes_in == 0 {
            return 0.0;
        }
        1.0 - (self.bytes_stored as f64 / self.bytes_in as f64)
    }
}

struct Page {
    codec: Codec,
    original_len: usize,
    data: Vec<u8>,
}

/// A compressed swap device in RAM. `capacity_pages` bounds memory use like the
/// real thing; when full, `write` reports failure so the caller can swap to disk.
pub struct ZramStore {
    capacity_pages: usize,
    pages: BTreeMap<u32, Page>,
}

impl ZramStore {
    pub fn new(capacity_pages: usize) -> Self {
        Self { capacity_pages, pages: BTreeMap::new() }
    }

    /// Stores a page. Returns the number of bytes saved (negative means the page
    /// ended up larger, which never happens because of the verbatim fallback).
    pub fn write(&mut self, index: u32, data: &[u8]) -> Option<i64> {
        let existing = self.pages.len();
        if !self.pages.contains_key(&index) && existing >= self.capacity_pages {
            return None;
        }
        let packed = lz::compress(data)?;
        let (codec, stored) = if packed.len() < data.len() {
            (Codec::YuiLz, packed)
        } else {
            (Codec::Verbatim, data.to_vec())
        };
        let saved = data.len() as i64 - stored.len() as i64;
        self.pages.insert(
            index,
            Page { codec, original_len: data.len(), data: stored },
        );
        Some(saved)
    }

    pub fn read(&self, index: u32) -> Option<Vec<u8>> {
        let page = self.pages.get(&index)?;
        match page.codec {
            Codec::Verbatim => Some(page.data.clone()),
            _ => lz::decompress(&page.data, page.original_len),
        }
    }

    pub fn remove(&mut self, index: u32) -> bool {
        self.pages.remove(&index).is_some()
    }

    pub fn codec_of(&self, index: u32) -> Option<Codec> {
        self.pages.get(&index).map(|p| p.codec)
    }

    pub fn stats(&self) -> PageStats {
        let mut stats = PageStats { pages: self.pages.len(), ..PageStats::default() };
        for page in self.pages.values() {
            stats.bytes_in += page.original_len as u64;
            stats.bytes_stored += page.data.len() as u64;
            if page.codec == Codec::Verbatim {
                stats.verbatim_pages += 1;
            }
        }
        stats
    }
}
