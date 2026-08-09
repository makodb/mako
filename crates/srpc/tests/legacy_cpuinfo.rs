#[allow(dead_code)]
#[path = "../src/base/legacy_cpuinfo.rs"]
mod legacy_cpuinfo;

use legacy_cpuinfo::CPUInfo;

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn owner_keeps_the_historical_one_method_public_surface() {
    let owner = include_str!("../src/base/legacy_cpuinfo.rs");
    assert_eq!(owner.matches("pub struct CPUInfo").count(), 1_usize);
    assert_eq!(owner.matches("pub fn cpu_stat").count(), 1_usize);
    assert!(!owner.contains("pub fn get_cpu_stat"));
    assert!(!owner.contains("pub fn cpuinfo_"));
}

#[test]
fn native_type_is_send_sync_while_cpp_trait_opt_out_is_pinned() {
    assert_send_sync::<CPUInfo>();
    let owner = include_str!("../src/base/legacy_cpuinfo.rs");
    assert!(owner.contains("#[cfg_attr(any(), cpp_no_auto_traits)]"));
    assert_eq!(core::mem::align_of::<CPUInfo>(), 8_usize);
}

#[test]
fn strtoul_base_zero_and_lenient_suffix_semantics_are_preserved() {
    assert_eq!(legacy_cpuinfo::cpp::test_parse_token(""), 0_u64);
    assert_eq!(legacy_cpuinfo::cpp::test_parse_token("19"), 19_u64);
    assert_eq!(legacy_cpuinfo::cpp::test_parse_token("077tail"), 63_u64);
    assert_eq!(legacy_cpuinfo::cpp::test_parse_token("0x10garbage"), 16_u64);
    assert_eq!(
        legacy_cpuinfo::cpp::test_parse_token("-1"),
        usize::MAX as u64
    );
}

#[test]
fn network_field_numbering_and_names_remain_legacy_compatible() {
    let line = "  lo: 101 2 3 4 5 6 7 8 909 10 11 12 13 14 15 16";
    let (txed, rxed) = legacy_cpuinfo::cpp::test_parse_network_line(line);
    assert_eq!(txed, 101_u64, "legacy txed reads /proc field one");
    assert_eq!(rxed, 909_u64, "legacy rxed reads /proc field nine");
}

#[test]
fn live_linux_sampler_returns_exactly_four_warmup_metrics() {
    let first = CPUInfo::cpu_stat();
    let second = CPUInfo::cpu_stat();

    assert_eq!(first.len(), 4_usize);
    assert_eq!(second.len(), 4_usize);
    assert!(first.iter().all(|value| value.is_finite()));
    assert!(second.iter().all(|value| value.is_finite()));
    assert!(first.iter().all(|value| *value == -1.0_f64));
    assert!(second.iter().all(|value| *value == -1.0_f64));
}
