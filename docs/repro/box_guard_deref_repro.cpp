#include <rusty/box.hpp>
#include <rusty/option.hpp>
#include <rusty/mutex.hpp>

struct Chan { void close(); };

#if RUSTYCPP_RUST
struct R2 {
    m: rusty::Mutex<Option<rusty::Box<Chan>>>,
    o: Option<rusty::Box<Chan>>,
}

impl R2 {
    fn no_guard(&self) { (*self.o.as_ref().unwrap()).close(); }
    fn via_guard(&self) {
        let guard = self.m.lock().unwrap();
        if (*guard).is_some() {
            (*(*guard).as_ref().unwrap()).close();
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=repro2.1 version=1 rust_sha256=6ebcaf142c5372b06e74dc987c8ba1a97ff3259a11e22eee48dcaef0d39fc0cf*/
struct R2;

struct R2 {
    rusty::Mutex<rusty::Option<rusty::Box<Chan>>> m;
    rusty::Option<rusty::Box<Chan>> o;

    void no_guard() const;
    void via_guard() const;
};


void R2::no_guard() const {
    ((rusty::detail::deref_if_pointer_like(this->o.as_ref().unwrap()))).close();
}

void R2::via_guard() const {
    auto guard = this->m.lock().unwrap();
    if (((*guard)).is_some()) {
        ((((*guard)).as_ref().unwrap())).close();
    }
}
/*RUSTYCPP:GEN-END id=repro2.1*/
