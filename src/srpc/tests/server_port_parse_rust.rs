use srpc::server::server_parse_port;

#[test]
fn decimal_prefix_parsing_retains_sign_whitespace_and_trailing_text() {
    for (input, expected) in [
        ("0", 0),
        ("65535", 65535),
        ("  +123port", 123),
        ("\t\n\r\u{b}\u{c}-42tail", -42),
        ("00123", 123),
        ("-0", 0),
        ("123\0ignored", 123),
        ("2147483647", i32::MAX),
        ("-2147483648", i32::MIN),
    ] {
        assert_eq!(
            server_parse_port(input),
            Some(expected),
            "{input:?}"
        );
    }
}

#[test]
fn decimal_prefix_rejects_missing_digits_overflow_and_the_old_buffer_limit() {
    for input in [
        "",
        " ",
        "+",
        "-",
        "- 1",
        "port123",
        "2147483648",
        "-2147483649",
        "999999999999999999999999999999999999999",
        "\x00123",
    ] {
        assert_eq!(server_parse_port(input), None, "{input:?}");
    }
    assert_eq!(server_parse_port(&format!("{}1", "0".repeat(62))), Some(1));
    assert_eq!(server_parse_port(&format!("{}1", "0".repeat(63))), None);
}
