# Writes the hand-made seeds of sample_processing_fuzz into the corpus
# directory given (#165): each holds a path the fuzz target must reach (a
# substitute and the value after it, a young and an expired hold, health
# and time companions, a quality rule). The byte layout is the one
# sample_processing_fuzz.cpp reads: a 10-byte header and 28-byte samples.
#
#   python3 -I sample_processing_seeds.py corpus/sample_processing_fuzz
#
# It does not make the nine older seeds of the corpus: float-raw-on-integer-tag,
# negative-zero-written, sequence-a to sequence-d, string-invalid,
# string-overlong and string-utf8. They came from earlier fuzzing and hand
# edits, were long enough to run, and are kept as they were, with the two
# header bytes the target gained in #165 inserted as 0 (no companion rules).
#
# Nor the three `found-*` seeds: inputs the fuzzer found when STRING tags
# were turned on (#169), kept as they were found, as regression tests.
import struct, sys, os
out = sys.argv[1]

def header(tag=1, choice=0, gain=0, offset=0, exponent=0, marker=0, low=0, high=1, features=0, status=1):
    return bytes([tag, choice, gain, offset, exponent, marker, low, high, features, status])

KIND = {'bool': 0, 'signed': 1, 'unsigned': 2, 'float': 3, 'string': 4, 'time': 5}
WIDTH = {0: 0, 1: 1, 8: 2, 16: 3, 32: 4, 64: 5}
OUTCOME = {'value': 0, 'no_value': 1, 'absent': 2, 'device_invalid': 3, 'comm_fail': 4}

def sample(kind='signed', width=16, outcome='value', quality=0, number=0, flt=0.0, pattern=None, flt_bits=None):
    if pattern is None:
        pattern = number & ((1 << width) - 1) if width < 64 else number & (2**64 - 1)
    if kind == 'float':
        bits = struct.unpack('<Q', struct.pack('<d', flt))[0]
        pattern = bits if width == 64 else struct.unpack('<I', struct.pack('<f', flt))[0]
        fbits = bits
    else:
        fbits = 0 if flt_bits is None else flt_bits
    # Bit 0 of the last byte (the top byte of the float field) is the health
    # companion's status when the header does not make it good (#169).
    b = bytes([KIND[kind], WIDTH[width], OUTCOME[outcome], quality])
    b += struct.pack('<Q', pattern & (2**64 - 1))
    b += struct.pack('<Q', number & (2**64 - 1))
    b += struct.pack('<Q', fbits)
    assert len(b) == 28
    return b

SUBSTITUTE = 5   # 5 % 3 == 2: action SUBSTITUTE, pattern 5
KEEP_LAST = 4    # 4 % 3 == 1: action KEEP_LAST, pattern 4
MARKER = 0x10    # HAS_MARKER; choice % 4 == 0: max_age 1 s

seeds = {
    # A value, a substitute, a COMM_FAIL that keeps it, a value again.
    'substitute-then-value': header(marker=SUBSTITUTE, choice=MARKER)
        + sample(number=42) + sample(number=SUBSTITUTE)
        + sample(outcome='comm_fail') + sample(number=43),
    # A substitute before any value, then a value.
    'substitute-first': header(marker=SUBSTITUTE, choice=MARKER)
        + sample(number=SUBSTITUTE) + sample(number=44),
    # keep_last: a value, a young hold, an expired hold, a value.
    'keep-last-young-then-expired': header(marker=KEEP_LAST, choice=MARKER)
        + sample(number=42) + sample(number=KEEP_LAST)
        + sample(number=KEEP_LAST) + sample(number=43),
    # DEVICE_INVALID held UNCERTAIN, then expired, then a value.
    'device-invalid': header(choice=MARKER)
        + sample(number=42) + sample(outcome='device_invalid', quality=1)
        + sample(outcome='device_invalid', quality=1) + sample(number=43),
    'comm-fail': header() + sample(number=42) + sample(outcome='comm_fail'),
    'value-int16': header() + sample(number=-1234),
    'negative-64': header() + sample(width=64, number=-(2**62)),
    'marker-all-ones': header(marker=0xFF, choice=MARKER, tag=1)
        + sample(width=8, number=0xFF, pattern=0xFF) + sample(width=8, number=7),
    'float-huge': header(tag=3) + sample(kind='float', width=64, flt=1.0e300)
        + sample(kind='float', width=64, flt=2.5),
    'float-nan': header(tag=3, choice=MARKER) + sample(kind='float', width=64, flt=1.5)
        + sample(kind='float', width=64, flt=float('nan')),
    'time': header(features=0b11011, marker=3)
        + sample(kind='time', number=1_800_000_000 * 10**9)
        + sample(number=1_800_000_001 * 10**9),
    # Health and a time companion with its own device time, a quality rule.
    'companions-good': header(features=0b11011, marker=3)
        + sample(number=1_800_000_000 * 10**9) + sample(number=1_800_000_002 * 10**9),
    'quality-rule-uncertain': header(features=0b00100, gain=1, offset=0, exponent=3)
        + sample(number=0x30) + sample(number=0x31),
    # A value, a substitute, then a young DEVICE_INVALID hold of the value
    # read: held without SUBSTITUTED (max_age 3 s: choice % 4 == 2).
    'substitute-then-device-invalid-hold': header(marker=SUBSTITUTE, choice=MARKER | 2)
        + sample(number=42) + sample(number=SUBSTITUTE)
        + sample(outcome='device_invalid', quality=1),
    # A substitute, then a refused raw value of width 0: the substitute and
    # its flag are kept.
    'substitute-then-refused': header(marker=SUBSTITUTE, choice=MARKER)
        + sample(number=SUBSTITUTE) + sample(width=0, number=7, pattern=0),
    # An UNCERTAIN time companion whose flags (the raw pattern) hold
    # SUBSTITUTED: the point takes UNCERTAIN, never the flag. Byte 2 is 10:
    # the outcome VALUE (10 % 5) and the companion UNCERTAIN (10 % 3).
    'uncertain-companion-with-substituted': header(features=0b10001, marker=3)
        + bytes([1, 3, 10, 0]) + sample(number=1_800_000_000 * 10**9, pattern=0x8)[4:],
    # Header byte 0: the tag type is 1 + byte % 5, the configured raw type
    # 1 + (byte >> 3) % 6 (#169). 34 is a STRING tag on a string raw: the
    # samples read the text after them.
    'string-good': header(tag=34)
        + sample(kind='string', width=0) + sample(kind='string', width=0)
        + 'unit 7'.encode(),
    # 0 is a BOOL tag on a bool raw.
    'bool-good': header(tag=0)
        + sample(kind='bool', width=1, number=1)
        + sample(kind='bool', width=1, number=0),
    # A double point (features bit 5): raw 2 is CLOSED, raw 7 a bad state.
    'double-point': header(features=0b0100000)
        + sample(kind='unsigned', number=2) + sample(kind='unsigned', number=7),
    # raw_range 0 to 100 (features bit 6; gain byte 0, offset byte 100): 150
    # is outside, UNCERTAIN with OUT_OF_RANGE; 50 is inside.
    'raw-range': header(features=0b1000000, gain=0, offset=100)
        + sample(number=150) + sample(number=50),
    # 10 is a BOOL tag on a configured signed raw, which CF-504 refuses: BAD
    # with OUT_OF_RANGE, though the value itself reads well (#169).
    'bool-tag-on-a-signed-raw': header(tag=10)
        + sample(kind='bool', width=1, number=1),
    'health-bad-then-good': header(features=0b01010, status=0)
        + sample(number=42)                      # status 0: BAD
        + sample(number=43, flt_bits=1 << 56),   # status 1: good
}
for name, data in seeds.items():
    with open(os.path.join(out, name), 'wb') as f:
        f.write(data)
    print(name, len(data))
