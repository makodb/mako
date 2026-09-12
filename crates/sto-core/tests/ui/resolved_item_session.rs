mod support;

use sto_core::ResolvedItemSession;

fn escape(
    session: ResolvedItemSession<'_, support::Adapter>,
) -> ResolvedItemSession<'static, support::Adapter> {
    session
}

fn main() {}
