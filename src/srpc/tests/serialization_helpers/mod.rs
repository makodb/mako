use srpc::serializable::{
    make_sink_proxy_buffer, make_source_proxy_buffer, BinaryReadArchive, BinaryWriteArchive,
    BufferSink, BufferSource,
};

#[allow(unsafe_code)]
pub fn encode(write: impl FnOnce(&mut BinaryWriteArchive)) -> Vec<u8> {
    let mut sink = BufferSink { bytes: Vec::new() };
    {
        // The stack allocation remains alive and unmoved until the archive drops.
        let mut archive = BinaryWriteArchive {
            sink_: unsafe { make_sink_proxy_buffer(&raw mut sink) },
        };
        write(&mut archive);
    }
    sink.bytes
}

#[allow(unsafe_code)]
pub fn decode<R>(bytes: &[u8], read: impl FnOnce(&mut BinaryReadArchive) -> R) -> (R, usize) {
    let mut source = BufferSource::new(bytes.as_ptr(), bytes.len());
    let result = {
        // Both the source cursor and its borrowed bytes outlive the archive.
        let mut archive = BinaryReadArchive::new(unsafe {
            make_source_proxy_buffer(&raw mut source)
        });
        read(&mut archive)
    };
    (result, source.remaining())
}
