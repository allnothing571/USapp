# Phase 2 plan: interaction

Status (2026-10-27): the owner approved retaining the existing stack (Vue 3, C++/Drogon,
PostgreSQL, Caddy, Docker Compose) and this sequence. Each milestone needs its own
design and approval. Update (2026-10-29): P2-2 implementation and
test-database verification are complete, including the pin-replay repair; P2-3 design was
assigned directly to Codex. P2-3a mandatory notifications were subsequently approved,
implemented and test-verified on usapp_test; 0019 frozen there only. P2-3b is implemented
and cc-reported test-verified (333 web tests after repairs), with Codex targeted static
review passed; 0020 frozen on test only. P2-4 was approved, implemented and test-verified by Codex on usapp_test; 0021 frozen there only (phase2-reactions.md Section 8).
Main rollout/device verification remain deferred. Phase 1 closeout:
[phase1-acceptance.md](phase1-acceptance.md).

Requirement references are to the original product document (V1). The extract used for
this plan is `_req_extract.txt` in the repository root; section numbers are V1's.

## 1. Sequence

| Item | Scope | V1 source | Depends on |
|---|---|---|---|
| **P2-1 Messages** (implemented, test-verified and deployed; phone acceptance pending) | Plain-text message board, one short reply per message, history and detail, partner's latest message on Home, "我们" (Us) navigation | §25, §19 (bottom-right), §47, §42 (`Message`, `MessageReply`), §3.1 | Phase 1 |
| **P2-2 Anniversaries** (approved 2026-10-29; implemented and verified on usapp_test; 0018 not on main; not deployed; [phase2-anniversaries.md](phase2-anniversaries.md) Sections 14-15) | Anniversaries and countdowns, Home top-right with a pinned or nearest item | §24, §19 (top-right), §44 | P2-1 (Us page) |
| P2-3 In-app notifications (P2-3a implemented/test-verified on usapp_test, not deployed; P2-3b implemented and test-verified on usapp_test, 0020 frozen there only; [phase2-notifications.md](phase2-notifications.md) Sections 11, 13) | Proposed split: P2-3a mandatory inbox/count/read; P2-3b optional events, "notify partner" toggle and choice memory | §17, §18, §18.3, §52.4 | P2-1, P2-2 |
| P2-4 Reactions (implemented/test-verified on usapp_test; 0021 frozen there only, not deployed; [phase2-reactions.md](phase2-reactions.md)) | One reaction per task completion; message quick replies reuse the existing single reply slot | §13, §25 ("可以发送一个轻量回应") | P2-3 |
| P2-5 Web Push (P2-5a and P2-5b implemented and test-verified with a fake transport, 0022-0023 on usapp_test only, not deployed; repairs and 0024 verified in a disposable isolated instance; P2-5c transport implemented and verified in isolation with stubs only, off by default, no real provider or device; [phase2-web-push.md §12-16](phase2-web-push.md#16-p2-5c-implementation-and-isolated-verification-claude-code-2026-11-02)) | Push delivery for notifications through the existing push-worker boundary | §18.3, §52.4 | P2-3 |
| P2-6 Reminders (owner approved O1-O15; R1 rule storage and API implemented and verified in disposable instances only, no generator, 0025 and repair 0026 frozen and on neither shared test nor main, not deployed; R2 in-app generation implemented the same way, off by default, [§18](phase2-reminders.md#18-r2-implementation-and-isolated-verification-claude-code-2026-11-02); R3 web controls implemented with automated tests only, [§19](phase2-reminders.md#19-r3-implementation-and-automated-verification-claude-code-2026-11-02); R4 rule inheritance implemented in disposable instances only, 0027 frozen, [§20](phase2-reminders.md#20-r4-implementation-and-isolated-verification-claude-code-2026-11-03); R5 stale push suppression implemented in disposable instances only, fake transport, 0028 frozen, [§21](phase2-reminders.md#21-r5-implementation-and-isolated-verification-claude-code-2026-11-03); generation status exposed, contract 0.23.0, [§22](phase2-reminders.md#22-reminder-generation-status-claude-code-2026-11-03); rollout unapproved; [phase2-reminders.md §17](phase2-reminders.md#17-r1-implementation-and-isolated-verification-claude-code-2026-11-02)) | Recipient-set task reminders and anniversary reminders | §11, §24 | P2-5 |

## 2. Cross-cutting rules for Phase 2

- Couple membership is checked server-side for every read and write (§54); the client
  never decides visibility.
- Every state-changing request keeps exact Origin, session-bound CSRF, idempotency keys
  for creations, private `no-store` responses, and no offline write queue.
- Each domain has its own append-only or narrowly granted tables. There is no generic
  workflow engine.
- Rows that later produce notifications keep stable UUIDs and are written with an audit
  row in the same transaction. P2-3 will consume these events; nothing speculative is
  built before it.
- Migrations 0000-0017 are frozen and applied on test and main (P2-1 rollout 2026-10-27).
- Migrations 0018-0021 are frozen on usapp_test only (P2-2/P2-3a/P2-3b/P2-4); none is applied on main.
- §29/§30 (creator edits, edit requests for shared content) apply to shared content such
  as anniversaries and plans. They are not applied to messages: a message is a personal
  note, not shared content (see [phase2-messages.md](phase2-messages.md) Section 3).

## 3. Explicitly out of Phase 2

Real-time chat, images or attachments in messages (images arrive with Phase 3 storage,
§45), multi-level replies, and anything listed in V1 §48.
