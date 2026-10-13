//! Snapshot image files (design §3): written whole before they are named, a
//! damaged or mismatched one refused, a crash before the directory sync
//! leaves no image, cleanup keeps the named one and newer.

use std::path::Path;

use raft_store::fs::Crash;
use raft_store::images;
use raft_store::{MemFs, StoreFs};

#[test]
fn write_read_damage_cleanup() {
    let fs = MemFs::new();
    let dir = Path::new("/s/images");
    fs.mkdir_p(dir);
    let name = images::write(&fs, dir, 10, 2, b"state at 10").unwrap();
    assert_eq!(name, "10-2.img");
    assert_eq!(images::read(&fs, dir, &name).unwrap(), b"state at 10");
    // Damage: a flipped byte, a truncation, a name that does not match.
    let mut b = fs.read(&dir.join(&name)).unwrap();
    b[40] ^= 1;
    fs.poke(&dir.join(&name), b.clone());
    assert!(images::read(&fs, dir, &name).unwrap_err().contains("damaged"));
    fs.poke(&dir.join(&name), b[..20].to_vec());
    assert!(images::read(&fs, dir, &name).is_err());
    images::write(&fs, dir, 20, 3, b"state at 20").unwrap();
    assert!(images::read(&fs, dir, "21-3.img").is_err());
    // Cleanup: below 20 goes, 20 stays; a .tmp only when asked.
    drop(fs.create_new(&dir.join("30-3.img.tmp")).unwrap());
    assert_eq!(images::cleanup(&fs, dir, 20, false).unwrap(), 1);
    assert!(fs.exists(&dir.join("20-3.img")) && fs.exists(&dir.join("30-3.img.tmp")));
    assert_eq!(images::cleanup(&fs, dir, 20, true).unwrap(), 1);
    assert!(!fs.exists(&dir.join("30-3.img.tmp")));
}

#[test]
fn a_power_cut_before_the_directory_sync_leaves_no_image() {
    let probe = MemFs::new();
    probe.mkdir_p(Path::new("/s/images"));
    let before = probe.ops();
    images::write(&probe, Path::new("/s/images"), 5, 1, b"x").unwrap();
    let steps = probe.ops() - before;
    for budget in 0..steps {
        let fs = MemFs::new();
        let dir = Path::new("/s/images");
        fs.mkdir_p(dir);
        fs.set_budget(Some(budget));
        assert!(images::write(&fs, dir, 5, 1, b"x").is_err());
        fs.crash(Crash::PowerCut);
        // Either no image, or (never here: the sync is the last step) a whole one.
        assert!(!fs.exists(&dir.join("5-1.img")), "budget {budget}");
    }
}
