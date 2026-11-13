# tests

Tests that span several components: integration tests that start real
processes against simulated devices, and performance tests. Unit tests do not
live here; they live in the `tests/` directory of their component.

## install

Tests of the staged install tree (`docs/install.md`):

- `install-stage` installs the build into `build/<preset>/tests/install/stage`.
- `install-layout` checks that tree against the directory layout of
  reference 4.4.
- `install-units-verify` runs `systemd-analyze verify` on every service unit
  in it. It needs `systemd-analyze` from systemd 250 or newer and fails
  without it.

```
ctest --preset dev -R '^install-'
```
