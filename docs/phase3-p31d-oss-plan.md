# P3-1d: real OSS setup and verification plan (for owner review)

Status (updated 2026-11-04): the plan below was written as planning only on 2026-11-03. **Since then** the adapter passed offline, and the owner reports a basic real OSS and desktop browser flow passed on a Shanghai test bucket ([phase3-plan.md](phase3-plan.md) Section 24.1). Most tests below remain open. Nothing here is a production decision. The combined release stays after all of Phase 3 is verified. Background: [phase3-plan.md](phase3-plan.md) Sections 4, 6, 17 and 21.4.

**Labels.** [Doc] the vendor documents it, with the source in Section 8. [Code] read from this repository. [Measure] not documented, or not trusted until tested. [Rec] my recommendation.

## 1. What the documentation changed

| Finding | Effect |
|---|---|
| [Doc] Object operations are atomic and strongly consistent, a successful upload is immediately readable, and a deleted object immediately ceases to exist | G3 and G4 now have vendor statements. The page speaks of uploads and does not name CopyObject, so both are still tested. G8 is still open: listing is not covered |
| [Doc] POST V4 policy has `eq` on key, `content-type` and `content-length-range` | G1's key, type and size rules are documented |
| [Doc] Delete returns 204 whether or not the object exists; with versioning it adds a delete marker | G6 holds with versioning off. Versioning would change deletion, so it matters to D5 |
| [Doc] New accounts' buckets force download of all file types on default domains; a custom domain with ICP filing bypasses it. `<img>` and presigned URLs are not addressed | **New risk.** See B1 |
| [Doc] IMG throughput: 20 MB/s and 50 QPS in Hangzhou, Shanghai, Beijing, Zhangjiakou and Shenzhen; 2 MB/s and 5 QPS elsewhere | **New input to D1.** A timeline page can ask for up to 60 thumbnails at once ([Code] 20 rows, 3 photos) |
| [Doc] Lifecycle rules can be delayed by "several days or weeks" | T3 is a backstop only, as designed |

**Four gaps found in our own code** ([Code], none changed in this batch):

| # | Gap | Next step |
|---|---|---|
| C1 | `content` passes `real_format` as the type for every variant (`MediaService.cc:950`), but `thumb` and `view` output WebP | Adapter labels variants `image/webp` |
| C2 | `variant=original` is open to anyone who may view the photo, and originals keep GPS. Section 6 says "originals keep it, private" | **Owner decision O8** |
| C3 | `FakeObjectStore` has no form extras, so a client adding fields to a form was never modelled | Test N4 |
| C4 | The plan says signed reads "add nosniff" (Section 4.5). No vendor parameter for this was found | Test I9. If absent, correct the plan |

## 2. Test environment

Everything below is disposable, and separate from any later production bucket.

### 2.1 Bucket

| Setting | Value |
|---|---|
| Bucket | One new private bucket, for this test only. No other data, ever |
| Region | Owner choice O1. [Rec] a region with the higher IMG limit, and the likely production region if known |
| Public access | Block Public Access on, ACL private [Doc] |
| Versioning | Off. A versioned variant runs only if D5 chooses it |
| Encryption | Default server-side encryption on. Exact mode: O3 |
| Replication, CDN, acceleration, website hosting, logging to other buckets | None |
| Lifecycle | One rule: `staging/` expires after 2 days, for T3. **None on `media/`**, matching the design |
| CORS | One rule: the exact origin below, method POST, no wildcard. `<img>` needs no CORS |
| Spend | Owner budget alert, O4 |
| Data | Synthetic images only. Section 4.8 forbids real photos anywhere until deletion exists |

### 2.2 Browser origin

[Rec] `https://localhost:18443`, served by a disposable Caddy in the test stack. HTTPS is needed for the `__Host-` session cookie. **Not** `https://10.6.245.114:8443`, because that is the running local deployment. CORS and CSP name this origin exactly.

### 2.3 Identities: minimum permissions

Three RAM users with no console access, plus the owner's own setup identity, which the application never receives. RAM users start with no permissions [Doc].

| Identity | Used for | Allow | Explicit Deny |
|---|---|---|---|
| `usapp-p31d-upload-signer` | `signUpload` only; local signing | `oss:PutObject` on `{bucket}/staging/*` | `oss:*` on `{bucket}/media/*` |
| `usapp-p31d-read-signer` | `signRead` only; local signing | `oss:GetObject` on `{bucket}/media/*` | `oss:*` on `{bucket}/staging/*` |
| `usapp-p31d-server` | head, imageInfo, copy, remove, list | `oss:GetObject` on `staging/*` and `media/*`; `oss:PutObject` on `media/*`; `oss:DeleteObject` on `staging/*` and `media/*`; `oss:ListObjects` on the bucket with `oss:Prefix` limited to `staging/*` and `media/*` | Bucket configuration actions |

| Action | Identity | Prefix | Source |
|---|---|---|---|
| Browser POST | upload-signer's signature | exact `staging/...` key | [Doc] policy `eq` |
| HEAD | server | staging, media | [Doc] `oss:GetObject` |
| Copy | server | read staging, write media | [Doc] GetObject, PutObject |
| `image/info` | server | media | [Measure] GetObject alone is expected to suffice |
| Variant read | read-signer's URL | media | [Doc] GetObject; [Measure] that no IMG-specific action is needed |
| Remove | server | staging, media | [Doc] `oss:DeleteObject` |
| List | server | staging, media | [Doc] `oss:ListObjects` with `oss:Prefix`; never an empty prefix |

### 2.4 Credentials, with no values in chat, source or logs

1. The owner creates the keys, and fills a file **outside the repository**, for example `%USERPROFILE%\.usapp-p31d\oss-test.env`, readable only by that Windows user. I never see, ask for or print the values. I would supply only a template with empty names, and the path.
2. The harness gets the file's **path** as a parameter, loads it into its own process, and passes values to containers by name (`-e NAME`). It never reads the repository `.env`, and uses the existing `--no-interpolate` config check.
3. The harness's existing log scan is extended to fail the run if any value, or an AccessKey id, appears in retained logs. Signed URLs and forms are never logged: test failures print fixed labels only, as the current suites do.
4. **Time-boxed:** [Rec] the owner creates the keys right before a run, then disables and deletes them after it, along with the bucket. Alternative: STS tokens, which are truly temporary, but limit each signed URL to the token's life [Doc] and add a security-token path to the adapter and its tests. That is O5.
5. Containers hold the values in their environment, visible to `docker inspect` on this machine. Hence retained containers must be removed with the keys already disabled, by the owner's choice. This is the one exception to "retain everything" in past runs.

## 3. Guarantees: positive and negative tests

**Method.** One conformance suite runs against both `FakeObjectStore` and the real adapter wherever both can do it. A test-only wrapper around the real adapter injects faults such as a lost response. Vendor behaviour that cannot be proved by a test is reported as observed, with the sample size.

| # | Guarantee | Positive | Negative | Status |
|---|---|---|---|---|
| G1 | Grant writes only its exact staging key | Browser and runner POST to the key within type and size | **N1** key changed to a final key; **N2** another staging key; **N3** oversize, undersize, wrong type, altered policy, expired; **N4** extra fields: `x-oss-object-acl`, `x-oss-meta-*`, `Content-Disposition`, `callback`, `success_action_redirect`, tagging | Rules [Doc]; N4 [Measure] |
| G1 | In-flight bound | | Throttled POST started before expiry and finished after, at growing delays | [Measure]; a cost, not a blocker |
| G2 | Client and server identities separated | Each identity's allowed action works | PutObject on `media/` as upload-signer; GET staging as read-signer; signed PUT or DELETE from read-signer; list as a signer; list an empty prefix as server; bucket configuration as server. All must be 403 | [Measure] policies |
| G3 | Copy atomic | Copied bytes' CRC64 equal the source; 200 parsed as a real `CopyObjectResult` | **Overwrite during promotion:** a live grant re-POSTs staging with different 20 MB content during the copy, 200 rounds. The final object's CRC64 must equal exactly one version, never neither | [Doc] general atomicity; copy [Measure]. **Observational: a pass is evidence, not proof** |
| G3 | Copy success, response lost | Wrapper calls the real copy, then reports Unavailable: attempt stays unused, retry takes a new key, READY names it, the sweep removes the lost copy | The READY key survives the sweep byte-identical | Logic tested; real objects [Measure] |
| G3 | 200 with an error body | Adapter treats anything but `CopyObjectResult` as Unavailable | Stub returns 200 with an `Error` body | [Doc] silent; stub test only |
| G3b | Each copy sent once | Local HTTP stub counts one request per copy | Stub resets the connection before and after reading the request, and holds past the deadline: still one request, and **zero bytes after the deadline**, as the push transport's test proves | [Code] Drogon has no cancel, and a queued request is sent on connect ([phase2-web-push.md](phase2-web-push.md)). Retry behaviour [Measure] |
| G4 | Read after copy | HEAD and `image/info` immediately after copy, 500 samples | Any 404 or stale result is counted and reported | [Doc] strong consistency; copy [Measure] |
| G5 | ETag changes with bytes | Same bytes copied: record whether ETags match | Different bytes: ETag and CRC64 both differ | [Doc] ETag undefined on HEAD; CRC64 documented. [Measure] |
| G6 | Delete idempotent | Delete twice: 204 both; HEAD 404 | Delete an absent key: 204 | [Doc] |
| G7 | Signed read: one key, one variant, read only | Each variant serves the right image; type and cache headers recorded | **Altered:** another key, another `x-oss-process`, process removed or added, `response-*` changed, expiry extended, method PUT or DELETE. All must fail | Example URL carries the parameter; whether it is signed [Measure]. **Blocks if not** |
| G7 | Signed read expiry | Works before `x-oss-expires`, with a 5-minute floored date | 403 just after expiry. Key disabled: record what existing URLs do | Expiry [Doc]; disabled keys [Measure] |
| G8 | Listing shows everything | Paginate more than 1000 keys; every key once | A key written, then listed repeatedly: record delay. A deleted key: record delay | [Doc] silent on consistency. [Measure] |
| Late objects | Cleanup after a late write | Staging re-POSTed after the sweep with a live grant; final key copied after the sweep by the wrapper; reconciler removes each and records it | READY key and `unknown` keys never removed; no signed read for a late key | Logic tested; real listing [Measure] |

**Timing gates**

| # | Gate | Test | If it fails |
|---|---|---|---|
| T1 | `sweepGraceSeconds` (1 h) outlasts late writes | Longest accepted in-flight POST and slowest copy observed | Raise grace. Cost only |
| T2 | How late a failed copy can land | **Cannot be proved.** Record what is seen; the reconciler covers it | None; nothing depends on a bound |
| T3 | Staging lifecycle rule | Object past 2 days is gone; `media/` untouched. **Takes 3-5 days of wall time**, and [Doc] can take longer | Reconciler alone; report |
| T4 | Bounded store calls | Each call against a stub that accepts and never answers, one that never accepts, and a slow body: returns by its deadline, and the next cleanup round runs. Real copy timings set the deadlines: p99 of a 20 MB copy, `image/info` and list | **Blocks** |

[Rec] starting deadlines, to be set from those measurements: HEAD, delete and `image/info` 10 s; list 20 s; copy 60 s.

## 4. Images and browser

Every sample is synthetic, except I5.

| # | Check | Pass |
|---|---|---|
| I1 | JPEG (baseline, progressive, CMYK), PNG (8-bit, 16-bit, alpha), WebP (lossy, lossless, alpha) | `image/info` format and dimensions match; READY; every variant renders |
| I2 | Refused: truncated files, text renamed `.png`, SVG, HTML, HEIC, GIF, a declared type that differs from the bytes, over 30000 px or 250 MP | REJECTED with the right reason. **A store outage is never a rejection**: record the exact non-image error, which the docs do not state, and map it to `Invalid` only on evidence |
| I3 | Animated WebP | [Doc] needs a ticket to decode. Record behaviour; adapter refuses `FrameCount` above 1 |
| I4 | Orientation 1-8 at 2000 px, and at 4032, 4097 and 8000 px | Upright in `thumb` and `view`. [Doc] rotation sources limited to 4096 px; auto-orient's relation to it is not stated. **Blocks if sideways, or if it fails** |
| I5 | One real phone JPEG, of a blank wall, location on, taken for this test | Same; **needs owner consent, O7**, since Section 4.8 forbids real photos |
| I6 | Metadata: JPEG with EXIF, GPS, XMP and a comment; PNG `eXIf` and text chunks; WebP `EXIF` and `XMP` chunks | Variant bytes contain **no GPS**, checked by parsing the bytes with a small test helper, not by `image/info`. **Blocks if GPS survives** in anything clients receive. `original`: see O8 |
| I7 | Dimensions | Whether `image/info` reports stored or oriented size [Measure]. Stored facts must be consistent with the UI's use |
| I8 | Burst | 60 thumbnails at once from the chosen region, then from a 5 QPS region if relevant: count failures and time. Per-account or per-bucket scope [Measure] |
| I9 | Headers on each variant | Type, `Cache-Control`, `Content-Disposition`, `X-Content-Type-Options`, recorded. `response-content-type` with `x-oss-process` works |

**Browser**, using the real web build, a real API process and the test bucket:

| # | Check | Pass |
|---|---|---|
| W1 | CSP | Test Caddy adds exactly the bucket origin to `connect-src` and `img-src`. Upload and images work. **Negative:** with today's CSP (`connect-src 'self'`) both fail, shown in the console |
| W2 | CORS | Exact origin works. **Negative:** another origin's POST response is blocked. Note that a blocked response may still have stored the bytes, and must show as `interrupted`, never as success |
| W3 | Upload | Grant, POST, completion, READY, in Chromium, and on a real iPhone and Android phone if the owner agrees. No cookie or CSRF header reaches OSS (Network panel) |
| W4 | Read | `<img>` renders through the 302 on the default domain, even with forced-download headers. **See B1** |
| W5 | Service worker | A **production build** with the worker registered: `caches` holds no photo, form or signed URL after a full flow. Vite dev could not show this |
| W6 | Failures | Interrupted upload, disabled network, expired grant, in the real UI |
| W7 | iOS Safari HEIC | [Assumption] converts to JPEG when `accept` omits HEIC. Device check |
| W8 | Real HTTP layer | The 201 and 302 paths, which have never run through a real process |

**Vendor behaviour that would block release until resolved**

| # | Risk | Falls back to |
|---|---|---|
| B1 | `<img>` will not render from a forced-download default domain | A custom domain, which needs ICP filing and a certificate, so **D1 becomes a hard dependency** |
| B2 | `x-oss-process` not covered by the signature | Styles, or redesign |
| B3 | GPS or EXIF survives in variants | Section 6's order: other parameters, then browser normalization (D3) |
| B4 | Auto-orient fails or misrenders above 4096 px | Section 6's order. Modern phone sensors can exceed this |
| B5 | A grant holder can set `callback`, public ACL or metadata that reaches READY | Policy conditions, copy with `REPLACE` metadata; redesign if refusal is impossible |
| B6 | Non-image input cannot be told from an outage | Design review: it would make REJECTED unreliable |
| B7 | Store calls cannot be bounded, or a copy is resent | Adapter redesign; G3b is a stop condition |
| B8 | IMG limits make a normal timeline fail | Region (D1), fewer eager thumbnails, or styles |

Not blocking, but recorded: in-flight expiry bound, lifecycle delay, nosniff availability, late-copy bound.

## 5. Next batch: smallest changes

**Reuse:** Drogon `HttpClient`, OpenSSL HMAC already linked, and the `WorkerTransport` per-call pattern, with its deadline and socket shutdown. [Rec] **No vendor SDK, no new dependency, no Node or sharp service, no migration, no contract change.** I have not found an SDK requirement that would override this. An SDK's own retry layer would also put G3b at risk.

| Change | Content |
|---|---|
| `services/api/src/services/OssObjectStore.{h,cc}` | Implements the seven `ObjectStore` calls. Pure functions for V4 header, query and POST-policy signing, tested with fixed vectors. Own event loop; one client per call; a deadline on every call; no retry; response size cap; certificate validation explicitly on; keys checked against the expected character set. The existing HTTP client path is plain `http`, so **HTTPS through it has never run here** |
| Copy | `x-oss-metadata-directive: REPLACE` with fixed metadata, so nothing a grant holder set reaches READY. Success only on a parsed `CopyObjectResult` |
| `signUpload` | Exact `eq` conditions for bucket, key and type; `content-length-range` set to the granted size; fixed `success_action_status`. No `${filename}`; extras per N4 |
| `signRead` | Variant strings from Section 6; `image/webp` for variants (C1). Signing date derived from the already floored expiry |
| Configuration | `MEDIA_ENABLED` default false, `OSS_ENDPOINT`, `OSS_REGION`, `OSS_BUCKET`, three identity pairs. Partial configuration refuses to start with media on, naming variables, never values. `.env.example` placeholders. `main.cc` calls `MediaService::configure` |
| Fake | **Stays** as the default for CTest and repeatable suites. Conformance tests take either store |
| New test target | `oss_live`, **separate because** it needs network, credentials, real time and money. Not in default CTest; refuses unless explicitly enabled. Existing targets and the 30 s limit are unchanged. The local HTTP stub tests for G3b and T4 are offline and join `unit` or `push_dispatch`-style limits |
| Harness | `p31d-isolated-verify.ps1`: only the API and live runner gain a second network with egress; PostgreSQL stays internal, with no published ports except the test Caddy on localhost |
| Test CSP | A test-only Caddyfile. **`deploy/Caddyfile` and `Caddyfile.lan` stay unchanged** until the release batch |

**Test sequence.** Fail fast; each stage is reported before the next. Stop at any blocker.

1. Offline: signing vectors, HTTP stub tests (G3b, T4), conformance against the fake. No cloud access.
2. Owner completes the checklist. Harness checks only that the names exist.
3. Bucket configuration read back: private, block on, versioning off, CORS, lifecycle.
4. G2 identity negatives, then G1 with N1-N4. **Stop if a grant reaches another key.**
5. G6, G4, G5, G3 (normal), G7, G8.
6. Promotion races: overwrite during copy, lost response, late objects, sweep and reconciler.
7. Images: I1-I4, I6-I9, I5 if consented.
8. Browser: W1-W8.
9. Timing: T1, T4 measured deadlines set, full suite rerun; T3 after its wait.
10. Report results per guarantee. Owner disables keys and deletes the bucket.

## 6. Owner checklist

Before the next batch starts. Provide choices in chat, and **never** credential values.

| # | Choose | [Rec] |
|---|---|---|
| O1 | Test region | The likely production region, with the higher IMG limit |
| O2 | Account: the one to be used in production, or a separate one | **The real account**, since [Doc] forced-download rules depend on when the account was activated |
| O3 | Encryption: OSS-managed or KMS | OSS-managed for the test; KMS needs extra permissions |
| O4 | Spend limit and alert | Owner's figure. Test data is small; cost is unpriced here |
| O5 | Time-boxed AccessKeys, or STS | Time-boxed AccessKeys |
| O6 | Test origin | `https://localhost:18443` |
| O7 | One non-personal real phone photo for I5, and devices for W3 and W7 | Yes, a blank wall |
| O8 | Who can read `original` through the API, given GPS | Only its uploader; variants for the partner. Needs a small service change if approved |
| O9 | Custom domain for the test, if B1 fires | Decide only if it fires |
| O10 | Key file path | `%USERPROFILE%\.usapp-p31d\oss-test.env` |

Then, in order:
- [ ] Create the bucket as in 2.1, and read back its settings
- [ ] Create the three RAM users and policies of 2.3. Keep the owner's own identity out of the file
- [ ] Create AccessKeys, and write them only to the key file
- [ ] Restrict the file to your Windows user
- [ ] Tell me: "ready", with O1-O10 and the bucket and region names. **No key ids, secrets or tokens**
- [ ] After the run: disable keys, delete keys, remove the retained containers, delete the bucket

## 7. Production decisions: not part of the test setup

| Decision | Why it waits | What the test informs |
|---|---|---|
| D1 Vendor and production region | Filing, latency, IMG limits | B1, B8, I8 |
| Custom domain, ICP filing, certificate | Depends on D1 | B1 |
| D5 Replication and versioning | Versioning changes delete semantics [Doc], and `x-oss-forbid-overwrite` is ignored with it; neither is relied on | G6 rerun on a versioned bucket if chosen |
| D6a-d Deletion, recovery window, backup bound, journal | **Real photos stay refused until implemented** (4.8) | Delete behaviour |
| Joint database-and-object restore drill (4.9) | Needs D5, and deletion for its journal case. **Release gate. Not satisfied by this test** | CRC64 facts |
| Production identities, key storage, rotation, encryption mode, budgets | Security decisions for real data | Permission set from 2.3 |
| CSP and CORS for the production origin | Ship with the combined release | W1, W2 |
| `MEDIA_ENABLED` on | After live checks, in the combined release | All of this plan |

**What this test cannot establish:** vendor guarantees under failure conditions we cannot create, behaviour in another region, or long-term durability. G3 and T2 remain partly observational, and the report will say so.

## 8. Sources

Read 2026-11-03. Pages may disagree or change; each [Doc] above is re-checked by a test where it matters.

- [What is OSS: atomicity and consistency](https://www.alibabacloud.com/help/en/oss/user-guide/what-is-oss)
- [PostObject](https://www.alibabacloud.com/help/en/oss/developer-reference/postobject)
- [POST V4 policy](https://www.alibabacloud.com/help/en/oss/developer-reference/signature-version-4-recommend)
- [CopyObject](https://www.alibabacloud.com/help/en/oss/developer-reference/copyobject)
- [HeadObject](https://www.alibabacloud.com/help/en/oss/developer-reference/headobject)
- [DeleteObject](https://www.alibabacloud.com/help/en/oss/developer-reference/deleteobject)
- [ListObjectsV2](https://www.alibabacloud.com/help/en/oss/developer-reference/listobjectsv2)
- [Presigned URLs](https://www.alibabacloud.com/help/en/oss/user-guide/how-to-obtain-the-url-of-a-single-object-or-the-urls-of-multiple-objects)
- [RAM policies and prefixes](https://www.alibabacloud.com/help/en/oss/user-guide/access-control-base-on-ram-policy)
- [Lifecycle rules](https://www.alibabacloud.com/help/en/oss/user-guide/lifecycle-rules-based-on-the-last-modified-time)
- [Forced download and preview](https://www.alibabacloud.com/help/en/oss/how-to-ensure-an-object-is-previewed-when-you-access-the-object)
- [Block Public Access](https://www.alibabacloud.com/help/en/oss/developer-reference/putbucketpublicaccessblock)
- [Access control overview](https://www.alibabacloud.com/help/en/oss/user-guide/permissions-and-access-control-overview)
- [IMG limits](https://www.alibabacloud.com/help/en/oss/user-guide/overview-17/)
- [Format conversion](https://www.alibabacloud.com/help/en/oss/user-guide/convert-image-formats-2)
- [Auto-orient](https://www.alibabacloud.com/help/en/oss/user-guide/auto-rotate-4)
- [Image info and EXIF](https://www.alibabacloud.com/help/en/oss/user-guide/query-the-exif-data-of-an-image-4)
