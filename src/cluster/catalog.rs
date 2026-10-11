//! Bootstrap-only canonical identities. Physical engine slots are not table IDs.
use vstd::prelude::*;
use crate::bytes::{compare, copy_bytes};
use crate::types::Status;

verus! {
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TableKind { Governed = 0, Replicated = 1, Static = 2 }
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Coordinates { Raw, Warehouse }
pub struct Table {
    pub id: u64,
    pub name: Vec<u8>,
    pub kind: TableKind,
    pub coordinates: Coordinates,
    pub initial_owner: u32,
}
pub struct Catalog {
    entries: Vec<Table>,
    sealed: bool,
}
impl Catalog {
    pub fn new() -> Self { Self { entries: Vec::new(), sealed: false } }
    pub fn len(&self) -> usize { self.entries.len() }
    pub fn at(&self, index: usize) -> Option<&Table> {
        if index < self.entries.len() { Some(&self.entries[index]) } else { None }
    }
    pub fn by_id(&self, id: u64) -> Option<&Table> {
        let mut i = 0usize;
        while i < self.entries.len()
            invariant i <= self.entries.len(),
            decreases self.entries.len() - i,
        {
            if self.entries[i].id == id { return Some(&self.entries[i]); }
            i += 1;
        }
        None
    }
    pub fn by_name(&self, name: &[u8]) -> Option<&Table> {
        let mut i = 0usize;
        while i < self.entries.len()
            invariant i <= self.entries.len(),
            decreases self.entries.len() - i,
        {
            if compare(&self.entries[i].name,name) == 0 { return Some(&self.entries[i]); }
            i += 1;
        }
        None
    }
    pub fn register(&mut self, id: u64, name: &[u8], kind: TableKind,
                    coordinates: Coordinates, initial_owner: u32) -> Status {
        if id == 0 || name.len() == 0 { return Status::Invalid; }
        let mut i = 0usize;
        while i < self.entries.len()
            invariant i <= self.entries.len(),
            decreases self.entries.len() - i,
        {
            let entry = &self.entries[i];
            let same_name = compare(&entry.name,name) == 0;
            if entry.id == id || same_name {
                return if entry.id == id && same_name && entry.kind == kind
                    && entry.coordinates == coordinates && entry.initial_owner == initial_owner
                    { Status::Ok } else { Status::Invalid };
            }
            i += 1;
        }
        if self.sealed { return Status::Busy; }
        self.entries.push(Table { id, name: copy_bytes(name), kind, coordinates, initial_owner });
        Status::Ok
    }
    pub fn seal(&mut self) { self.sealed = true; }
    pub fn tpcc(micro: bool) -> Result<Self,Status> {
        let mut catalog = Self::new();
        let mut i = 0usize;
        while i < 12
            invariant i <= 12,
            decreases 12 - i,
        {
            let name: &str = match i {
                0 => "customer", 1 => "customer_name_idx", 2 => "district",
                3 => "history", 4 => "new_order", 5 => "oorder",
                6 => "oorder_c_id_idx", 7 => "order_line", 8 => "stock",
                9 => "stock_data", 10 => "warehouse", _ => "item",
            };
            let kind = if i < 11 { TableKind::Governed }
                else if micro { TableKind::Static } else { TableKind::Replicated };
            let coordinates = if i < 11 { Coordinates::Warehouse } else { Coordinates::Raw };
            let status = catalog.register(i as u64 + 1,name.as_bytes(),kind,coordinates,0);
            match status { Status::Ok => {}, _ => return Err(status) }
            i += 1;
        }
        Ok(catalog)
    }
}

pub fn warehouse_coordinate(warehouse: u32) -> [u8;4] {
    [(warehouse >> 24) as u8, (warehouse >> 16) as u8,
     (warehouse >> 8) as u8, warehouse as u8]
}
} // verus!
