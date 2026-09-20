use sto_core::UniqueItemKeys;

fn escape(keys: &[()]) -> UniqueItemKeys<'static, ()> {
    UniqueItemKeys::try_new(keys).unwrap()
}

fn main() {}
