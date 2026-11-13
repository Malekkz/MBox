# Fixtures for the tests of the check script

These files break the rules on purpose. They are not built, and the `check`
target leaves this directory out. `../check_test.py` runs the checks on them.

A line that must be reported by the banned-function or `SCADA_RT` check ends
with `// expect: <check name>`. The tests compare the reported lines with the
marked lines exactly, so a missed finding and a false finding both fail.
