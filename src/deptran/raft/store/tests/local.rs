//! The local-filesystem check (plan P2): mountinfo samples give the right
//! verdict, and this host's data directories do too.

use std::path::Path;

use raft_store::local::{check_local, mount_of, verdict};

const INFO: &str = "\
22 1 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw,errors=remount-ro
25 22 0:22 / /dev/shm rw,nosuid,nodev shared:4 - tmpfs tmpfs rw,inode64
30 22 0:28 / /tmp rw,nosuid,nodev shared:9 - tmpfs tmpfs rw,size=50331648k
40 22 0:40 / /home/users rw,relatime shared:20 - nfs4 130.245.173.100:/zoohome rw,vers=4.2
41 40 0:41 / /home/users/zyang2/mnt\\040space rw shared:21 - xfs /dev/sdb1 rw
42 22 0:42 / /var/tmp/over rw shared:22 - nfs 10.0.0.1:/x rw
43 22 0:43 /bind /data rw shared:23 - ext4 /dev/sda2 rw
";

#[test]
fn verdicts() {
    let ok = |p: &str| verdict(Path::new(p), INFO);
    assert_eq!(ok("/var/tmp/raft-wal-u/run.1").unwrap(), "ext4");
    assert_eq!(ok("/dev/shm/raft-wal-u").unwrap(), "tmpfs");
    assert_eq!(ok("/data/x").unwrap(), "ext4"); // a bind mount
    let nfs = ok("/home/users/zyang2/raft").unwrap_err();
    assert!(nfs.contains("nfs4") && nfs.contains("/home/users"), "{nfs}");
    assert!(ok("/var/tmp/over/x").unwrap_err().contains("nfs"));
    // A local mount nested inside NFS is local; an escaped space is decoded.
    assert_eq!(ok("/home/users/zyang2/mnt space/s").unwrap(), "xfs");
    // Whole components only: /home/usersX is not under /home/users.
    assert_eq!(ok("/home/usersX").unwrap(), "ext4");
    assert_eq!(mount_of(Path::new("/tmp/a"), INFO).unwrap().1, "tmpfs");
}

#[test]
fn this_host() {
    // The live mountinfo reads and decides (whatever /var/tmp and the home
    // are here: a CI container's are overlay, which the store refuses).
    for dir in [Some("/var/tmp".into()), std::env::var_os("HOME")].into_iter().flatten() {
        let dir = Path::new(&dir).canonicalize().unwrap();
        let info = std::fs::read_to_string("/proc/self/mountinfo").unwrap();
        let (_, t, _) = mount_of(&dir, &info).unwrap();
        assert_eq!(check_local(&dir).is_ok(), raft_store::local::LOCAL_TYPES.contains(&t.as_str()), "{}", dir.display());
    }
}
