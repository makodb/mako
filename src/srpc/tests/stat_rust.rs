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
    // max is the true maximum of the stream (-2), not the zero-seeded field.
    // The former seed-at-0 body reported 0 here; see the srpc.stat Verus spec.
    assert_stat(&stat, 2, -9, -4, -2, -7);
}

#[test]
fn all_positive_stream_reports_the_true_min_not_zero() {
    // Regression for the seed-at-0 bug: an all-positive stream must report its
    // real minimum, not the zero the field was initialised to.
    let mut stat = AvgStat::new();
    stat.sample(12);
    stat.sample(7);
    stat.sample(33);
    stat.sample(5);
    stat.sample(91);
    assert_stat(&stat, 5, 148, 29, 91, 5);
}
