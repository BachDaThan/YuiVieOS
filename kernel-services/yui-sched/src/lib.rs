//! yui-sched — the scheduling *policy* of Tầng 2 ("Bộ lập lịch Chuyển đổi nóng",
//! inspired by sched_ext + LAVD).
//!
//! The frame kernel keeps the mechanism (switch, timers, per-CPU runqueues);
//! this crate is the policy: which task runs next, on which kind of core, and how
//! much virtual runtime it is charged. Because the policy is ordinary safe Rust
//! with no unsafe blocks, it can be hot-swapped at runtime and unit tested
//! without booting anything.
//!
//! Three rules, straight from the architecture diagram:
//!   * game / latency sensitive  -> P-core, small charge, wakeup preemption
//!   * AI / render (throughput)  -> P-cores pinned, longer slices
//!   * batch / background        -> E-cores, normal charge
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::cmp::Ordering;

pub type TaskId = u64;
pub type Ns = u64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CoreKind {
    /// Performance core (big).
    Performance,
    /// Efficiency core (LITTLE).
    Efficiency,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TaskClass {
    /// Games, audio, input handling: latency dominates.
    Interactive,
    /// Model inference / rendering: prefers P-cores, tolerates long slices.
    AiRender,
    /// Compiles, downloads, indexing: prefers E-cores.
    Batch,
}

impl TaskClass {
    /// Weight used for virtual-runtime accounting (bigger = runs more often).
    pub const fn weight(self) -> u64 {
        match self {
            TaskClass::Interactive => 3,
            TaskClass::AiRender => 2,
            TaskClass::Batch => 1,
        }
    }

    pub const fn prefers_performance_core(self) -> bool {
        !matches!(self, TaskClass::Batch)
    }

    /// Maximum slice in nanoseconds before the task is charged again.
    pub const fn slice_ns(self) -> Ns {
        match self {
            TaskClass::Interactive => 1_000_000,   // 1 ms
            TaskClass::AiRender => 8_000_000,      // 8 ms
            TaskClass::Batch => 4_000_000,         // 4 ms
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            TaskClass::Interactive => "interactive",
            TaskClass::AiRender => "ai-render",
            TaskClass::Batch => "batch",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Task {
    pub id: TaskId,
    pub class: TaskClass,
    pub vruntime: Ns,
    /// Set when the task woke from sleep; it gets one "latency credit" pick.
    pub wakeup_credit: bool,
    /// Affinity hint from the caller (0 = no hint).
    pub cpu_hint: u32,
}

impl Task {
    pub fn new(id: TaskId, class: TaskClass) -> Self {
        Self { id, class, vruntime: 0, wakeup_credit: false, cpu_hint: 0 }
    }

    pub fn with_vruntime(mut self, vruntime: Ns) -> Self {
        self.vruntime = vruntime;
        self
    }
}

#[derive(Clone, Debug)]
pub struct Policy {
    /// When true, batch work is kept off P-cores unless no E-core is available.
    pub strict_affinity: bool,
    /// How many nanoseconds of "credit" a woken interactive task gets.
    pub wakeup_boost_ns: Ns,
    /// Migrate a task after this many E-core picks (simple load balancing).
    pub migrate_after_picks: u32,
}

impl Default for Policy {
    fn default() -> Self {
        Self { strict_affinity: true, wakeup_boost_ns: 500_000, migrate_after_picks: 4 }
    }
}

#[derive(Debug, Default)]
pub struct Scheduler {
    runqueue: Vec<Task>,
    policy: Policy,
    picks: u64,
    /// min_vruntime of the queue, used to keep vruntime monotonic.
    min_vruntime: Ns,
    log: Vec<String>,
}

impl Scheduler {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn with_policy(policy: Policy) -> Self {
        Self { policy, ..Self::default() }
    }

    pub fn policy(&self) -> &Policy {
        &self.policy
    }

    pub fn set_policy(&mut self, policy: Policy) {
        // Hot-swap: only the policy changes, never the runqueue bookkeeping.
        self.policy = policy;
        self.log.push(String::from("policy-swapped"));
    }

    pub fn enqueue(&mut self, mut task: Task) {
        if task.vruntime < self.min_vruntime {
            task.vruntime = self.min_vruntime;
        }
        self.runqueue.push(task);
    }

    pub fn len(&self) -> usize {
        self.runqueue.len()
    }

    pub fn is_empty(&self) -> bool {
        self.runqueue.is_empty()
    }

    pub fn picks(&self) -> u64 {
        self.picks
    }

    /// Chooses the next task for a given core kind.  The chosen task stays in
    /// the runqueue: the caller charges it through [`Scheduler::charge`] once it
    /// has actually run, which is how the frame kernel drives this policy.
    ///
    /// Ordering rules, most significant first:
    ///   1. class affinity (batch belongs on E-cores, everything else prefers P)
    ///   2. wakeup credit (an interactive task that just woke up)
    ///   3. smallest effective vruntime
    ///   4. smallest id (deterministic for traces and tests)
    pub fn pick(&mut self, core: CoreKind) -> Option<TaskId> {
        if self.runqueue.is_empty() {
            return None;
        }
        let on_e_core = matches!(core, CoreKind::Efficiency);

        let mut best: Option<(u8, u8, Ns, TaskId)> = None;
        let mut best_index = 0usize;
        for (index, task) in self.runqueue.iter().enumerate() {
            let mut affinity_penalty = 0u8;
            if self.policy.strict_affinity {
                if task.class.prefers_performance_core() && on_e_core {
                    affinity_penalty = 1;
                } else if !task.class.prefers_performance_core() && !on_e_core {
                    affinity_penalty = 1;
                }
            }
            let interactive_credit = task.wakeup_credit && task.class == TaskClass::Interactive;
            let credit_penalty = if interactive_credit { 0 } else { 1 };
            let effective = if interactive_credit {
                task.vruntime.saturating_sub(self.policy.wakeup_boost_ns)
            } else {
                task.vruntime
            };
            let key = (affinity_penalty, credit_penalty, effective, task.id);
            match best {
                None => {
                    best = Some(key);
                    best_index = index;
                }
                Some(current) => {
                    if compare(key, current) == Ordering::Less {
                        best = Some(key);
                        best_index = index;
                    }
                }
            }
        }

        self.picks += 1;
        if let Some(task) = self.runqueue.get_mut(best_index) {
            task.wakeup_credit = false;
            self.min_vruntime = self.min_vruntime.max(task.vruntime);
            Some(task.id)
        } else {
            None
        }
    }

    /// Charges a task for the time it really ran and returns the weighted charge.
    pub fn charge(&mut self, id: TaskId, delta_ns: Ns) -> Option<Ns> {
        let task = self.runqueue.iter_mut().find(|t| t.id == id)?;
        let charged = delta_ns / task.class.weight().max(1);
        task.vruntime = task.vruntime.saturating_add(charged);
        Some(charged)
    }

    /// Removes a task that blocked or exited.
    pub fn dequeue(&mut self, id: TaskId) -> bool {
        let before = self.runqueue.len();
        self.runqueue.retain(|t| t.id != id);
        before != self.runqueue.len()
    }

    /// Marks a sleeping task as woken so it gets one latency-credit pick.
    pub fn wake(&mut self, id: TaskId) -> bool {
        if let Some(task) = self.runqueue.iter_mut().find(|t| t.id == id) {
            task.wakeup_credit = true;
            return true;
        }
        false
    }

    pub fn set_class(&mut self, id: TaskId, class: TaskClass) -> bool {
        if let Some(task) = self.runqueue.iter_mut().find(|t| t.id == id) {
            task.class = class;
            return true;
        }
        false
    }

    pub fn min_vruntime(&self) -> Ns {
        self.min_vruntime
    }

    pub fn log(&self) -> &[String] {
        &self.log
    }

    /// Adjusts slicing to the elastic k/W knob: a smaller window means shorter
    /// slices so interactive latency survives on weak machines.
    pub fn scale_slice(&self, class: TaskClass, window: u32) -> Ns {
        let base = class.slice_ns();
        // 1024 -> 1/2, 4096 -> 1x, 8192 -> 2x
        match window {
            0..=1024 => base / 2,
            1025..=4096 => base,
            _ => base.saturating_mul(2),
        }
    }
}

fn compare(a: (u8, u8, Ns, TaskId), b: (u8, u8, Ns, TaskId)) -> Ordering {
    match a.0.cmp(&b.0) {
        Ordering::Equal => match a.1.cmp(&b.1) {
            Ordering::Equal => match a.2.cmp(&b.2) {
                Ordering::Equal => a.3.cmp(&b.3),
                other => other,
            },
            other => other,
        },
        other => other,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn weights_and_slices_reflect_classes() {
        assert!(TaskClass::Interactive.weight() > TaskClass::Batch.weight());
        assert!(TaskClass::Interactive.slice_ns() < TaskClass::AiRender.slice_ns());
        assert!(TaskClass::Interactive.prefers_performance_core());
        assert!(!TaskClass::Batch.prefers_performance_core());
        assert_eq!(TaskClass::AiRender.name(), "ai-render");
    }

    #[test]
    fn picks_smallest_vruntime() {
        let mut sched = Scheduler::new();
        sched.enqueue(Task::new(1, TaskClass::Interactive).with_vruntime(500));
        sched.enqueue(Task::new(2, TaskClass::Interactive).with_vruntime(100));
        sched.enqueue(Task::new(3, TaskClass::Interactive).with_vruntime(300));

        assert_eq!(sched.pick(CoreKind::Performance), Some(2)); // 100 là nhỏ nhất

        // charge() TRẢ VỀ phần bị tính (delta / trọng số) và CỘNG vào vruntime;
        // nó không đặt lại vruntime. Interactive có trọng số 3.
        assert_eq!(sched.charge(2, 300), Some(100)); // 100 -> 200
        assert_eq!(sched.pick(CoreKind::Performance), Some(2)); // 200 vẫn nhỏ nhất

        assert_eq!(sched.charge(2, 900), Some(300)); // 200 -> 500
        assert_eq!(sched.pick(CoreKind::Performance), Some(3)); // 300 < 500
        assert_eq!(sched.charge(3, 900), Some(300)); // 300 -> 600

        // task 1 và task 2 cùng vruntime 500 ⇒ id nhỏ hơn thắng (tất định).
        assert_eq!(sched.pick(CoreKind::Performance), Some(1));
        assert_eq!(sched.charge(1, 600), Some(200)); // 500 -> 700
        assert_eq!(sched.pick(CoreKind::Performance), Some(2)); // 500 < 600 < 700

        assert_eq!(sched.picks(), 5);
        assert_eq!(sched.len(), 3);
    }

    #[test]
    fn batch_prefers_efficiency_cores() {
        let mut sched = Scheduler::new();
        sched.enqueue(Task::new(1, TaskClass::Interactive).with_vruntime(10));
        sched.enqueue(Task::new(2, TaskClass::Batch).with_vruntime(5));
        // On an E-core the class affinity beats vruntime.
        assert_eq!(sched.pick(CoreKind::Efficiency), Some(2));

        let mut sched2 = Scheduler::new();
        sched2.enqueue(Task::new(1, TaskClass::Interactive).with_vruntime(10));
        sched2.enqueue(Task::new(2, TaskClass::Batch).with_vruntime(5));
        // On a P-core the interactive task wins despite the bigger vruntime.
        assert_eq!(sched2.pick(CoreKind::Performance), Some(1));

        // With strict affinity off, the E-core simply takes the smallest vruntime.
        let mut sched3 = Scheduler::with_policy(Policy { strict_affinity: false, ..Policy::default() });
        sched3.enqueue(Task::new(1, TaskClass::Interactive).with_vruntime(10));
        sched3.enqueue(Task::new(2, TaskClass::Batch).with_vruntime(5));
        assert_eq!(sched3.pick(CoreKind::Efficiency), Some(2));
    }

    #[test]
    fn wakeup_credit_lets_interactive_task_jump_the_queue() {
        let mut sched = Scheduler::new();
        sched.enqueue(Task::new(1, TaskClass::Interactive).with_vruntime(10_000));
        sched.enqueue(Task::new(2, TaskClass::Interactive).with_vruntime(1_000));
        assert!(sched.wake(1));
        // credit = 500_000 ns, so task 1's effective vruntime drops far below task 2's.
        assert_eq!(sched.pick(CoreKind::Performance), Some(1));
        // The credit is consumed by the pick.
        assert_eq!(sched.pick(CoreKind::Performance), Some(2));
        assert!(!sched.wake(999));
    }

    #[test]
    fn charge_is_weight_normalised() {
        let mut sched = Scheduler::new();
        sched.enqueue(Task::new(1, TaskClass::Interactive));
        sched.enqueue(Task::new(2, TaskClass::Batch));
        assert_eq!(sched.charge(1, 3_000_000), Some(1_000_000));
        assert_eq!(sched.charge(2, 3_000_000), Some(3_000_000));
        let batch = sched.runqueue.iter().find(|t| t.id == 2).unwrap();
        assert_eq!(batch.vruntime, 3_000_000);
        assert_eq!(sched.charge(99, 1_000), None);
    }

    #[test]
    fn fairness_over_many_picks() {
        let mut sched = Scheduler::new();
        for id in 1..=4 {
            sched.enqueue(Task::new(id, TaskClass::Interactive));
        }
        let mut counts = [0usize; 5];
        for _ in 0..400 {
            if let Some(id) = sched.pick(CoreKind::Performance) {
                counts[id as usize] += 1;
                sched.charge(id, TaskClass::Interactive.slice_ns());
            }
        }
        for id in 1..=4 {
            assert!(counts[id] >= 80, "task {} starved: {}", id, counts[id]);
            assert!(counts[id] <= 120, "task {} hogged the CPU: {}", id, counts[id]);
        }
    }

    #[test]
    fn hot_swap_policy_keeps_bookkeeping() {
        let mut sched = Scheduler::new();
        sched.enqueue(Task::new(7, TaskClass::Batch).with_vruntime(100));
        let mut policy = sched.policy().clone();
        policy.strict_affinity = false;
        sched.set_policy(policy);
        assert_eq!(sched.len(), 1);
        assert!(!sched.policy().strict_affinity);
        assert_eq!(sched.pick(CoreKind::Efficiency), Some(7));
        assert_eq!(sched.log().len(), 1);
        assert_eq!(sched.log()[0], "policy-swapped");
        assert!(sched.min_vruntime() >= 100);
    }

    #[test]
    fn slice_scales_with_window() {
        let sched = Scheduler::new();
        let small = sched.scale_slice(TaskClass::AiRender, 1024);
        let normal = sched.scale_slice(TaskClass::AiRender, 4096);
        let large = sched.scale_slice(TaskClass::AiRender, 8192);
        assert!(small < normal);
        assert!(normal < large);
        assert_eq!(small, TaskClass::AiRender.slice_ns() / 2);
    }

    #[test]
    fn dequeue_and_class_change() {
        let mut sched = Scheduler::new();
        sched.enqueue(Task::new(1, TaskClass::Batch));
        assert!(sched.set_class(1, TaskClass::Interactive));
        assert!(!sched.set_class(2, TaskClass::Interactive));
        assert_eq!(sched.charge(1, 3_000_000), Some(1_000_000));
        assert!(sched.dequeue(1));
        assert!(!sched.dequeue(1));
        assert!(sched.is_empty());
    }
}
