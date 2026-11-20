//! Nhãn nguồn sự kiện nhập (P4.13, mở rộng P2.21) — bản phản chiếu của
//! `yui::sandbox::InputSourceKind` / `InputPrivilege` trong
//! `include/yui/sandbox.hpp`. Thứ tự và giá trị số phải khớp từng biến thể;
//! `scripts/verify.py` so hai bên.
//!
//! QUY TẮC CỨNG: chỉ `PhysicalHardware` (driver gốc của host gắn thẻ, mã thiết
//! bị khác 0) mới tính là xác nhận tin cậy. `RemoteAuthorized` (phiên từ xa đã
//! cấp thẻ) và `GuestBluetooth` (bàn phím BT qua ngăn xếp trong khách) điều
//! khiển được ứng dụng thường nhưng không bao giờ bấm được hộp thoại xác nhận,
//! cấp thẻ giữ tỉnh, mở cổng hay cài khoá ký.

/// Nguồn của một sự kiện nhập.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum InputSourceKind {
    PhysicalHardware = 0,
    SyntheticApp = 1,
    RemoteAuthorized = 2,
    GuestBluetooth = 3,
}

impl InputSourceKind {
    /// Giải mã từ số trên dây; giá trị lạ trả `None` (không đoán).
    pub fn from_u8(v: u8) -> Option<InputSourceKind> {
        match v {
            0 => Some(InputSourceKind::PhysicalHardware),
            1 => Some(InputSourceKind::SyntheticApp),
            2 => Some(InputSourceKind::RemoteAuthorized),
            3 => Some(InputSourceKind::GuestBluetooth),
            _ => None,
        }
    }
}

/// Mức quyền một sự kiện mang theo.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum InputPrivilege {
    None,
    AppControl,
    TrustedConfirmation,
}

/// Sự kiện đã gắn nhãn (phần dùng để quyết định quyền).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TaggedSource {
    pub source: InputSourceKind,
    pub hardware_device_id: u64,
    pub remote_session_id: u64,
}

/// Cùng quy tắc với `yui::sandbox::input_privilege`.
pub fn input_privilege(e: &TaggedSource) -> InputPrivilege {
    match e.source {
        InputSourceKind::PhysicalHardware => {
            if e.hardware_device_id != 0 {
                InputPrivilege::TrustedConfirmation
            } else {
                InputPrivilege::None
            }
        }
        InputSourceKind::RemoteAuthorized => {
            if e.remote_session_id != 0 {
                InputPrivilege::AppControl
            } else {
                InputPrivilege::None
            }
        }
        InputSourceKind::GuestBluetooth => InputPrivilege::AppControl,
        InputSourceKind::SyntheticApp => InputPrivilege::None,
    }
}

pub fn counts_as_trusted_confirmation(e: &TaggedSource) -> bool {
    input_privilege(e) == InputPrivilege::TrustedConfirmation
}

pub fn may_drive_application(e: &TaggedSource) -> bool {
    input_privilege(e) != InputPrivilege::None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ev(source: InputSourceKind, hw: u64, remote: u64) -> TaggedSource {
        TaggedSource {
            source,
            hardware_device_id: hw,
            remote_session_id: remote,
        }
    }

    #[test]
    fn only_physical_hardware_confirms() {
        assert!(counts_as_trusted_confirmation(&ev(InputSourceKind::PhysicalHardware, 7, 0)));
        assert!(!counts_as_trusted_confirmation(&ev(InputSourceKind::PhysicalHardware, 0, 0)));
        assert!(!counts_as_trusted_confirmation(&ev(InputSourceKind::SyntheticApp, 7, 0)));
        assert!(!counts_as_trusted_confirmation(&ev(InputSourceKind::RemoteAuthorized, 7, 9)));
        assert!(!counts_as_trusted_confirmation(&ev(InputSourceKind::GuestBluetooth, 7, 0)));
    }

    #[test]
    fn remote_and_bluetooth_drive_apps_only() {
        let remote = ev(InputSourceKind::RemoteAuthorized, 0, 9);
        assert_eq!(input_privilege(&remote), InputPrivilege::AppControl);
        let unbound = ev(InputSourceKind::RemoteAuthorized, 0, 0);
        assert_eq!(input_privilege(&unbound), InputPrivilege::None);
        let bt = ev(InputSourceKind::GuestBluetooth, 0, 0);
        assert_eq!(input_privilege(&bt), InputPrivilege::AppControl);
        assert!(!may_drive_application(&ev(InputSourceKind::SyntheticApp, 0, 0)));
    }

    #[test]
    fn wire_values_round_trip_and_unknown_is_rejected() {
        for v in 0u8..4 {
            assert_eq!(InputSourceKind::from_u8(v).map(|k| k as u8), Some(v));
        }
        assert_eq!(InputSourceKind::from_u8(4), None);
        assert_eq!(InputSourceKind::from_u8(255), None);
    }
}
