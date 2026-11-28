//! Durable checkpoint publication bounds log trimming.
use vstd::prelude::*;
verus! {
pub fn may_trim(requested: u64, checkpoint: u64, files_validated: bool, files_published: bool) -> (allowed: bool)
    ensures allowed <==> files_validated && files_published && requested <= checkpoint,
{
    files_validated && files_published && requested <= checkpoint
}

}
