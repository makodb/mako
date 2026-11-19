// Separate executable deliberately links the production entropy kernel.
#[test]
fn client_random_draws_use_real_entropy_and_respect_bounds() {
    let mut seen = std::collections::BTreeSet::new();
    for _ in 0..256 {
        let value = srpc::client::client_rand(-5, 5);
        assert!((-5..=5).contains(&value));
        seen.insert(value);
    }
    assert!(seen.len() > 1, "client random draws were constant: {seen:?}");
    assert_eq!(srpc::client::client_rand(7, 7), 7);
}
