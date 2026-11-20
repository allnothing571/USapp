# V1 release candidate rebuild

The local Git ref `v1-candidate-20261120` fixes the current V1 source tree. It includes
application sources, the three pnpm lockfiles, contracts, database migrations through
0053, and build/test tooling. Use a clean checkout of this ref. Runtime secrets, live
data and generated outputs must come from their own approved sources.

Web and admin builds use Node 24.21.0 and pnpm 10.34.9. From each application directory
run `pnpm install --frozen-lockfile` and `pnpm build`. The push worker has its own lockfile.

The verified offline API compiler input is the existing local Docker image
`sha256:83afaf6179c2f2c80c73ee209f907773f33a67deac1610905924ed17e66cbfb1`.
The checked-in `tools/export-verify.ps1` builds the API with that image and runs its
disposable PostgreSQL checks. The compiler image is a local dependency and must be
preserved separately for an offline rebuild on another machine. The public Dockerfile
recipe currently resolves some upstream inputs by tag/package repository, so it does
not promise byte-for-byte identical artifacts.

This baseline records source/build inputs. Production data, provider enrollment,
browser/device acceptance, and release readiness are tracked in the V1 completion ledger.
