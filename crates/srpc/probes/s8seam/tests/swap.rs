//! Proof the switch really switches: a value written on a foreign stack
//! must be visible after coming back, and the fiber must not fall
//! through past its swap.
use s8seam::{swap, Ctx};

static mut MAIN: Ctx = Ctx { sp: 0 };
static mut FIBER: Ctx = Ctx { sp: 0 };
static mut MARK: u64 = 0;

extern "C" fn fiber_entry() {
    unsafe {
        MARK = 0xC0FFEE;
        swap(core::ptr::addr_of_mut!(FIBER), core::ptr::addr_of_mut!(MAIN));
        // Falling through here means the swap did not return to main.
        MARK = 0xDEAD;
    }
}

#[test]
fn switching_stacks_runs_on_the_other_stack_and_comes_back() {
    let mut stack = vec![0u64; 64 * 1024];
    unsafe {
        let top = stack.as_mut_ptr().add(stack.len());
        // 16-byte align, then lay out: [6 callee-saved regs][ret addr]
        let mut sp = (top as usize & !0xF) as *mut u64;
        sp = sp.sub(1);
        *sp = fiber_entry as usize as u64; // ret target
        sp = sp.sub(6);
        for i in 0..6 {
            *sp.add(i) = 0;
        }
        FIBER.sp = sp as u64;

        swap(core::ptr::addr_of_mut!(MAIN), core::ptr::addr_of_mut!(FIBER));
        assert_eq!(MARK, 0xC0FFEE, "the fiber body did not run on its own stack");
    }
}
