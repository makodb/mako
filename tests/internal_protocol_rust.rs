// Import the generated module exactly as an external rustc consumer does. This
// is also the visibility boundary used when rusty-cpp emits the C++ module.
use srpc::internal_protocol::{
    encode_response_size, kInternalHeartbeatRpcId, kResponseHeaderExtFlag,
    kResponseSizeMask, response_has_extended_header, response_payload_size,
};

#[test]
fn constants_preserve_the_wire_contract() {
    assert_eq!(kInternalHeartbeatRpcId, i32::MIN);
    assert_eq!(kResponseHeaderExtFlag, 0x8000_0000);
    assert_eq!(kResponseSizeMask, 0x7fff_ffff);
}

#[test]
fn decode_and_encode_match_the_six_row_boundary_matrix() {
    let rows = [
        (0, false, 0, 0, i32::MIN),
        (1, false, 1, 1, i32::MIN + 1),
        (i32::MAX, false, i32::MAX, i32::MAX, -1),
        (i32::MIN, true, 0, 0, i32::MIN),
        (i32::MIN + 1, true, 1, 1, i32::MIN + 1),
        (-1, true, i32::MAX, i32::MAX, -1),
    ];

    for (input, has_extended, payload, plain, extended) in rows {
        assert_eq!(response_has_extended_header(input), has_extended);
        assert_eq!(response_payload_size(input), payload);
        assert_eq!(encode_response_size(input, false), plain);
        assert_eq!(encode_response_size(input, true), extended);
    }
}
