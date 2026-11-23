# V2 shared room

The worktree starts from V1 candidate `15d94a9032d2bd4965399b26c2119ae29219d8bf`.
The home screen links to one room per shared space. The layout is initially empty
and offers four free furnishings. A member can place, move or remove each piece
on a four-by-three grid. The view loads the saved layout on entry and on explicit
refresh.

Migration 0054 stores the shared layout and per-member appearance consent. The
three room routes derive identity from the same-origin authenticated session.
Writes require the existing CSRF checks. Database functions derive the room from
membership and the application role has no direct access to these tables.
Every layout save specifies its expected version. An initial insert and subsequent
updates reject a stale version without replacing the newer layout.

Each member's appearance is private by default. An opted-in partner's current
appearance is rechecked against room membership and consent before the response.
Responses keep the partner ID internal. The room reuses existing V1 placeholder
avatar assets. It does not post asset or wallet entries.

The first batch covered the free layout and appearance. Mobile acceptance
with two physical devices and production release are tracked separately.

## Sunshine furniture and live room

The catalog starts with a vase for 10 sunshine, a bookshelf for 20 and an
armchair for 30. A member confirms the price before purchase. The debit is
posted to that member's sunshine ledger and the entitlement belongs to the
current space. Both members can place the item. Replay uses the original item
and key. A second member cannot buy the same entitlement. The layout still has
four placement slots.

The room socket is available to authenticated members from the public site's
origin. Presence starts hidden and is published only while a member chooses to
share it and keeps the room visible. The server sends invalidations without
room content. The receiving browser reads the authorized snapshot. The room
continues to poll during a socket outage.

Live saves are opt-in. Each placement, removal or template saves against the
last confirmed version. If a write conflicts, the browser retains the draft and
returns to the existing merge flow, including a choice when the combined
layout exceeds four items. A disconnected browser can use manual saves. Socket
notifications operate within one API process.

The web suite passed 1,012 tests, both type checks and a production build.
The isolated backend run `usapp-e1-a3f140db74` passed 36 CTest targets, 1,513
HTTP assertions and the furniture ledger and room authorization checks.
Run `usapp-backup-probe-4b57c95506` passed the V1/V2 release pair with
54 → 59 → 59 migrations, two-session room invalidation and presence consent,
wrong-origin refusal, one sunshine debit under a concurrent purchase, replay,
and placement by the other member. The frozen V1 fixture stayed intact.

## First batch verification

- Web production build and both type checks passed. All 971 web unit tests
  passed, including three room tests.
- Headless Edge loaded the production web build at a narrow viewport against
  a local synthetic API. Placing a table, saving and reading the layout back
  passed without horizontal overflow.
- The isolated PostgreSQL ledger covered membership, validation and consent.
  Live HTTP checks exercised the public listener with separate test sessions
  for both members and an unrelated space. The live suite passed 1,482
  assertions and the full CTest suite passed all 36 targets.

## Second batch: keeping a draft while reading the latest layout

The web client compares the layout originally loaded, the user's draft and the
latest saved layout. It combines compatible changes, including removals, and
uses the latest version on the next explicit save. If the latest layout already
contains the draft's changes, the Save button stays disabled.

Different edits to the same item or two items competing for a cell require a
decision. The room identifies the items or the cell, keeps the original draft
visible and blocks saving. The user can read again or explicitly choose to
replace the draft with the latest layout. A connection failure preserves the
draft and requires rereading before saving. The existing endpoint and database
version check continue to guard every save.

The web build and both type checks passed, along with all 975 web tests. An
Edge smoke test at a narrow viewport used a local synthetic API to return a
stale-version refusal, then merged and saved both placements. This batch
changed only the web client; two-device acceptance remains pending.

## Third batch: visible room synchronization

The web room checks the existing read endpoint every 15 seconds while the
document is visible and attempts an immediate check when it becomes visible.
A foreground action cancels an in-flight background read and takes priority.
Background reads time out after eight seconds and retry on the next interval.

An idle room adopts a new saved layout automatically. While the user has a
draft, a newer saved version leaves the draft and its base intact and requires
reading the latest layout before another save. The user can use the existing
merge action to keep compatible changes. Synchronization never saves a layout.
Partner appearance follows fresh consent reads even while a layout draft is
open. The view hides that appearance while the document is hidden or when a
background check fails. Authorization loss clears the room.

The web build, both type checks and all 981 web tests passed. The Edge narrow
viewport smoke test used a local synthetic API to exercise idle updates, draft
protection, merging and explicit save. Backend code and migrations were outside
this batch. Two-device acceptance remains pending.

## V1 to V2 application upgrade and rollback rehearsal

Run `pwsh -NoProfile -File tools/backup-verify.ps1 -SharedRoomUpgrade` from
this worktree. The mode uses the exact local V1 candidate tag and compiles V1
and V2 from separate source trees with the same local compiler image. The
database runs on an internal Docker network with temporary storage and no host
ports. All accounts, sessions and records in the rehearsal are synthetic.

1. Apply the frozen 54 V1 migrations, seed a shared space with a wallet and
   message, and check that the V1 application serves them.
2. Apply and repeat the V2 migrations. Check every installed checksum and
   verify layout, appearance consent and watering with separate member sessions.
3. Run the V1 application again on the V2 schema. Read the legacy data and
   publish a new legacy message. The upgraded schema stays in place.
4. Run V2 again. Check the V1 message, wallet and original fixture fingerprint.
   Verify that V2 layout, consent and watering survived, then save a second
   layout version.

This is an application rollback on the upgraded schema. Browser and phone
acceptance, deployment configuration, and production data restoration require
their own evidence.

The isolated run `usapp-backup-probe-fd3bc527b6` passed all four startup and
HTTP checks. The migration ledger went from 54 entries to 55 and stayed at
55 across rollback and restart. The repeat migration reported zero privilege
deviations. V1 posted a message during the rollback, and V2 read it after
returning. V2 also read the original room layout and shared appearance before
saving a new layout. The original wallet, message, reply and session fixture
fingerprint matched the baseline.

## Shared garden

The small room links to the shared garden. Each space has one plant. Both
members see the cumulative watering count, the current growth stage, and each
member's status for today. The plant starts as a seed, sprouts at 3 waterings,
grows leaves at 8, and blooms at 14. Skipped days do not remove progress.
Watering is free and does not create a wallet transaction.

Migration 0055 stores one watering per member, space, and Beijing date.
The authenticated read endpoint derives the space from membership. It
aggregates existing waterings and does not create any records. Watering uses
the date shown by the server and a database uniqueness constraint. Concurrent
or repeated requests for the same member and date confirm one record. A request
for a past or future date is refused. The page then reads today's state and
leaves the new day's watering to a fresh click. The app role has no direct
table access and the push role cannot call either garden function.

The view reads progress on entry, when returning from the background, and on
manual refresh. It refreshes after a confirmed watering and uses the server
total for stage changes. After an uncertain result, the user can refresh to
check the recorded state. The existing isolated upgrade/restart probe now
includes the garden and verifies that a watering survives application rollback.

The database proof covered same-day repeats, stale and future dates, a prior
watering with a missed day, both members and another space. Live HTTP checks
also submitted two concurrent writes for one member and verified the count.
The web build, type checks and all 985 web tests passed. An Edge narrow
viewport smoke test with a local synthetic API followed the room link, watered
the plant and verified its transition from seed to sprout.

Isolated backend run `usapp-e1-bae38d381f` passed 1,499 live HTTP assertions
and all 36 CTest targets. It verified all 56 migration checksums and reported
zero privilege deviations after migration, repeat application and testing.

Upgrade/rollback run `usapp-backup-probe-352c3dd9c3` passed all four stages
with the migration counts 54 → 56 → 56. The original V1 fixture and the
watering recorded under V2 were present after returning to V2.

### Automatic garden synchronization

Once the garden loads, the web page checks for an updated snapshot every 15
seconds while visible. The server's Beijing date drives the two daily statuses
and button availability, so the next day's first refresh enables watering
without resetting accumulated growth. The automatic path only reads.

Hiding the page pauses polling, and returning brings an immediate check.
Manual refresh and watering cancel any outstanding poll. A timed-out or failed
poll keeps the last successful snapshot visible with a status message until the
next successful read. Authorization loss clears the snapshot. This batch uses
the existing garden endpoints and rules.

The web build and all 989 web tests passed. Four new cases exercise server-date
rollover, visibility, a late poll after watering, and timeout followed by loss
of access. The browser rerun could not be completed in the available test
window.

### Shared room draft undo and redo

Placing, moving and removing furniture records one local draft step. The
current draft keeps up to 30 undo steps, and a new edit drops the redo steps.
Undo and redo do not send requests. Only the existing Save action publishes a
layout; a confirmed save starts a fresh draft.

Adopting a newer shared layout drops the earlier draft history. After merging
compatible changes, one undo restores the latest shared layout, including the
other member's changes. Redo restores the merged draft. An unsuccessful save
still uses the existing conflict or uncertain-result recovery.

The web build and all 992 web tests passed. The room tests exercised
placement, moving and removal, redo invalidation, confirmed save, background
replacement of an old redo branch and undo after merging a partner change.

### Retaining the room draft across navigation

Leaving the room holds the layout draft and its undo and redo history in the
current tab's application memory. The copy is keyed by account and shared
space and is cleared on sign-out, account or space changes, and loss of room
access. Reloading or closing the tab ends the draft.

Returning reads the room from the server before displaying a saved draft. If
the shared version changed, the existing merge compares the original base,
the draft and the latest layout. Compatible changes merge for review and a
fresh manual save. Conflicting changes keep the draft visible and block a
stale save. Avatar appearance and consent always come from the fresh read.

The web build and all 995 web tests passed. A narrow Chrome viewport with a
local synthetic API verified two round trips from the room to the garden,
restoration of redo, a compatible merge, undo to the latest shared layout and
one explicit save against the new version.

### Seven-day watering history

The garden shows today and the six preceding Beijing dates. Each row reports
the viewer's and the other member's status, including dates with no watering.
Migration 0056 adds a read-only member-scoped function over existing watering
records. The garden endpoint reads the history and the headline count in the
same SQL statement. Refresh and polling replace them together.

The additive response field is optional for compatibility with older V2
servers. The web page omits the history panel when the older response does
not include it. The function returns daily flags only, and the application
role still has no direct access to the watering table.

The web build and all 996 web tests passed. A local synthetic Chrome test
verified the seven rows and the updated history after watering. Backend
run `usapp-e1-d84e5e5b23` passed 1,504 live HTTP assertions and all 36 CTest
targets, with 57 checked migrations and zero privilege deviations.
Upgrade/rollback run `usapp-backup-probe-a297affd60` passed with the migration
counts 54 → 57 → 57 and verified that V2 could still read the historic
watering from the other member's viewpoint.

### Room layout templates

The room offers Tea Corner, Window Plants and Lounge Corner templates. Each
preview uses the existing four-by-three grid and furniture catalog. Applying
a template replaces the local draft as one undoable edit, after which individual
items can still be moved or removed. Selecting the already-applied template adds
no history entry. The existing Save button and version check publish the draft.

The web build and all 998 web tests passed. A local Chrome test at a narrow
viewport checked the previews, draft replacement, undo and redo. The mock
server received one request when Save was pressed.

### Item-by-item room conflict resolution

When a merge finds incompatible changes, the room shows both versions of
each affected furnishing. The user can choose its location or absence from
their draft or the shared layout. Choices combine with compatible edits
already carried forward by the merge. If a choice introduces another cell
collision, the affected items are shown and applying stays disabled.

Applying builds a new local draft against the checked shared version. One
undo restores that shared layout. Publishing still requires an explicit
save. If polling detects another shared version before applying, the page
discards the choices and asks for a fresh read while keeping the draft.

The web build and all 1,001 web tests passed. A narrow-viewport Chrome test
against a local synthetic API resolved a cell collision, kept a compatible
partner edit and verified that the only shared write followed Save.

### Garden status on the home page

The home page links directly to the shared garden. Its card reads the existing
garden snapshot and shows the current growth stage, total waterings, and both
members' status on the server's Beijing date. The card shares stage rules with
the garden page. It loads independently from the wallet and other home cards,
and follows the home page's refresh, foreground, reconnect and daily schedule.
If the read fails, users can retry the card alone. A space or account change
cancels and discards the earlier request.

The web build and all 1,004 web tests passed. A narrow Chrome test with a local
synthetic API verified wallet loading during a garden read failure, an
independent retry and navigation from the home card to the garden.

### Monthly watering calendar

The garden's seven-day panel links to a monthly calendar with previous month,
next month and a return-to-current-month action. Each day shows separate
watering marks and the calendar counts the days when both members watered.

Migration 0057 adds a bounded, read-only lookup over existing records. A read
is scoped to the caller's shared space and returns at most 31 days. The default
month and the cutoff are based on Beijing time. The current month ends at
today, past months include their calendar days, and future months are refused.
The existing garden snapshot and watering rules stay compatible.

The web build and all 1,007 web tests passed. Chrome at a narrow viewport used
a local synthetic API to verify opening the calendar, displaying a partial
current month, navigating to the previous month and returning. Backend run
`usapp-e1-35f1518c1f` passed 1,513 live HTTP assertions and 36 CTest targets.
Release-pair run `usapp-backup-probe-0304efdb7a` passed with migration counts
54 → 58 → 58 and confirmed the prior watering was visible on the calendar.

#### Month jump and daily detail

The calendar accepts a specific month and limits the selection to the current
server month or earlier. Invalid selections leave the loaded calendar in
place. Each date can be selected to inspect both members' watering statuses
from the loaded month. The current day's cell is marked on the current
month. Reading another month clears the selected date.

The web build and all 1,009 web tests passed. A narrow Chrome test with a
local synthetic API verified an invalid future selection, a jump to leap
February, opening a date's detail and returning to the current month.
