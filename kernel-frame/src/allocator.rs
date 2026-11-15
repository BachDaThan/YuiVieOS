//! Bộ cấp phát bump — đủ cho pha khởi động (vài chục KiB), thay bằng bộ cấp
//! phát vùng (zone allocator) ở P2.3.
//!
//! Vì sao cần cấp phát ngay từ đầu: `yui-cap` dùng `alloc` (String cho chuỗi
//! quyền, Vec cho ngăn xếp thu hồi). Cấp phát tất định, không phân mảnh, không
//! khoá phức tạp — đúng thứ một khung khởi động cần.

use core::alloc::{GlobalAlloc, Layout};
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicUsize, Ordering};

/// 4 MiB tĩnh trong .bss: đủ cho profiler + bảng năng lực + log khởi động.
const HEAP_SIZE: usize = 4 * 1024 * 1024;

#[repr(align(16))]
struct Heap(UnsafeCell<[u8; HEAP_SIZE]>);

// An toàn: chỉ bộ cấp phát này chạm vào vùng nhớ, và nó cấp phát tuyến tính.
unsafe impl Sync for Heap {}

static HEAP: Heap = Heap(UnsafeCell::new([0; HEAP_SIZE]));
static NEXT: AtomicUsize = AtomicUsize::new(0);

pub struct BumpAllocator;

impl BumpAllocator {
    pub const fn new() -> Self {
        Self
    }
}

unsafe impl GlobalAlloc for BumpAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let base = HEAP.0.get() as *mut u8;
        let mut offset = NEXT.load(Ordering::Relaxed);
        loop {
            let align = if layout.align() == 0 { 1 } else { layout.align() };
            let aligned = (offset + align - 1) & !(align - 1);
            let end = match aligned.checked_add(layout.size()) {
                Some(end) if end <= HEAP_SIZE => end,
                _ => return core::ptr::null_mut(),
            };
            match NEXT.compare_exchange_weak(offset, end, Ordering::SeqCst, Ordering::Relaxed) {
                Ok(_) => return base.add(aligned),
                Err(actual) => offset = actual,
            }
        }
    }

    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: Layout) {
        // Bump allocator không thu hồi: pha khởi động chỉ cấp phát một lần.
    }
}

/// Số byte đã cấp phát — dùng để in ngân sách bộ nhớ lúc khởi động.
pub fn used_bytes() -> usize {
    NEXT.load(Ordering::Relaxed)
}
