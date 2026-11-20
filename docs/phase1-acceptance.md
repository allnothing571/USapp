# Phase 1 qualified closeout

Recorded 2026-10-27 by Claude Code from existing milestone evidence and the owner's
Android report. This is a qualified closeout of Phase 1 for continued private local use
and for starting Phase 2. It is **not** production-readiness approval. Phase 0
limitations ([phase0-acceptance.md](phase0-acceptance.md)) remain open. No checks were
re-run for this document.

## 1. Implemented and automated-test verified

| Milestone | Scope | Evidence |
|---|---|---|
| M1 | Nickname editing | [phase1-plan.md](phase1-plan.md) Sections 7.8-7.9 |
| M2-A/M2-B | Append-only wallet ledger; own balances | [phase1-plan.md](phase1-plan.md) Sections 9.14-9.20 |
| M3-A/B/C | One-off tasks, publication, completion and settlement | [phase1-m3-design.md](phase1-m3-design.md), [phase1-m3c-design.md](phase1-m3c-design.md) |
| M4 | Counting tasks | [phase1-plan.md](phase1-plan.md) Section 10.8 |
| M5-A/B/C | Daily, weekly and custom-range tasks | [phase1-m5-daily.md](phase1-m5-daily.md), [phase1-m5-weekly.md](phase1-m5-weekly.md), [phase1-m5-custom.md](phase1-m5-custom.md) |
| M6 | Task edit/cancel with recipient confirmation | [phase1-m6-design.md](phase1-m6-design.md) |
| M7 | Reward-shop products | [phase1-m7-rewards.md](phase1-m7-rewards.md) |
| M8 | Direct redemption and fulfilment | [phase1-m8-redemption.md](phase1-m8-redemption.md) |
| M9 | Request redemption, mutual cancellation and exact refunds | [phase1-m9-redemption.md](phase1-m9-redemption.md) |
| M10 | Four-quadrant home, bottom navigation, Me page | [phase1-m10-home.md](phase1-m10-home.md) |

Every milestone above records passing automated checks (backend CTest including
db_gated, SQL compatibility scripts in both roles, web unit tests and typecheck, as
applicable) in its own document.

## 2. Deployed (recorded rollout evidence)

- Main database: migrations 0000-0016 applied and frozen; checksums verified at each
  rollout (latest: [phase1-m9-redemption.md](phase1-m9-redemption.md) Section 5).
- API: M9 runtime deployed with clean startup policies (wallet 187, task 972 checks).
- Web: M10 frontend published with verified served hashes
  ([phase1-m10-home.md](phase1-m10-home.md) Section 8).
- Local runtime: LAN HTTPS at `https://192.168.1.55:8443`
  ([../deploy/LOCAL-HTTPS.md](../deploy/LOCAL-HTTPS.md) Section 8).

## 3. Owner-confirmed Android checks (reported 2026-10-27)

The owner reported that these worked on an Android device:

- installing the local root certificate;
- opening the site over LAN HTTPS;
- logging in;
- seeing the own nickname and own balances;
- basic bottom navigation;
- installing the app to the home screen.

No other device behaviour is claimed.

## 4. Not verified, and known limitations

- Not confirmed by the owner on a real device:
  - task completion and counting;
  - task edit/cancel;
  - product management;
  - direct and request redemption, fulfilment, cancellation and refund;
  - home pending links and date rollover;
  - Me invitation flows.
- Installed-app update activation (service-worker update prompt) is unverified.
- Exhaustive device/browser acceptance (iOS, other Android browsers, safe areas on
  multiple devices) is not performed.
- Rollback paths were documented but never exercised.
- Waived live checks stay waived: M4 remaining checks, M5-A live checks
  ([phase1-plan.md](phase1-plan.md) Section 10.8, [phase1-m5-daily.md](phase1-m5-daily.md)).
- Known product limitations are recorded in the milestone documents, for example:
  - task-change indicators cover open tasks plus 30 days of history;
  - cursor pages are not snapshots;
  - sunshine points have nothing to spend on until Phase 4.
- The LAN address comes from DHCP; every Compose command must include
  `deploy/docker-compose.lan.yml`.

## 5. Decision recorded here

Phase 1 is closed as **implemented, automated-test verified and deployed for private
local use, with partial owner device confirmation**. The unverified workflows above stay
open and should be exercised during normal use; a defect found there is fixed as a
Phase 1 repair, not as Phase 2 scope.
