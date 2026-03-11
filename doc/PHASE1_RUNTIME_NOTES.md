# Phase 1 Runtime Notes (AI-first / Agents-first)

Implemented runtime foundations in `src/Extension/AIAgent.c` with practical controls:

- Intent router: ask/search/explain/patch/refactor/run/review/build/design_change/self_modify_ide.
- Budget contracts: quick/balanced/max_quality/economy with hard limits.
- Role contracts: Planner/Implementer/Reviewer/Debug/Test/Refactor/Research/Setup with tool-scope policies.
- Context ledger: per-task prompt/tool telemetry + persisted ledger log (`ai_context_ledger.log`).
- Failure memory: blocks repeated failing tool attempts in a task.
- Radius mode: touched-file limit and enforcement.
- Repo constitution loading into prompt (`repo.constitution.md`).
- Task graph status surface in agent status stream.
- Shadow mode (preview): user can request `shadow mode` / `dry run` to preview mutation steps without applying writes.
- Compact diff story appended to completion summaries.
- Long-file live embedding index: truncated reads now append a lightweight chunk-hash map (`[live-embedding-index]`) so follow-up reads can target relevant windows without replaying entire files.

These are implemented as runtime behavior (not just docs), while keeping the editor path native and non-blocking.
