# Bikode Repo Constitution

## Architecture Laws
1. Keep editor interaction path native and non-blocking.
2. Keep Python off keystroke/render hot paths.
3. Route AI tasks through intent + budget contracts before model calls.

## Agent Safety Laws
1. Start with smallest radius (selection/function/file) and widen only with evidence.
2. Do not touch more than 6 files in balanced mode without explicit user request.
3. Prefer non-destructive previews before mutation when risk is high.

## Code Quality Laws
1. Every mutation task must provide a compact causal summary.
2. Expensive tool usage and shell commands must be budget-aware.
3. Repeated failed tool paths should be avoided within the same task.
