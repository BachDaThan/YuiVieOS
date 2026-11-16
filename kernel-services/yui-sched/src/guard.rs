//! Guard chống bất thường nền — chính sách mở rộng cho yui-sched
//! (docs/SECURITY.md §11.1). no_std + alloc, an toàn tuyệt đối.
//!
//! Luồng: telemetry (tải CPU/GPU riêng từng tiến trình) → [`Guard`] →
//! quyết định hạ lớp ưu tiên + bóp xung → audit + hỏi người dùng qua yuid.
//! Bản C++ đối chiếu (`kernel-services/guard.cpp`) dùng chung ngưỡng và
//! quy tắc; hằng số CAP dưới đây là `RIGHT_COMPUTE_HEAVY` của kernel-cap.

/// Quyền được phép tính toán nặng kéo dài (CAP_COMPUTE_HEAVY).
pub const CAP_COMPUTE_HEAVY: u64 = 1 << 21;

/// Mẫu tải của một tiến trình tại một thời điểm.
#[derive(Clone, Copy, Debug)]
pub struct Sample {
    pub pid: u64,
    pub cpu_percent: f64,
    pub gpu_percent: f64,
    pub has_window: bool,
    pub caps: u64,
    pub now_ns: i64,
}

/// Ngưỡng phát hiện (mặc định khớp SECURITY.md: 70 % / 15 s / bóp 20 %).
#[derive(Clone, Copy, Debug)]
pub struct GuardPolicy {
    pub cpu_percent: f64,
    pub gpu_percent: f64,
    pub sustain_ns: i64,
    pub throttle_percent: u32,
}

impl Default for GuardPolicy {
    fn default() -> Self {
        GuardPolicy {
            cpu_percent: 70.0,
            gpu_percent: 70.0,
            sustain_ns: 15_000_000_000,
            throttle_percent: 20,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    Normal,
    Watching,
    Throttled,
}

#[derive(Clone, Copy, Debug)]
struct Track {
    over_since_ns: i64,
    alerted: bool,
}

/// Bộ Guard thuần chính sách: không I/O, không đồng hồ — thời gian do
/// telemetry cung cấp qua mẫu (kiểm thử được deterministically).
pub struct Guard {
    policy: GuardPolicy,
    tracks: alloc::collections::BTreeMap<u64, Track>,
}

impl Guard {
    pub fn new(policy: GuardPolicy) -> Self {
        Guard {
            policy,
            tracks: alloc::collections::BTreeMap::new(),
        }
    }

    pub fn policy(&self) -> &GuardPolicy {
        &self.policy
    }

    /// Nạp một mẫu. Trả `Some(throttle_percent)` khi lần đầu phát hiện
    /// (tiến trình cần bị hạ lớp + bóp xung); `None` nếu không có gì mới.
    pub fn observe(&mut self, s: &Sample) -> Option<u32> {
        let legit = s.has_window || (s.caps & CAP_COMPUTE_HEAVY) != 0;
        if legit {
            // Có cửa sổ hoặc quyền hợp lệ: không theo dõi, tháo bóp nếu lỡ.
            self.tracks.remove(&s.pid);
            return None;
        }
        let over = s.cpu_percent > self.policy.cpu_percent || s.gpu_percent > self.policy.gpu_percent;
        let t = self.tracks.entry(s.pid).or_insert(Track {
            over_since_ns: -1,
            alerted: false,
        });
        if !over {
            t.over_since_ns = -1;
            return None;
        }
        if t.over_since_ns < 0 {
            t.over_since_ns = s.now_ns;
        }
        if t.alerted || s.now_ns - t.over_since_ns < self.policy.sustain_ns {
            return None;
        }
        t.alerted = true;
        Some(self.policy.throttle_percent)
    }

    pub fn verdict(&self, pid: u64) -> Verdict {
        match self.tracks.get(&pid) {
            None => Verdict::Normal,
            Some(t) if t.alerted => Verdict::Throttled,
            Some(_) => Verdict::Watching,
        }
    }

    pub fn is_throttled(&self, pid: u64) -> bool {
        matches!(self.verdict(pid), Verdict::Throttled)
    }

    /// Người dùng chọn "để yên" / tiến trình kết thúc: quên trạng thái.
    pub fn clear(&mut self, pid: u64) -> bool {
        self.tracks.remove(&pid).is_some()
    }
}

impl Default for Guard {
    fn default() -> Self {
        Self::new(GuardPolicy::default())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(pid: u64, now_ns: i64, cpu: f64) -> Sample {
        Sample {
            pid,
            cpu_percent: cpu,
            gpu_percent: 0.0,
            has_window: false,
            caps: 0,
            now_ns,
        }
    }

    const SEC: i64 = 1_000_000_000;

    #[test]
    fn hidden_miner_detected_after_15s() {
        let mut g = Guard::default();
        assert_eq!(g.observe(&sample(4242, 0, 95.0)), None);
        assert_eq!(g.observe(&sample(4242, 14 * SEC, 95.0)), None);
        assert_eq!(g.observe(&sample(4242, 15 * SEC, 95.0)), Some(20));
        assert!(g.is_throttled(4242));
        assert_eq!(g.verdict(4242), Verdict::Throttled);
        // Đã cảnh báo: không cảnh báo lần hai cho cùng chuỗi vi phạm.
        assert_eq!(g.observe(&sample(4242, 30 * SEC, 95.0)), None);
    }

    #[test]
    fn sustained_load_resets_when_it_drops() {
        let mut g = Guard::default();
        assert_eq!(g.observe(&sample(7, 0, 95.0)), None);
        assert_eq!(g.observe(&sample(7, 14 * SEC, 95.0)), None);
        assert_eq!(g.observe(&sample(7, 15 * SEC, 10.0)), None);  // đứt chuỗi
        assert_eq!(g.observe(&sample(7, 29 * SEC, 95.0)), None);
        assert_eq!(g.observe(&sample(7, 30 * SEC, 95.0)), None);
        assert!(!g.is_throttled(7));
    }

    #[test]
    fn legit_processes_are_untouched() {
        let mut g = Guard::default();
        let mut w = sample(1, 0, 99.0);
        w.has_window = true;
        for t in [0, 10 * SEC, 30 * SEC] {
            w.now_ns = t;
            assert_eq!(g.observe(&w), None);
        }
        let mut c = sample(2, 0, 99.0);
        c.caps = CAP_COMPUTE_HEAVY;
        for t in [0, 10 * SEC, 30 * SEC] {
            c.now_ns = t;
            assert_eq!(g.observe(&c), None);
        }
        assert!(!g.is_throttled(1));
        assert!(!g.is_throttled(2));
    }

    #[test]
    fn clear_releases_state() {
        let mut g = Guard::default();
        assert_eq!(g.observe(&sample(9, 0, 92.0)), None);
        assert_eq!(g.observe(&sample(9, 15 * SEC, 92.0)), Some(20));
        assert!(g.clear(9));
        assert!(!g.is_throttled(9));
        assert_eq!(g.verdict(9), Verdict::Normal);
    }
}
