# IDR Release and Compatibility Policy

Status: Policy for v0.2.0 onward
Last updated: 2026-11-15

This document defines how IDR is versioned, what compatibility promises
come with a release, how features are deprecated, and what must be true
before a release is published.

It describes **policy**, not the current state of the project. The
current state is stated explicitly in section 2.

## 1. Versioning scheme

IDR uses [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html):

```
MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
```

- **MAJOR** increments for incompatible changes to the wire protocol,
  the on-disk WAL or snapshot format, the configuration surface, or
  documented delivery guarantees.
- **MINOR** increments for backward-compatible features, including new
  optional protocol fields and new metrics.
- **PATCH** increments for backward-compatible bug and security fixes.

While the MAJOR version is `0`, the project is pre-1.0. In that range a
MINOR bump may contain breaking changes, as SemVer permits. Breaking
changes in `0.x` still require a changelog entry and a migration note.

Pre-release identifiers (`-rc.1`, `-beta.2`) mark builds that are not
yet recommended for deployment.

### 1.1 What "the interface" means

A release version applies to the whole project, but compatibility is
judged separately for each surface:

| Surface | Examples | Breaking change |
|---|---|---|
| Wire protocol | Message names, field order, framing, `IDR-AUTH-V1` | A peer built for the previous MINOR cannot interoperate |
| On-disk format | WAL lines, snapshot files, manifest | An older binary cannot read the data without migration |
| Configuration | `IDR_*` environment variables, future config file keys, CLI flags | A documented setting is removed or changes meaning |
| Delivery semantics | `SUBMIT_ACK`, `TASK_RESULT_ACK`, `TASK_UNKNOWN`, retention | A documented guarantee becomes weaker |
| Observation | Metrics names, JSON fields, log event names, exit codes | A dashboard, alert, or runbook breaks |

Adding a new optional field, a new metric, or a new log event is a MINOR
change, not a breaking one, provided old peers and old consumers keep
working.

Changing a documented guarantee is always **MAJOR**, even if the code
change is small. Weakening `SUBMIT_ACK` from "durable" to "accepted" is
a breaking change regardless of the version number chosen.

## 2. Current state

As of this document:

- there are no git tags and no published releases;
- the coordinator, worker, and clients are built from source with `make`;
- there is no version string compiled into the binaries and no
  `--version` flag;
- the WAL has no format version or checksum, so the on-disk format is
  explicitly unstable;
- CI builds and runs the test suite on `ubuntu-latest` only.

Treat every commit on `main` as an unreleased development build. Do not
assume any compatibility promise for untagged commits.

The first tagged release will be `v0.2.0`, after Phase 1 (WAL 2.0) lands.
The version is `0.2.0` rather than `0.1.0` because the WAL and delivery
semantics change incompatibly from the current untagged state. Adopting
`v0.1.0` retroactively for an untagged commit would imply a stable
baseline that does not exist.

## 3. Compatibility promises

### 3.1 Within a MAJOR version

For a released `1.x.y`:

- a `1.x` client can talk to a `1.y` coordinator and worker for any
  `x` and `y`, using the protocol version negotiated or documented for
  that MAJOR;
- data written by `1.x` is readable by `1.y` without manual conversion;
- configuration accepted by `1.x` is still accepted by `1.y`, unless it
  was deprecated according to section 4;
- documented delivery semantics are never weakened in a MINOR or PATCH
  release.

Downgrades are not guaranteed. A newer binary may write data that an
older binary cannot read. Operators who need rollback must take a
snapshot before upgrading and follow the runbook for that release.

### 3.2 Across MAJOR versions

A MAJOR release may break the wire protocol, the on-disk format, or the
configuration surface. It must ship with:

- a migration path for on-disk data, or an explicit statement that a
  clean start is required;
- a compatibility matrix showing which coordinator, worker, and client
  versions can interoperate;
- release notes that name each breaking change and the action required.

### 3.3 Mixed-version operation

Mixed-version operation is supported only where the release notes say so.

Current v0.x has no protocol version negotiation beyond the
`IDR-AUTH-V1` marker inside the MAC input. A coordinator and worker
built from different commits may fail in confusing ways. Until explicit
negotiation exists, upgrade the coordinator, workers, and clients
together.

ADR-003 changes worker messages (worker ID, epoch, attempt) in an
incompatible way. That release must be a coordinated upgrade, and its
notes must say so.

### 3.4 Protocol marker

`IDR-AUTH-V1` identifies the authentication scheme. It is inside the MAC
input, so an attacker cannot downgrade a signed message to an older
scheme. Any change to the scheme, the timestamp encoding, or the MAC
input requires a new marker and a documented transition. See
`docs/PROTOCOL.md` section 9.

An `IDR-AUTH-V2` deployment must accept both markers during migration and
retire V1 only when no V1 peers remain.

## 4. Deprecation

A feature is deprecated before it is removed.

1. The release that deprecates a feature still supports it and logs a
   warning when it is used.
2. The deprecation is recorded in `CHANGELOG.md`, the relevant document,
   and the release notes, with the release that will remove it.
3. Removal happens no earlier than the next MAJOR release, or at least
   one MINOR release later for `0.x`.
4. Security fixes may shorten this window. If so, the release notes must
   explain why.

Deprecated surfaces include environment variables, protocol fields,
message names, WAL record types, metrics names, and documented
behaviors.

For `0.x`, a deprecation window of one MINOR release is the minimum. For
`1.x`, removal requires a MAJOR release even if the window was longer.

## 5. Release artifacts

A release consists of:

- an annotated git tag named `v<version>` on a commit that passed CI;
- a GitHub release with the changelog for that version;
- a source archive generated by GitHub from the tag;
- a `SHA256SUMS` file covering every published artifact;
- the exact compiler and platform used for any binary artifact;
- a link to the benchmark report for that release, once Phase 3 exists.

Current status: none of this is automated. Phase 5 adds the changelog,
tagging automation, and checksums. Phase 7 adds signing and reproducible
builds.

Binary artifacts are a convenience, not the primary distribution. The
supported way to build IDR is from source with `make` on Linux.

## 6. Release checklist

A release is published only when every item is true.

### 6.1 Before tagging

- [ ] `make` succeeds from a clean checkout with no warnings treated as
      errors that were suppressed for the release.
- [ ] `make test` passes on the supported compiler matrix.
- [ ] Sanitizer binaries are built **and the test suite is run under
      them**. Building `make asan` alone is not evidence.
- [ ] `clang-tidy`, `cppcheck`, and `scan-build` are clean, or every
      remaining finding is documented.
- [ ] Fuzz smoke tests are clean for the agreed duration (Phase 4
      onward).
- [ ] Crash-point and recovery tests pass for the changed durability
      code.
- [ ] Benchmarks were re-run if the release touches the hot path, and
      the results are recorded with hardware and workload.
- [ ] `CHANGELOG.md` has an entry for this version. Until Phase 5 adds
      the changelog, the GitHub release notes serve as the changelog.
- [ ] Documentation matches the code, including every
      "current behavior" statement.
- [ ] Breaking changes and migrations are called out in the release
      notes.
- [ ] The working tree is clean and the tag points at the reviewed
      commit.

### 6.2 Tagging

```bash
git tag -a v0.2.0 -m "IDR v0.2.0"
git push origin v0.2.0
```

- [ ] The tag is annotated, not lightweight.
- [ ] CI is green on the tagged commit.
- [ ] The tag matches the version recorded in the changelog.

### 6.3 After tagging

- [ ] GitHub release published with notes and `SHA256SUMS`.
- [ ] Artifacts downloaded and verified from the published release.
- [ ] A fresh clone of the tag builds and passes `make test`.
- [ ] A known-good snapshot and WAL from the previous release are
      restored successfully, or the release notes state that migration
      is required.
- [ ] The release is announced with the compatibility matrix.

## 7. Version and documentation mapping

| Version | Content | Status |
|---|---|---|
| untagged `main` | Pre-Phase 0 | Current baseline, not a release |
| v0.2.0 | Phase 0 docs, Phase 1 WAL 2.0, delivery semantics | Planned |
| v0.3.0 | Phase 2 scheduler, worker identity, fencing | Planned |
| v0.4.0 | Phase 3 connections, scale, benchmark harness | Planned |
| v0.5.0 | Phase 4 TLS, per-worker credentials, fuzzing | Planned |
| v0.6.0 | Phase 5 metrics, config, CLI, packaging | Planned |
| v1.0.0 | Production baseline, HA optional per track | Target |

The version numbers are provisional. The ordering is not: a release does
not include work from a later phase unless the earlier phase's exit
criteria in `docs/ROADMAP.md` are met.

### 7.1 Documentation versions

Documents that describe behavior carry one of these status lines:

- **Current** - describes the code on `main` as of the last update;
- **Current v0.x, with v1.0 targets called out** - describes the code and
  marks intended changes;
- **Draft for v1.0** or **Proposed for v1.0** - describes intent, not
  implemented behavior;
- **Policy for v0.2.0 onward** - this document.

A document must not describe unimplemented behavior as if it exists.
When a phase lands, its documents move from "Proposed" to "Current" in
the same change.

## 8. Security releases

- Security fixes may be released as a PATCH without waiting for a MINOR.
- A security release must include the affected versions, the impact, the
  fixed version, and any required operator action.
- Do not publish exploit details before a fixed release is available.
- Before Phase 4 there is no `SECURITY.md` and no formal disclosure
  process. `docs/THREAT_MODEL.md` section 10 describes the interim
  private-reporting expectation. Do not claim a disclosure policy exists
  until it does.

## 9. What this policy does not promise

- It does not promise that `0.x` releases are compatible with each other.
- It does not promise binary compatibility across compilers, libc
  versions, or distributions.
- It does not promise support for old releases indefinitely. Support
  covers the latest MINOR of the current MAJOR; older lines receive
  security fixes only if stated in the release notes.
- It does not promise reproducible builds until Phase 7.
- It does not promise signed artifacts until Phase 7.

## 10. Open questions

These are unresolved and must be decided before `v1.0.0`:

- the exact support window for the previous MAJOR release;
- whether the wire protocol gets an explicit version field in addition
  to `IDR-AUTH-V1`;
- whether WAL and snapshot formats are versioned independently of the
  project version;
- whether binary artifacts are published for multiple distributions or
  source-only;
- the signing mechanism and key custody model.

## References

- `docs/ROADMAP.md` - phases and exit criteria that gate each release
- `docs/PROTOCOL.md` - wire protocol and `IDR-AUTH-V1` versioning
- `docs/SEMANTICS.md` - delivery guarantees that a MAJOR release may change
- `docs/ARCHITECTURE.md` - components and on-disk formats
- `docs/adr/0001-snapshot-and-wal-segmentation.md` - WAL and snapshot formats
- `docs/adr/0003-worker-identity-and-fencing.md` - incompatible worker protocol change
