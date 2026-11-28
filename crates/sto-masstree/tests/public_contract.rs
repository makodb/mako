use sto_masstree::{Table, Value};
#[test]
fn native_table_and_values_are_shared_owners() {
    fn shared<T: Send + Sync + Clone>() {}
    shared::<Table>();
    shared::<Value>();
}
