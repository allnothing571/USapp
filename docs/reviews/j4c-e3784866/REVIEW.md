# J4c resource confirmation - NOT AUTHORIZED

Account UID (owner-confirmed declaration): 1567452047414069
Region: cn-shanghai
Bucket: usapp-j4c-log-e3784866
TableStore instance / table: uj4ce3784866 / journal
Journal: 0afe377a-3e06-4134-997f-4f1a437b8492
Budget: CNY 5, manual stop threshold, NOT an automatic billing cap.

All files are LOCAL PROPOSALS. Names are NOT checked for cloud availability.
No credentials, network calls, resource creation, binding, retention lock or deletion.
The owner declaration is NOT authenticated account/resource ownership evidence.

Runtime identities must be new, programmatic only, with no inherited/extra grants.
Recovery reader uses Start/Abort for transactional reads, but cannot write or commit.
Transaction control calls may consume capacity; data-read-only is not zero-cost.
UpdateRow permission can bypass client head rules; IAM does not enforce monotonicity.

The initial-head file is a PROPOSED SEED, not a trusted latest checkpoint.
Initialize only a NEW, verified-empty whole partition while writers are stopped.
Unknown rows, missing head on an existing journal, old cycles or restored state: STOP.
Initializer and initialization authority/fresh-lineage proof remain unimplemented.

Before creation: review exact config/identities/budget/window/cleanup, obtain separate approval.
After approval: independently verify owner, permissions, never-enabled bucket versioning,
private settings, no lifecycle/replication/retention, exact table options and local transactions.
No live runner, credential management or live initialization is provided by this package.

Cleanup is NOT automated or authorized: enumerate EXACT test keys/rows/resources first,
exclude all pre-existing resources, then obtain the exact cleanup authorization.
Proposed test bounds are review values, not enforced by this package.
Hash verification proves internal consistency, not authenticity, authorization or anti-rollback.
J3, restore/replay, J5 and release gates remain open.
