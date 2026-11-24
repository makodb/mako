//! The existing repository's TPC-C byte layouts: big-endian fixed keys,
//! zigzag varints for i32 values, and padded inline strings. These remain
//! compatible with the retained Rust fused transaction kernels.
use crate::wire::{Error, ErrorCode, Result};
fn invalid(message: &str) -> Error {
    Error::new(ErrorCode::Invalid, message)
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Fixed<const N: usize>(pub [u8; N]);
impl<const N: usize> Fixed<N> {
    pub fn new(bytes: &[u8]) -> Result<Self> {
        if bytes.len() > N {
            return Err(invalid("fixed string too long"));
        }
        let mut value = [0; N];
        value[..bytes.len()].copy_from_slice(bytes);
        Ok(Self(value))
    }
}
impl<const N: usize> Default for Fixed<N> {
    fn default() -> Self {
        Self([0; N])
    }
}
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Inline<const N: usize>(pub Vec<u8>);
impl<const N: usize> Inline<N> {
    pub fn new(bytes: &[u8]) -> Result<Self> {
        if bytes.len() > N {
            return Err(invalid("inline string too long"));
        }
        Ok(Self(bytes.to_vec()))
    }
}
pub struct Reader<'a>(&'a [u8]);
impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        if n > self.0.len() {
            return Err(invalid("truncated TPC-C value"));
        }
        let (head, tail) = self.0.split_at(n);
        self.0 = tail;
        Ok(head)
    }
    fn varint(&mut self) -> Result<u32> {
        let mut value = 0;
        for shift in (0..35).step_by(7) {
            let byte = self.take(1)?[0];
            if shift == 28 && byte > 15 {
                return Err(invalid("TPC-C varint overflow"));
            }
            value |= u32::from(byte & 127) << shift;
            if byte & 128 == 0 {
                if shift != 0 && byte == 0 {
                    return Err(invalid("noncanonical TPC-C varint"));
                }
                return Ok(value);
            }
        }
        Err(invalid("TPC-C varint overflow"))
    }
}
fn varint(mut value: u32, out: &mut Vec<u8>) {
    while value >= 128 {
        out.push((value as u8 & 127) | 128);
        value >>= 7;
    }
    out.push(value as u8);
}
trait Field: Sized {
    fn read(input: &mut Reader<'_>) -> Result<Self>;
    fn write(&self, out: &mut Vec<u8>) -> Result<()>;
}
impl Field for u32 {
    fn read(input: &mut Reader<'_>) -> Result<Self> {
        input.varint()
    }
    fn write(&self, out: &mut Vec<u8>) -> Result<()> {
        varint(*self, out);
        Ok(())
    }
}
impl Field for i32 {
    fn read(input: &mut Reader<'_>) -> Result<Self> {
        let value = input.varint()?;
        Ok(((value >> 1) as i32) ^ -((value & 1) as i32))
    }
    fn write(&self, out: &mut Vec<u8>) -> Result<()> {
        varint(((*self as u32) << 1) ^ ((*self >> 31) as u32), out);
        Ok(())
    }
}
impl Field for i16 {
    fn read(input: &mut Reader<'_>) -> Result<Self> {
        Ok(Self::from_le_bytes(input.take(2)?.try_into().unwrap()))
    }
    fn write(&self, out: &mut Vec<u8>) -> Result<()> {
        out.extend_from_slice(&self.to_le_bytes());
        Ok(())
    }
}
impl Field for f32 {
    fn read(input: &mut Reader<'_>) -> Result<Self> {
        let value = Self::from_le_bytes(input.take(4)?.try_into().unwrap());
        if value.is_finite() {
            Ok(value)
        } else {
            Err(invalid("nonfinite TPC-C amount"))
        }
    }
    fn write(&self, out: &mut Vec<u8>) -> Result<()> {
        if !self.is_finite() {
            return Err(invalid("nonfinite TPC-C amount"));
        }
        out.extend_from_slice(&self.to_le_bytes());
        Ok(())
    }
}
impl Field for u8 {
    fn read(input: &mut Reader<'_>) -> Result<Self> {
        Ok(input.take(1)?[0])
    }
    fn write(&self, out: &mut Vec<u8>) -> Result<()> {
        out.push(*self);
        Ok(())
    }
}
impl Field for bool {
    fn read(input: &mut Reader<'_>) -> Result<Self> {
        match input.take(1)?[0] {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(invalid("TPC-C boolean")),
        }
    }
    fn write(&self, out: &mut Vec<u8>) -> Result<()> {
        out.push(u8::from(*self));
        Ok(())
    }
}
impl<const N: usize> Field for Fixed<N> {
    fn read(input: &mut Reader<'_>) -> Result<Self> {
        Ok(Self(input.take(N)?.try_into().unwrap()))
    }
    fn write(&self, out: &mut Vec<u8>) -> Result<()> {
        out.extend_from_slice(&self.0);
        Ok(())
    }
}
impl<const N: usize> Field for Inline<N> {
    fn read(input: &mut Reader<'_>) -> Result<Self> {
        let length = if N <= 255 {
            usize::from(input.take(1)?[0])
        } else {
            usize::from(u16::from_le_bytes(input.take(2)?.try_into().unwrap()))
        };
        if length > N {
            return Err(invalid("TPC-C inline string length"));
        }
        let storage = input.take(N + 1)?;
        Ok(Self(storage[..length].to_vec()))
    }
    fn write(&self, out: &mut Vec<u8>) -> Result<()> {
        if self.0.len() > N || N > u16::MAX as usize {
            return Err(invalid("TPC-C inline string length"));
        }
        if N <= 255 {
            out.push(self.0.len() as u8);
        } else {
            out.extend_from_slice(&(self.0.len() as u16).to_le_bytes());
        }
        out.extend_from_slice(&self.0);
        out.resize(out.len() + N + 1 - self.0.len(), 0);
        Ok(())
    }
}
pub trait Record: Sized {
    fn encode(&self) -> Result<Vec<u8>>;
    fn decode(bytes: &[u8]) -> Result<Self>;
}
macro_rules! record {
    ($name:ident { $($field:ident : $ty:ty),* $(,)? }) => {
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct $name { $(pub $field: $ty,)* }
        impl Record for $name {
            fn encode(&self) -> Result<Vec<u8>> {
                let mut bytes = Vec::new(); $(self.$field.write(&mut bytes)?;)* Ok(bytes)
            }
            fn decode(bytes: &[u8]) -> Result<Self> {
                let mut input = Reader(bytes); let value = Self { $($field: <$ty as Field>::read(&mut input)?,)* };
                if !input.0.is_empty() { return Err(invalid("trailing TPC-C value bytes")); } Ok(value)
            }
        }
    }
}
record!(Warehouse { ytd: f32, tax: f32, name: Inline<10>, street1: Inline<20>, street2: Inline<20>, city: Inline<20>, state: Fixed<2>, zip: Fixed<9> });
record!(District { ytd: f32, tax: f32, next_order: i32, name: Inline<10>, street1: Inline<20>, street2: Inline<20>, city: Inline<20>, state: Fixed<2>, zip: Fixed<9> });
record!(Customer { discount: f32, credit: Fixed<2>, last: Inline<16>, first: Inline<16>, credit_limit: f32, balance: f32, ytd_payment: f32, payment_count: i32, delivery_count: i32, street1: Inline<20>, street2: Inline<20>, city: Inline<20>, state: Fixed<2>, zip: Fixed<9>, phone: Fixed<16>, since: u32, middle: Fixed<2> });
record!(CustomerData { data: Inline<300> });
record!(Balance { balance: f32 });
record!(CustomerName { customer: i32 });
record!(History { amount: f32, data: Inline<24> });
record!(Item { name: Inline<24>, price: f32, data: Inline<50>, image: i32 });
record!(MicroItem { name: Inline<8> });
record!(NewOrder { dummy: Fixed<12> });
record!(Order {
    customer: i32,
    carrier: i32,
    lines: u8,
    all_local: bool,
    date: u32
});
record!(OrderIndex { dummy: u8 });
record!(OrderLine {
    item: i32,
    delivery: u32,
    amount: f32,
    supply: i32,
    quantity: u8
});
record!(Stock {
    quantity: i16,
    ytd: f32,
    orders: i32,
    remote: i32
});
record!(StockData { data: Inline<50>, district1: Fixed<24>, district2: Fixed<24>, district3: Fixed<24>, district4: Fixed<24>, district5: Fixed<24>, district6: Fixed<24>, district7: Fixed<24>, district8: Fixed<24>, district9: Fixed<24>, district10: Fixed<24> });

pub fn key(fields: &[u32]) -> Vec<u8> {
    fields
        .iter()
        .flat_map(|field| field.to_be_bytes())
        .collect()
}
pub fn key_field(bytes: &[u8], index: usize) -> Result<u32> {
    let start = index
        .checked_mul(4)
        .ok_or_else(|| invalid("TPC-C key index"))?;
    let end = start
        .checked_add(4)
        .ok_or_else(|| invalid("TPC-C key index"))?;
    let bytes = bytes
        .get(start..end)
        .ok_or_else(|| invalid("TPC-C key width"))?;
    Ok(u32::from_be_bytes(bytes.try_into().unwrap()))
}
pub fn name_key(warehouse: u32, district: u32, last: &[u8], first: &[u8]) -> Result<Vec<u8>> {
    let mut bytes = key(&[warehouse, district]);
    bytes.extend_from_slice(&Fixed::<16>::new(last)?.0);
    bytes.extend_from_slice(&Fixed::<16>::new(first)?.0);
    Ok(bytes)
}
