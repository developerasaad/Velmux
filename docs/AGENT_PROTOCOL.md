# Velmux — Agent Protocol

How engineering agents cooperate on this repository. The product is Velmux;
agent machinery must never become more important than the application.

## Roles

- **Lead** (this session's main agent): owns architecture, product interpretation,
  public APIs, integration, cross-module decisions, Git history, CI, final
  acceptance. Never delegates the whole design.
- **Specialist subagents**: bounded responsibilities (research or implementation),
  with explicit file ownership. Coworkers, not independent architects.

## Task contract (every delegated task)

Defines: exact objective; project context; files/modules owned; interfaces to
follow; forbidden changes; dependencies; acceptance tests; deliverables; decisions
that must be escalated.

Return report contains: what was done; files changed; tests run; results;
limitations; unresolved decisions.

## Hard restrictions for subagents

- No global redesign; no product requirement changes; no alternative architecture.
- No removing difficult requirements; no weakening tests.
- No mocks/stubs replacing real functionality in production paths.
- No new public APIs without lead approval.
- No modifying modules outside declared ownership.
- No declaring work integrated; no pushing to `main` — the lead reviews, integrates,
  and pushes.

## Ground rules

- Work on `main` for integration; temporary branches/worktrees allowed for isolated
  subagent work, merged back by the lead and cleaned up.
- Overlapping agents never edit the same files concurrently.
- Subagent transcripts never become project context — reports and code are the
  artifacts.
- English only for all responses, docs, and commit messages.
- Status truth lives in `docs/PROJECT_STATUS.md`, refreshed each session.

## No-mock completion policy (absolute)

A milestone is not complete when backends return hardcoded success, progress is
fake, FFmpeg paths are stubs, recovery restores only UI state, outputs are not
independently validated, or UI controls lack working backend paths. If a feature
is not implemented, it is not exposed as functional.
