//! Tầng 1 trên bare metal: khởi tạo bảng năng lực và chứng minh ba tính chất
//! ngay trên máy thật (in ra cổng nối tiếp):
//!   1. cấp một quyền gốc,
//!   2. dẫn xuất quyền con **không thể** mở rộng quyền,
//!   3. thu hồi quyền cha làm quyền con chết theo.
//!
//! Không có bước nào là "giả lập": đây là cùng đoạn mã trong `kernel-cap` mà
//! Tầng 4 dùng để xác minh thẻ, chỉ khác là chạy ở ring 0.

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
    println!("    quyen goc : slot {} [{}]", root, rights_to_string(root_rights));

    let child = match table.derive(root, RIGHT_FS_READ, 7) {
        Ok(slot) => slot,
        Err(err) => {
            println!("    LOI: dan xuat that bai: {:?}", err);
            return;
        }
    };
    println!("    dan xuat  : slot {} [{}]", child, rights_to_string(RIGHT_FS_READ));
    println!(
        "    kiem tra  : doc fs = {}, ghi fs = {}",
        ok(table.check(child, RIGHT_FS_READ)),
        ok(table.check(child, RIGHT_FS_WRITE))
    );

    // Tính chất 2: con không thể tự mở rộng quyền.
    match table.derive(child, RIGHT_FS_WRITE, 9) {
        Err(CapError::InsufficientRights) => println!("    mo rong   : bi tu choi (dung)"),
        Err(other) => println!("    mo rong   : loi khac {:?}", other),
        Ok(_) => println!("    mo rong   : LOI NGHIEM TRONG — da mo rong duoc quyen"),
    }

    // Tính chất 3: thu hồi bắc cầu.
    let revoked = table.revoke(root);
    let child_alive = table.check(child, RIGHT_FS_READ).is_ok();
    println!(
        "    thu hoi   : {} (con cua slot {} con song? {})",
        revoked, child, child_alive
    );
    println!("    con song  : {} slot", table.len());
}

fn ok(result: Result<(), CapError>) -> &'static str {
    match result {
        Ok(()) => "duoc",
        Err(_) => "khong",
    }
}
