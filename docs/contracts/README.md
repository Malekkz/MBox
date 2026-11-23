# Interface contracts

A contract fixes how two parts of the system talk to each other, exactly
enough that each side can be built and tested on its own. The reference
architecture says what the system is; the contracts say what crosses each
boundary.

## Rules

- Code follows the contract. If the code needs something the contract does not
  allow, change the contract in the same pull request as the code, in its own
  commit, with a version bump and a migration note. The change is reviewed
  against the code that implements it. A change that needs a decision is
  raised as a question first (`CLAUDE.md`, "When to stop and ask").
- Every contract has a version. A draft is `0.x` and may change freely within
  its phase. From `1.0`, an incompatible change needs a new major version and a
  migration note.
- A contract is written just before the phase that first needs it, by drafting
  from the reference document, and is reviewed by a person before code starts.
- Each contract ends with conformance tests. A phase is accepted against them.

## Status

| Contract | File | Needed by | Status |
| --- | --- | --- | --- |
| Tag database | `tag-database.md` | Phase 1 | 1.0, accepted at the close of phase 1 (D-66) |
| Configuration schemas | `config-schemas.md` | Phase 2 | Outline below |
| Driver interface | `driver-interface.md` | Phase 2 | 0.5, accepted for phase 2 implementation; becomes 1.0 when phase 2 passes its acceptance tests (rulings of 2026-11-23, D-103) |
| Historian file format and upload | `historian-format.md` | Phase 3 | Outline below |
| REST and WebSocket API | `rest-api.md` | Phase 4 | Outline below |
| Node link protocol | `node-link.md` | Phase 11 | Outline below |

The local RPC messages between services (commands, configuration, history
queries) are defined as Protocol Buffers schemas in the repository and are
reviewed with the service that introduces them; they do not have a separate
contract document.

---

## Outline: configuration schemas (phase 2)

Source: reference architecture sections 12, 7.3, 7.4, 7.6, 9.1, 10.1.

The contract is the set of JSON Schema files, one per configuration file type,
plus this document explaining the rules schemas cannot express.

1. File tree and which schema applies to which path.
2. Common definitions: tag name grammar, durations (`100ms`, `5s`, `90d`),
   percentages, references to tags, templates, policies, areas and secrets.
3. `system.yaml`: unit identity, node role, time sync, network, storage
   thresholds, upstream sync, email server.
4. Templates and device instances: how a template expands, how overrides merge,
   how the name prefix is applied.
5. Tags: plain, internal, manual and virtual tags; the expression grammar for
   virtual tags.
6. Logging: aggregate tiers, policies, per-tag overrides, how retention classes
   are derived.
7. Outputs, areas, interlocks, safe states.
8. Alarms.
9. Control: tasks, function block programs (block types, pins, parameters),
   structured-text files.
10. Cross-file validation rules, each with an error code and message (section
    12.5 of the reference document is the starting list).
11. Schema versioning and migration: how an older configuration tree is
    upgraded.
12. How the tag-set definition and its `config_hash` are computed from the
    configuration (this is the input to the tag database contract).
13. Conformance tests: a corpus of valid and invalid configuration trees with
    the expected result for each.

Decisions to make while writing: YAML subset allowed (no anchors or custom
tags is the proposal); whether templates may include other templates; exact
merge order for overrides.

## Outline: driver interface (phase 2)

Drafted as `driver-interface.md` 0.1 (#99); the outline is kept as the
record of what the draft had to cover. The two open decisions are D-81 and
D-82.

Source: reference architecture sections 6 and 8.1.

1. Scope: the boundary between `libscada-driver` and a protocol
   implementation; what the framework does and what the protocol does.
2. The `ProtocolDriver` interface: each call, its inputs and outputs, when it
   is called, on which thread, and what it may block on.
3. Object model: driver, channel, device, point; lifetime and ownership.
4. Point addressing: the protocol-specific address is an opaque structure
   validated by the protocol at `configure`.
5. Samples: value, quality, quality flags, source timestamp; how protocol
   status maps to quality.
6. Scheduling: poll groups, priorities, write-before-read, block-read merging,
   synchronous scan groups.
7. Failure handling: timeouts, retries, demotion and recovery, staleness.
8. Write requests: operations, select-before-operate with and without native
   protocol support, expiry, results, auto/manual/remote source checks.
9. Diagnostics: the system tags every driver publishes, with names and units.
10. Reload: which configuration changes are applied live and which need a
    restart.
11. Simulator requirements: what the simulated device for each protocol must
    support.
12. Conformance tests that every driver must pass, independent of protocol.

Decisions to make while writing: whether a protocol is linked into its driver
executable or loaded as a plugin; threading model for report-driven protocols.

## Outline: historian file format and upload (phase 3)

Source: reference architecture section 9.

1. Directory layout by retention class and tier.
2. Partition file: header, per-tag blocks, footer, checksums, format version.
3. Encodings: timestamps, each value type, quality, gap markers, strings.
4. Aggregate rows: fields and how each is computed, including time-weighted
   average and the good-quality share.
5. Write-ahead log format and the recovery procedure.
6. SQLite index schema: partition catalogue, tag id map across generations,
   sync state.
7. Retention: how classes are derived, when files are deleted, the low-space
   procedure.
8. Tier redefinition: how files written with different tier intervals coexist.
9. Query semantics: raw, aggregated, last-value-before, down-sampling for
   trends.
10. Upload contract: endpoint, authentication, idempotency, ordering,
    resumption, journal batches.
11. Conformance tests, including power-loss tests and a reference reader that
    can decode any valid file.

Decisions to make while writing: compressor and level; block size; timestamp
resolution on disk.

## Outline: REST and WebSocket API (phase 4)

Source: reference architecture sections 11.1 to 11.3, 8, 13.1, 13.2.

The contract is the OpenAPI document plus this document for what OpenAPI does
not express.

1. Versioning, base path, content types, pagination, time format.
2. Authentication: sessions, CSRF, API tokens, re-authentication for critical
   actions.
3. Permission required for every operation, by role and area.
4. Error model: one shape, stable codes, and for refused commands the failed
   check and blocking condition.
5. Resources: tags, history, outputs and command sequence, authority, alarms,
   journal, configuration workflow, screens, system, users, audit.
6. Idempotency and concurrency: which requests can be retried safely; how
   concurrent configuration edits are handled.
7. WebSocket: handshake, subscribe and unsubscribe messages, snapshot then
   changes, batching, heartbeat, back-pressure, reconnect.
8. Rate limits and maximum sizes.
9. Audit: which operations produce which audit record.
10. Conformance tests: a permission matrix test for every operation, and a
    WebSocket test under the reference load.

Decisions to make while writing: session lifetime defaults; whether the
WebSocket gets a binary encoding.

## Outline: node link protocol (phase 11)

Source: reference architecture section 17.

1. Transport: TCP, TLS 1.3, certificate pinning, port, socket options.
2. Framing and encoding: length prefix, Protocol Buffers schema, maximum
   message size.
3. Connection state machine for master and slave, including version and
   manifest mismatch.
4. Messages: Hello, Manifest, Snapshot, Changes, Heartbeat, Command, Result;
   fields, ordering rules, sequence numbers.
5. Export rules: patterns, deadband, minimum interval, priority, batching.
6. Commands: operator and remote-auto sources, select-before-operate across
   the link, expiry, identity carried for audit, the role cap on the slave.
7. Link loss: detection, per-output behaviour, cancellation of selections,
   reconnect and resume.
8. Health tags published at each end.
9. Compatibility: protocol versioning between units running different
   software versions.
10. Conformance tests, including latency measurement and link-cut tests.

Decisions to make while writing: maximum exported tags per slave; behaviour
when a slave's exported tag set changes while connected.
