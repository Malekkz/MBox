"""Which translation units clang-tidy must lint after a change.

The `tidy` check of scada_check.py lints every translation unit by default.
Given a base revision, it lints only the units a change could have affected
(docs/decisions.md, D-66):

- every changed translation unit, and every translation unit whose
  dependencies, as the compiler reports them, contain a changed file;
- everything, when a file changed that sets the rules of the lint or of the
  build (FULL_LINT_PATTERNS), when a header was deleted or renamed, and when
  the changed set or the dependencies cannot be worked out.

Nothing here decides that a unit is clean. An error never makes the set
smaller: it makes it everything.
"""

import concurrent.futures
import dataclasses
import fnmatch
import pathlib
import shlex
import subprocess

# A change of any of these lints everything. Paths are relative to the source
# directory, with "/" as separator; a pattern without "/" matches the file
# name in any directory.
FULL_LINT_PATTERNS = (
    ".clang-tidy",  # the lint rules
    "CMakeLists.txt",  # build files: flags, include paths, definitions
    "*.cmake",
    "CMakePresets.json",
    "CMakeUserPresets.json",
    "cmake/*",  # includes the toolchain files, cmake/toolchains/
    "third-party/*",  # pinned dependencies, their headers and flags
    ".github/workflows/*",  # the pipeline
    ".github/actions/*",  # the setup action: the LLVM version
    "tools/check/*",  # the check itself, its rules and this selection
    "*.in",  # inputs of configure_file, which can generate headers
)

# A deleted or renamed file with one of these suffixes may have been included
# by a unit that has not changed. Its former dependents are not known from
# the dependencies after the change, so everything is linted.
INCLUDED_SUFFIXES = (".h", ".hpp", ".hh", ".hxx", ".inc", ".ipp", ".tcc",
                     ".def")


class SelectionError(Exception):
    """The changed set or the dependencies could not be worked out."""


@dataclasses.dataclass(frozen=True)
class Change:
    """One changed file. `old_path` is set for a rename or a copy."""
    status: str  # First letter of git's status: A, C, D, M, R, T.
    path: str
    old_path: str = ""


@dataclasses.dataclass(frozen=True)
class Selection:
    everything: bool
    reason: str
    units: tuple = ()  # Only when not everything: units to lint, sorted.


def everything(reason):
    return Selection(everything=True, reason=reason)


def matches_full_lint(path):
    """True if a change of `path` (relative, "/"-separated) lints all."""
    name = path.rsplit("/", 1)[-1]
    for pattern in FULL_LINT_PATTERNS:
        if "/" in pattern:
            if fnmatch.fnmatchcase(path, pattern):
                return True
        elif fnmatch.fnmatchcase(name, pattern):
            return True
    return False


def select_units(changes, dependencies, source_dir):
    """Returns the Selection for `changes`.

    `dependencies` maps each translation unit of the check (an absolute,
    resolved path) to the set of files it depends on, itself included.
    Paths of `changes` are relative to `source_dir`.
    """
    removed = []
    present = []
    for change in changes:
        if change.status in ("R", "C"):
            if change.status == "R":
                removed.append(change.old_path)
            present.append(change.path)
        elif change.status == "D":
            removed.append(change.path)
        else:
            present.append(change.path)

    for path in removed + present:
        if matches_full_lint(path):
            return everything(f"{path} changed, which sets the rules")
    for path in removed:
        if path.endswith(INCLUDED_SUFFIXES):
            return everything(f"{path} was deleted or renamed; the units "
                              "that included it are not known")

    changed = {(source_dir / path).resolve() for path in present}
    units = sorted(
        unit for unit, depends_on in dependencies.items()
        if unit in changed or not changed.isdisjoint(depends_on))
    return Selection(everything=False, reason="changed files",
                     units=tuple(units))


def run_git(source_dir, *arguments):
    try:
        result = subprocess.run(
            ["git", "-C", str(source_dir), *arguments],
            capture_output=True, text=True, check=False)
    except OSError as error:
        raise SelectionError(f"cannot run git: {error}") from error
    if result.returncode != 0:
        raise SelectionError(f"'git {' '.join(arguments)}' failed: "
                             f"{result.stderr.strip()}")
    return result.stdout


def parse_name_status(text):
    """Parses the output of `git diff --name-status -z`."""
    fields = text.split("\0")
    if fields and fields[-1] == "":
        fields.pop()
    changes = []
    index = 0
    while index < len(fields):
        status = fields[index][:1]
        if status in ("R", "C"):
            if index + 2 >= len(fields):
                raise SelectionError("cannot parse the list of changes")
            changes.append(Change(status, fields[index + 2],
                                  fields[index + 1]))
            index += 3
        elif status in ("A", "D", "M", "T"):
            if index + 1 >= len(fields):
                raise SelectionError("cannot parse the list of changes")
            changes.append(Change(status, fields[index + 1]))
            index += 2
        else:
            # U (unmerged), X (unknown) or anything new: not understood.
            raise SelectionError(
                f"unknown change status '{fields[index]}' in the list of "
                "changes")
    return changes


def changes_since(source_dir, base):
    """Files changed between the merge base of `base` and HEAD and the work
    tree, untracked files included. Raises SelectionError if that cannot be
    worked out."""
    shallow = run_git(source_dir, "rev-parse", "--is-shallow-repository")
    if shallow.strip() != "false":
        raise SelectionError("the clone is shallow")
    merge_base = run_git(source_dir, "merge-base", base, "HEAD").strip()
    if not merge_base:
        raise SelectionError(f"no merge base of '{base}' and HEAD")
    changes = parse_name_status(run_git(
        source_dir, "diff", "--name-status", "-z", "--find-renames",
        "--no-ext-diff", merge_base, "--"))
    untracked = run_git(source_dir, "ls-files", "-z", "--others",
                        "--exclude-standard")
    changes += [Change("A", path) for path in untracked.split("\0") if path]
    return merge_base, changes


def parse_make_dependencies(text, directory):
    """Paths of a make rule as `-M` writes it, resolved against
    `directory`. The target is left out."""
    text = text.replace("\\\n", " ")
    target_end = text.find(": ")
    if target_end < 0:
        if text.rstrip().endswith(":"):
            target_end = len(text.rstrip()) - 1
        else:
            raise SelectionError("cannot parse the dependency output")
    paths = []
    current = []
    characters = iter(text[target_end + 1:])
    for character in characters:
        if character == "\\":
            following = next(characters, "")
            if following in (" ", "#", "\\"):
                current.append(following)
            else:
                current += [character, following]
        elif character == "$":
            following = next(characters, "")
            current.append("$" if following == "$" else character + following)
        elif character.isspace():
            if current:
                paths.append("".join(current))
                current = []
        else:
            current.append(character)
    if current:
        paths.append("".join(current))
    return {(directory / path).resolve() for path in paths}


# Options of a compile command that name or shape its output. They are left
# out, so that the compiler writes only the dependency list, to stdout.
_OUTPUT_OPTIONS_WITH_VALUE = ("-o", "-MF", "-MT", "-MQ")
_OUTPUT_OPTIONS = ("-c", "-S", "-E", "-M", "-MM", "-MD", "-MMD", "-MG", "-MP")


def dependency_command(entry):
    """The compile command of `entry` changed to print its dependencies."""
    if "arguments" in entry:
        arguments = list(entry["arguments"])
    else:
        arguments = shlex.split(entry["command"])
    command = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
        elif argument in _OUTPUT_OPTIONS_WITH_VALUE:
            skip = True
        elif argument in _OUTPUT_OPTIONS or any(
                argument.startswith(option) and len(argument) > len(option)
                for option in _OUTPUT_OPTIONS_WITH_VALUE):
            pass
        else:
            command.append(argument)
    # -M: the dependencies, system headers included, instead of an object.
    return command + ["-M"]


def dependencies(entries, units, jobs):
    """Maps each unit in `units` to the files it depends on, from the
    compiler's own dependency output (`-M`) for its compile command in
    `entries` (compile_commands.json). Raises SelectionError if a unit has
    no command or the compiler fails."""
    by_unit = {}
    for entry in entries:
        try:
            directory = pathlib.Path(entry["directory"])
            by_unit[(directory / entry["file"]).resolve()] = entry
        except (KeyError, TypeError) as error:
            raise SelectionError(
                f"bad entry in the compile commands: {error}") from error

    def one(unit):
        entry = by_unit.get(unit)
        if entry is None:
            raise SelectionError(f"no compile command for '{unit}'")
        directory = pathlib.Path(entry["directory"])
        try:
            result = subprocess.run(dependency_command(entry), cwd=directory,
                                    capture_output=True, text=True,
                                    check=False)
        except (OSError, ValueError) as error:
            raise SelectionError(
                f"cannot list the dependencies of '{unit}': {error}") \
                from error
        if result.returncode != 0:
            raise SelectionError(
                f"the compiler cannot list the dependencies of '{unit}': "
                f"{result.stderr.strip()}")
        return unit, parse_make_dependencies(result.stdout, directory)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        return dict(pool.map(one, units))
