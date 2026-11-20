//! Tầng 1 trên bare metal: khởi tạo bảng năng lực và chứng minh ba tính chất
//! ngay trên máy thật (in ra cổng nối tiếp):
//!   1. cấp một quyền gốc,
//!   2. dẫn xuất quyền con **không thể** mở rộng quyền,
//!   3. thu hồi quyền cha làm quyền con chết theo.
//!
//! Không có bước nào là "giả lập": đây là cùng đoạn mã trong `kernel-cap` mà
//! Tầng 4 dùng để xác minh thẻ, chỉ khác là chạy ở ring 0.

use crate::console::print_str_no_newline;

use yui_cap::{
    rights_to_string, CapError, CapTable, Capability, RIGHT_FS_READ, RIGHT_FS_WRITE, RIGHT_GRAPH,
};

pub fn self_test() {
    println!("  Tang 1    : khoi tao bang nang luc (kernel-cap)");

    let mut table = CapTable::new();
    let root_rights = RIGHT_FS_READ | RIGHT_FS_WRITE | RIGHT_GRAPH;
    let root = match table.insert(Capability::new(1, root_rights, 0)) {
        Ok(slot) => slot,
        Err(err) => {
            println!("    LOI: khong cap duoc quyen goc: {:?}", err);
            return;
        }
    };
    print!("    quyen goc : slot {} [", root);
    print_str_no_newline(&rights_to_string(root_rights));
    println!("]");

    let child = match table.derive(root, RIGHT_FS_READ, 7) {
        Ok(slot) => slot,
        Err(err) => {
            println!("    LOI: dan xuat that bai: {:?}", err);
            return;
        }
    };
    print!("    dan xuat  : slot {} [", child);
    print_str_no_newline(&rights_to_string(RIGHT_FS_READ));
    println!("]");
    // Chuỗi/bool KHÔNG đi qua `{}`: trên aarch64 (MMU tắt) `Formatter::pad`
    // làm nhân dừng giữa dòng — lỗi này từng chặn nhân aarch64 ngay tại đây,
    // trước cả bước [4] màn hình chào (xem console.rs).
    print!("    kiem tra  : doc fs = ");
    print_str_no_newline(ok(table.check(child, RIGHT_FS_READ)));
    print!(", ghi fs = ");
    print_str_no_newline(ok(table.check(child, RIGHT_FS_WRITE)));
    println!();

    // Tính chất 2: con không thể tự mở rộng quyền.
    match table.derive(child, RIGHT_FS_WRITE, 9) {
        Err(CapError::InsufficientRights) => println!("    mo rong   : bi tu choi (dung)"),
        Err(other) => println!("    mo rong   : loi khac {:?}", other),
        Ok(_) => println!("    mo rong   : LOI NGHIEM TRONG — da mo rong duoc quyen"),
    }

    // Tính chất 3: thu hồi bắc cầu.
    let revoked = table.revoke(root);
    let child_alive = table.check(child, RIGHT_FS_READ).is_ok();
    print!("    thu hoi   : ");
    print_str_no_newline(yes_no(revoked));
    print!(" (con cua slot {} con song? ", child);
    print_str_no_newline(yes_no(child_alive));
    println!(")");
    println!("    con song  : {} slot", table.len());
}

fn yes_no(b: bool) -> &'static str {
    if b {
        "true"
    } else {
        "false"
    }
}

fn ok(result: Result<(), CapError>) -> &'static str {
    match result {
        Ok(()) => "duoc",
        Err(_) => "khong",
    }
}
