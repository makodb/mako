//! Native scan ABI and transport-byte adapters. Production cursor/range/page
//! decisions and decoded-order contracts live in the same-source checked core.
use crate::full_scan_core as checked;
use checked::{Request, PageBudget, Completion, Maximum, read_blob, REQUEST_CAPACITY,
              PAGE_CAPACITY, REQUEST_TAG, PAGE_TAG};
#[cfg(test)]
use checked::PAGE_ROWS;
use crate::types::Status;
use crate::routing_codec::{Reader, Writer};

fn check(status: Status) -> Result<(), Status> {
    if status == Status::Ok { Ok(()) } else { Err(status) }
}

// Marshaling uses the shared LE-word/u64-blob convention. The allocation-free
// request writer targets the caller's fixed native buffer directly.
impl Request {
    fn decode(bytes: &[u8]) -> Result<Self, Status> {
        if bytes.len() > REQUEST_CAPACITY { return Err(Status::Invalid); }
        let mut reader = Reader::new(bytes);
        if reader.read_u32()? != REQUEST_TAG { return Err(Status::Invalid); }
        let flags = reader.read_u32()?;
        if flags & !7 != 0 { return Err(Status::Invalid); }
        let lo = read_blob(&mut reader)?.to_vec();
        let hi = read_blob(&mut reader)?;
        let cursor = read_blob(&mut reader)?;
        if (flags & 1 == 0 && !hi.is_empty()) || (flags & 2 == 0 && !cursor.is_empty()) {
            return Err(Status::Invalid);
        }
        check(reader.finish())?;
        let request = Self {
            lo, hi: if flags & 1 != 0 { Some(hi.to_vec()) } else { None },
            cursor: if flags & 2 != 0 { Some(cursor.to_vec()) } else { None },
            reverse: flags & 4 != 0,
        };
        if !request.valid() { return Err(Status::Invalid); }
        Ok(request)
    }
    fn encode(&self, output: &mut [u8]) -> Result<usize, Status> {
        let hi = self.hi.as_deref().unwrap_or(&[]);
        let cursor = self.cursor.as_deref().unwrap_or(&[]);
        let length = 32usize.checked_add(self.lo.len()).and_then(|n| n.checked_add(hi.len()))
            .and_then(|n| n.checked_add(cursor.len())).ok_or(Status::Exhausted)?;
        if length > REQUEST_CAPACITY || length > output.len() { return Err(Status::Exhausted); }
        let flags = u32::from(self.hi.is_some()) | (u32::from(self.cursor.is_some()) << 1)
            | (u32::from(self.reverse) << 2);
        output[..4].copy_from_slice(&REQUEST_TAG.to_le_bytes());
        output[4..8].copy_from_slice(&flags.to_le_bytes());
        let mut offset = 8;
        for bytes in [self.lo.as_slice(), hi, cursor] {
            output[offset..offset+8].copy_from_slice(&(bytes.len() as u64).to_le_bytes());
            offset += 8;
            output[offset..offset+bytes.len()].copy_from_slice(bytes);
            offset += bytes.len();
        }
        Ok(length)
    }
}

pub struct FullScan { request: Request, completion: Completion }
impl FullScan {
    fn new(request: Request) -> Self { Self { request,completion: Completion {done: false} } }
    fn consume(&mut self, bytes: &[u8], mut callback: impl FnMut(&[u8], &[u8]) -> bool)
        -> Result<bool, Status>
    {
        if self.completion.done { return Err(Status::Invalid); }
        // Validation has no callbacks/field edits. Rejection cannot expose a
        // partial malformed batch. Keys and values borrow transport storage.
        let page = self.request.validate_page(bytes)?;
        let mut reader = Reader::new(bytes);
        reader.read_slice(12)?;
        for _ in 0..page.count {
            let key = read_blob(&mut reader)?;
            let value = read_blob(&mut reader)?;
            if !callback(key,value) {
                check(self.completion.complete(page.count,page.eof,true))?;
                return Ok(true);
            }
        }
        if let Some(last) = page.last { check(self.request.advance(last))?; }
        check(self.completion.complete(page.count,page.eof,false))?;
        Ok(self.completion.done)
    }
}

pub struct Page {
    request: Request,
    bytes: Writer,
    last_offset: usize,
    last_length: usize,
    budget: PageBudget,
    error: Option<Status>,
    maximum: Maximum,
}
impl Page {
    fn new(request: Request) -> Result<Self, Status> {
        let mut bytes = Writer::with_capacity(PAGE_CAPACITY)?;
        check(bytes.write_u32(PAGE_TAG))?;
        check(bytes.write_u32(0))?;
        check(bytes.write_u32(0))?;
        Ok(Self { request,bytes,last_offset: 0,last_length: 0,
            budget: PageBudget::new(),error: None,maximum: Maximum::new() })
    }
    fn add(&mut self, key: &[u8], value: &[u8]) -> Result<bool, Status> {
        if self.error.is_some() { return Err(Status::Invalid); }
        let previous = if self.budget.count == 0 { self.request.cursor.as_deref() }
            else { Some(&self.bytes.as_bytes()[self.last_offset..self.last_offset+self.last_length]) };
        if !self.request.follows(key,previous) {
            self.error = Some(Status::Invalid); return Err(Status::Invalid);
        }
        match self.budget.reserve(key.len(),value.len()) {
            Status::Ok => {},
            Status::NotFound => return Ok(false),
            error => { self.error = Some(error); return Err(error); },
        }
        self.last_offset = self.bytes.len()+8;
        self.last_length = key.len();
        let result = check(self.bytes.write_bytes(key))
            .and_then(|()| check(self.bytes.write_bytes(value)));
        if let Err(error) = result { self.error = Some(error); return Err(error); }
        Ok(true)
    }
    fn finish(&self, output: &mut [u8]) -> Result<usize, Status> {
        if let Some(error) = self.error { return Err(error); }
        if output.len() < self.bytes.len() { return Err(Status::Exhausted); }
        output[..self.bytes.len()].copy_from_slice(self.bytes.as_bytes());
        output[4..8].copy_from_slice(&(self.budget.count as u32).to_le_bytes());
        output[8..12].copy_from_slice(&u32::from(!self.budget.more).to_le_bytes());
        Ok(self.bytes.len())
    }
}

// Native pointer and callback adapters are the narrow ABI boundary, not engine
// admission or a trusted statement that storage is complete.
#[allow(unsafe_code)]
mod ffi {
    use super::*;
    use std::ffi::c_void;
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct Bytes { data: *const u8, len: usize }
    impl Bytes {
        unsafe fn slice<'a>(self) -> Result<&'a [u8], Status> {
            if self.len == 0 { return Ok(&[]); }
            if self.data.is_null() || self.len > isize::MAX as usize { return Err(Status::Invalid); }
            Ok(std::slice::from_raw_parts(self.data, self.len))
        }
        fn from_slice(bytes: &[u8]) -> Self { Self { data: bytes.as_ptr(), len: bytes.len() } }
    }
    #[repr(C)]
    pub struct Bounds {
        lo: Bytes, hi: Bytes, cursor: Bytes,
        has_hi: u32, has_cursor: u32, reverse: u32,
    }
    unsafe fn output<'a>(data: *mut u8, size: usize) -> Result<&'a mut [u8], Status> {
        if data.is_null() || size > isize::MAX as usize { return Err(Status::Invalid); }
        Ok(std::slice::from_raw_parts_mut(data, size))
    }
    fn status(result: Result<(), Status>) -> u32 { result.err().unwrap_or(Status::Ok) as u32 }
    #[no_mangle]
    pub unsafe extern "C" fn mako_full_scan_new(bounds: Bounds, out: *mut *mut FullScan) -> u32 {
        status((|| {
            if out.is_null() || bounds.has_hi > 1 || bounds.has_cursor > 1 || bounds.reverse > 1 {
                return Err(Status::Invalid);
            }
            let request = Request {
                lo: bounds.lo.slice()?.to_vec(),
                hi: if bounds.has_hi != 0 { Some(bounds.hi.slice()?.to_vec()) } else { None },
                cursor: if bounds.has_cursor != 0 { Some(bounds.cursor.slice()?.to_vec()) } else { None },
                reverse: bounds.reverse != 0,
            };
            if !request.valid() { return Err(Status::Invalid); }
            *out = Box::into_raw(Box::new(FullScan::new(request)));
            Ok(())
        })())
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_full_scan_free(scan: *mut FullScan) {
        if !scan.is_null() { drop(Box::from_raw(scan)); }
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_full_scan_request(scan: *const FullScan, data: *mut u8,
        capacity: usize, length: *mut usize) -> u32 {
        status((|| {
            let scan = scan.as_ref().ok_or(Status::Invalid)?;
            if length.is_null() || scan.completion.done { return Err(Status::Invalid); }
            *length = scan.request.encode(output(data, capacity)?)?; Ok(())
        })())
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_full_scan_consume(scan: *mut FullScan, page: Bytes,
        callback: Option<unsafe extern "C" fn(*mut c_void, Bytes, Bytes) -> u32>,
        context: *mut c_void, done: *mut u32) -> u32 {
        status((|| {
            let scan = scan.as_mut().ok_or(Status::Invalid)?;
            let callback = callback.ok_or(Status::Invalid)?;
            if done.is_null() { return Err(Status::Invalid); }
            *done = u32::from(scan.consume(page.slice()?, |key, value| {
                callback(context, Bytes::from_slice(key), Bytes::from_slice(value)) != 0
            })?);
            Ok(())
        })())
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_scan_page_new(request: Bytes, out: *mut *mut Page) -> u32 {
        status((|| {
            if out.is_null() { return Err(Status::Invalid); }
            let request = Request::decode(request.slice()?)?;
            *out = Box::into_raw(Box::new(Page::new(request)?)); Ok(())
        })())
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_scan_page_free(page: *mut Page) {
        if !page.is_null() { drop(Box::from_raw(page)); }
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_scan_page_bounds(page: *const Page) -> Bounds {
        let r = &(*page).request;
        Bounds { lo: Bytes::from_slice(&r.lo), hi: Bytes::from_slice(r.hi.as_deref().unwrap_or(&[])),
            cursor: Bytes::from_slice(r.cursor.as_deref().unwrap_or(&[])),
            has_hi: u32::from(r.hi.is_some()), has_cursor: u32::from(r.cursor.is_some()),
            reverse: u32::from(r.reverse) }
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_scan_page_observe_max(page: *mut Page, key: Bytes) -> u32 {
        status((|| {
            let page = page.as_mut().ok_or(Status::Invalid)?;
            if page.error.is_some() || page.budget.count != 0 { return Err(Status::Invalid); }
            let result = check(page.maximum.observe(&page.request,key.slice()?));
            if let Err(error) = result { page.error = Some(error); }
            result
        })())
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_scan_page_maximum(page: *const Page, maximum: *mut Bytes,
        present: *mut u32) -> u32 {
        status((|| {
            let page = page.as_ref().ok_or(Status::Invalid)?;
            if maximum.is_null() || present.is_null() { return Err(Status::Invalid); }
            if let Some(error) = page.error { return Err(error); }
            *present = u32::from(page.maximum.key.is_some());
            *maximum = Bytes::from_slice(page.maximum.key.as_deref().unwrap_or(&[]));
            Ok(())
        })())
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_scan_page_add(page: *mut Page, key: Bytes, value: Bytes,
        continue_scan: *mut u32) -> u32 {
        status((|| {
            let page = page.as_mut().ok_or(Status::Invalid)?;
            if continue_scan.is_null() { return Err(Status::Invalid); }
            *continue_scan = u32::from(page.add(key.slice()?, value.slice()?)?); Ok(())
        })())
    }
    #[no_mangle]
    pub unsafe extern "C" fn mako_scan_page_finish(page: *const Page, data: *mut u8,
        capacity: usize, length: *mut usize) -> u32 {
        status((|| {
            let page = page.as_ref().ok_or(Status::Invalid)?;
            if length.is_null() { return Err(Status::Invalid); }
            *length = page.finish(output(data, capacity)?)?; Ok(())
        })())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn request(reverse: bool) -> Request {
        Request { lo: vec![], hi: Some(vec![255]), cursor: None, reverse }
    }
    #[test]
    fn binary_cursor_and_explicit_eof() {
        let mut scan = FullScan::new(request(false));
        let mut page = Page::new(request(false)).ok().unwrap();
        for key in [b"a".as_slice(), b"a\0", b"a\0\0", b"b"] { assert!(page.add(key, b"v").ok().unwrap()); }
        page.budget.more = true;
        let mut bytes = [0; PAGE_CAPACITY];
        let n = page.finish(&mut bytes).ok().unwrap();
        let mut keys = Vec::new();
        assert!(!scan.consume(&bytes[..n], |k,_| { keys.push(k.to_vec()); true }).ok().unwrap());
        assert_eq!(keys, [b"a".to_vec(), b"a\0".to_vec(), b"a\0\0".to_vec(), b"b".to_vec()]);
        assert!(scan.consume(&bytes[..n], |_,_| true).is_err());
        let empty = Page::new(request(false)).ok().unwrap();
        let n = empty.finish(&mut bytes).ok().unwrap();
        assert!(scan.consume(&bytes[..n], |_,_| true).ok().unwrap());
    }
    #[test]
    fn reverse_boundary_early_stop_and_malformed_page() {
        let mut page = Page::new(request(true)).ok().unwrap();
        for key in [b"z".as_slice(), b"a\0", b"a", b""] { assert!(page.add(key,b"v").ok().unwrap()); }
        let mut bytes = [0; PAGE_CAPACITY];
        let n = page.finish(&mut bytes).ok().unwrap();
        let mut scan = FullScan::new(request(true));
        let mut calls = 0;
        assert!(scan.consume(&bytes[..n], |_,_| { calls += 1; false }).ok().unwrap());
        assert_eq!(calls, 1);
        let mut scan = FullScan::new(request(true));
        assert!(scan.consume(&bytes[..n-1], |_,_| panic!("malformed batch exposed a row")).is_err());
    }
    #[test]
    fn exact_page_limit_and_request_roundtrip() {
        let mut page = Page::new(request(false)).ok().unwrap();
        for i in 0..PAGE_ROWS { assert!(page.add(&[i as u8],b"v").ok().unwrap()); }
        assert!(!page.add(&[PAGE_ROWS as u8],b"v").ok().unwrap());
        let mut bytes = [0; REQUEST_CAPACITY];
        let r = Request { lo: b"a\0".to_vec(), hi: Some(b"z".to_vec()), cursor: Some(b"b\0".to_vec()), reverse: true };
        let n = r.encode(&mut bytes).ok().unwrap();
        let decoded = Request::decode(&bytes[..n]).ok().unwrap();
        assert_eq!(decoded.lo,r.lo); assert_eq!(decoded.hi,r.hi);
        assert_eq!(decoded.cursor,r.cursor); assert!(decoded.reverse);
        assert!(Request::decode(&bytes[..n+1]).is_err());
    }
    #[test]
    fn oversized_and_out_of_range_rows_are_errors_not_eof() {
        let mut page = Page::new(request(false)).ok().unwrap();
        assert!(page.add(b"a",&[0; PAGE_CAPACITY]).is_err());
        let mut bytes = [0; PAGE_CAPACITY];
        assert!(page.finish(&mut bytes).is_err());
        let mut page = Page::new(request(false)).ok().unwrap();
        assert!(page.add(&[255],b"outside exclusive bound").is_err());
        assert!(page.finish(&mut bytes).is_err());
        let mut page = Page::new(request(false)).ok().unwrap();
        assert!(page.add(b"a",b"v").ok().unwrap());
        assert!(page.add(b"a",b"duplicate").is_err());
    }
    #[test]
    fn reverse_pages_resume_strictly_below_exact_binary_cursor() {
        let mut scan = FullScan::new(request(true));
        let mut first = Page::new(request(true)).ok().unwrap();
        assert!(first.add(b"a\0",b"one").ok().unwrap());
        first.budget.more = true;
        let mut bytes = [0; PAGE_CAPACITY];
        let n = first.finish(&mut bytes).ok().unwrap();
        assert!(!scan.consume(&bytes[..n],|_,_| true).ok().unwrap());
        let mut encoded = [0; REQUEST_CAPACITY];
        let n = scan.request.encode(&mut encoded).ok().unwrap();
        let next = Request::decode(&encoded[..n]).ok().unwrap();
        assert_eq!(next.cursor.as_deref(),Some(b"a\0".as_slice()));
        let mut second = Page::new(next).ok().unwrap();
        assert!(second.add(b"a",b"two").ok().unwrap());
        assert!(second.add(b"",b"three").ok().unwrap());
        let n = second.finish(&mut bytes).ok().unwrap();
        let mut keys = Vec::new();
        assert!(scan.consume(&bytes[..n],|key,_| {keys.push(key.to_vec()); true}).ok().unwrap());
        assert_eq!(keys,[b"a".to_vec(),Vec::new()]);
    }
    #[test]
    fn unbounded_reverse_discovers_real_maximum_and_preserves_empty_key() {
        let unbounded = || Request { lo: vec![],hi: None,cursor: None,reverse: true };
        let mut scan = FullScan::new(unbounded());
        let mut encoded = [0; REQUEST_CAPACITY];
        let n = scan.request.encode(&mut encoded).ok().unwrap();
        let mut page = Page::new(Request::decode(&encoded[..n]).ok().unwrap()).ok().unwrap();
        assert!(page.maximum.key.is_none());
        let keys = [vec![],vec![0],vec![255],vec![255,0,255]];
        for key in &keys {
            assert_eq!(page.maximum.observe(&page.request,key),Status::Ok);
        }
        assert_eq!(page.maximum.key.as_ref(),keys.last());
        for key in keys.iter().rev() { assert!(page.add(key,b"").ok().unwrap()); }
        let mut bytes = [0; PAGE_CAPACITY];
        let n = page.finish(&mut bytes).ok().unwrap();
        let mut delivered = Vec::new();
        assert!(scan.consume(&bytes[..n],|key,value| {
            assert!(value.is_empty()); delivered.push(key.to_vec()); true
        }).ok().unwrap());
        assert_eq!(delivered,keys.into_iter().rev().collect::<Vec<_>>());
        assert_eq!(scan.request.cursor.as_deref(),Some(b"".as_slice()));
        assert!(scan.consume(&bytes[..n],|_,_| panic!("completed scan replay")).is_err());
        let mut only_empty = Maximum::new();
        assert_eq!(only_empty.observe(&unbounded(),b""),Status::Ok);
        assert_eq!(only_empty.key.as_deref(),Some(b"".as_slice()));
    }
    #[test]
    fn byte_full_page_and_callback_stop_are_terminal_without_replay() {
        let unbounded = || Request { lo: vec![],hi: None,cursor: None,reverse: false };
        let mut page = Page::new(unbounded()).ok().unwrap();
        assert!(page.add(b"",&vec![0; PAGE_CAPACITY-28]).ok().unwrap());
        assert!(!page.add(b"a",b"").ok().unwrap());
        let mut bytes = [0; PAGE_CAPACITY];
        let n = page.finish(&mut bytes).ok().unwrap();
        assert_eq!(n,PAGE_CAPACITY);
        let mut scan = FullScan::new(unbounded());
        assert!(scan.consume(&bytes[..n],|_,_| false).ok().unwrap());
        assert!(scan.consume(&bytes[..n],|_,_| panic!("stopped scan replay")).is_err());
        let mut empty = FullScan::new(unbounded());
        let n = Page::new(unbounded()).ok().unwrap().finish(&mut bytes).ok().unwrap();
        assert!(empty.consume(&bytes[..n],|_,_| panic!("empty page delivered")).ok().unwrap());
    }
}
