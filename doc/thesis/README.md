# Thesis Workspace

This folder is now the working thesis area, not a full implementation
reference dump.

## Active Files

- `complete_thesis.md` is the current paper-style thesis draft.
- `Thesis Tips and Figures/` contains final manual figures, generated graphs,
  frozen graph data, university guideline PDFs, and the current figure guide.
- `archive/` contains older planning drafts and implementation reference notes.
  Use it only if you need source material that was removed from the active
  workspace.

## Figure And Data Locations

- Manual architecture figures:
  `Thesis Tips and Figures/figures/manual/`
- Generated evaluation graphs:
  `Thesis Tips and Figures/figures/graphs/`
- Frozen CSV/Markdown data used for graph numbers:
  `Thesis Tips and Figures/data_snapshot/`
- Diagram prototype HTML:
  `Thesis Tips and Figures/prototypes/thesis_diagram_prototypes.html`
- University formatting guides:
  `Thesis Tips and Figures/guidelines/`

## Current Thesis Story

Raft can replace Mako's original Multi-Paxos backend without leaving Paxos's
performance class, but only when the integration preserves Mako's execution
model: preferred leadership keeps submission on the expected leader, Multi-Raft
preserves Paxos-shaped parallelism, and Single-Raft becomes viable only after
ReplayPool keeps follower replay from falling behind.

## Cleanup Policy

Keep the root folder small. New thesis-facing assets should go into
`Thesis Tips and Figures/`; old notes, outdated drafts, and source-code
reference material should go into `archive/` rather than the root.
