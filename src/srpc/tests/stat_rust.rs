// Exercise the extracted module exactly as an external rustc consumer does.
// Public fields are part of the existing production C++ aggregate surface.
use srpc::stat::AvgStat;

fn assert_stat(stat: &AvgStat, count: i64, sum: i64, average: i64, maximum: i64, minimum: i64) {
    assert_eq!(stat.n_stat_, count);
    assert_eq!(stat.sum_, sum);
    assert_eq!(stat.avg_, average);
    assert_eq!(stat.max_, maximum);
    assert_eq!(stat.min_, minimum);
    assert_eq!(stat.avg(), average);
}

#[test]
fn new_sample_and_clear_preserve_the_public_boundary() {
    let mut stat = AvgStat::new();
    assert_stat(&stat, 0, 0, 0, 0, 0);

    stat.sample(3);
    stat.sample(-5);
    stat.sample(8);
    assert_stat(&stat, 3, 6, 2, 8, -5);

    stat.clear();
    assert_stat(&stat, 0, 0, 0, 0, 0);
}

#[test]
fn peek_and_reset_return_snapshots_without_layout_indirection() {
    let mut stat = AvgStat::new();
    stat.sample(3);
    stat.sample(-5);
    stat.sample(8);

    let peeked = stat.peek();
    assert_stat(&peeked, 3, 6, 2, 8, -5);
    assert_stat(&stat, 3, 6, 2, 8, -5);

    let reset = stat.reset();
    assert_stat(&reset, 3, 6, 2, 8, -5);
    assert_stat(&stat, 0, 0, 0, 0, 0);

    stat.sample(-7);
    stat.sample(-2);
    assert_stat(&stat, 2, -9, -4, 0, -7);
}
