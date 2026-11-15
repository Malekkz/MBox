# third-party

The list of third-party dependencies, the licence allow-list, and any vendored
third-party code (reference 13.6, 16.3; decisions D-20 to D-22).

| File | Content |
| --- | --- |
| `dependencies.json` | Every third-party dependency, with version and licence. |
| `licence-allowlist.json` | The licences a dependency may have. |
| `<name>/` | Vendored code of the dependency `<name>`, if it is vendored. |

## What the build does

`cmake/ScadaDependencies.cmake` runs at configure time, and again whenever one
of the two files changes. It stops the build when:

- an entry is malformed, or its licence is not on the allow-list,
- an entry's licence is allowed for some scopes only and the entry has
  another scope,
- a directory here is not named after an entry,
- a target of this project links something that is not a target of this
  project, a `link_names` value of an entry, or a platform library
  (`Threads::Threads`, `pthread`, `m`, `dl`, `rt`, `atomic`).

On success it writes two files to `build/<preset>/reports/`, and
`cmake --install` copies them to `usr/share/doc/scada/` of the staged tree:

| File | Content |
| --- | --- |
| `licence-report.md` | Table of the dependencies and the allow-list. |
| `sbom.cdx.json` | Software bill of materials, CycloneDX 1.5 JSON. |

Not detected: a header-only dependency that is used through an include path
without linking a target, and what vendored or fetched code links itself.
Reviewers must check these.

## Adding a dependency

1. Check the licence in the dependency's own source, not on a web page.
2. Add an entry to `dependencies.json` in the same pull request as the code
   that uses it.
3. Link it by one of the names in `link_names`.
4. If the build fetches it, take the version from the list with
   `scada_dependency_version(<variable> <name>)`, as `cmake/ScadaTesting.cmake`
   does, so that the version is written once.

```json
{
  "name": "example",
  "version": "1.2.3",
  "licence": "MIT",
  "scope": "product",
  "purpose": "What it is used for, one line",
  "url": "https://example.invalid/project",
  "purl": "pkg:github/example/example@1.2.3",
  "link_names": ["Example::example"]
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `name` | yes | Unique name; letters, digits and `. _ + -`. Also the directory name if vendored. |
| `version` | yes | The exact version used. |
| `licence` | yes | One SPDX identifier, matched exactly against the allow-list. For a dependency offered under several licences, the one it is used under. |
| `scope` | yes | `product` (shipped on the unit), `build` (build tool) or `test` (tests only). Only `product` entries are marked as shipped in the bill of materials. |
| `purpose` | yes | What it is used for. |
| `url` | yes | Project home page or repository. |
| `purl` | no | Package URL, used by vulnerability scanners. |
| `link_names` | no | Names as written in `target_link_libraries`: CMake targets or library names. |
| `commit` | no | The commit hash of the source used, 40 hexadecimal characters in lower case, for a dependency built from a source repository. |

Unknown keys are errors. The compiler, its runtime libraries and the C library
are part of the toolchain and are not listed (D-21).

## Changing the allow-list

The allow-list holds permissive and public-domain licences only. GPL, LGPL,
MPL and EPL are not on it until the licensing decision of reference section 16
is made. A change to `licence-allowlist.json` needs review by a person and an
entry in `docs/decisions.md`.

A licence may be allowed for some scopes only. `licence_scopes` maps it to
those scopes; a licence not named there is allowed for every scope:

```json
"licence_scopes": { "CECILL-B": ["build", "test"] }
```

`CECILL-B` is allowed for dependencies of scope `build` and `test` only, not
for anything linked or shipped (D-37, D-39; herdtools7). The check rejects a
`product` entry with it, and the licence report prints the limit.

## How to test it

```
ctest --preset dev -R dependency-check
```

The tests are in `cmake/tests/`. They configure a small project against the
real allow-list and check each way the build must fail.
