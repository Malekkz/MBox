<!--
Template for the README.md of a library, service or tool. Copy it to
<component>/README.md, fill in every section and delete this comment.
Keep it short: one screen. Numbers carry units.
-->

# <component name>

One or two sentences: what this component does and which part of the reference
document it implements (section number).

## What it does

- The responsibilities of the component, one line each.
- What it deliberately does not do, if that is likely to be assumed.

## Interfaces

- Contracts it implements or uses (`docs/contracts/<file>`, version).
- Public headers, sockets, files or tags it provides.

## How to run it

```
<command line, with the options most often needed>
```

For a library, show the CMake target to link and a minimal usage example
instead.

## How to test it

```
ctest --preset dev -R <test name pattern>
```

Name the simulator or fixture the integration tests need, if any.
