// Number <-> string conversions (src/px_dtoa.c). Exhaustive checks against
// exact references are in tools/dtoa_check.py; these pin the cases that were
// wrong before, and the spec's rounding rules.
function eq(got, want, what) {
    if (got !== want) throw new Error(what + ": got " + got + ", want " + want);
}

// shortest round-trip digits
eq(String(0.1 + 0.2), "0.30000000000000004", "0.1 + 0.2");
eq(String(5e-324), "5e-324", "min denormal");
eq(String(1.7976931348623157e308), "1.7976931348623157e+308", "max");
eq(String(1e21), "1e+21", "1e21");
eq(String(123456789012345680000), "123456789012345680000", "below 1e21");
eq(String(1e-7), "1e-7", "1e-7");
eq(String(0.000001), "0.000001", "1e-6");
eq(String(-0), "0", "-0");
eq(String(2 ** 53 + 2), "9007199254740994", "2^53 + 2");
eq(String(1 / 3), "0.3333333333333333", "1/3");

// toFixed / toExponential / toPrecision: exact ties round up (not to even)
eq((2.5).toFixed(0), "3", "2.5 toFixed");
eq((0.5).toFixed(0), "1", "0.5 toFixed");
eq((1.25).toFixed(1), "1.3", "1.25 toFixed");
eq((1.005).toFixed(2), "1.00", "1.005 is below the tie");
eq((-1.5).toFixed(0), "-2", "negative tie");
eq((-0.0001).toFixed(2), "-0.00", "negative rounding to zero");
eq((-0).toFixed(2), "0.00", "-0 toFixed");
eq((9.96).toFixed(1), "10.0", "carry");
eq((0.005).toFixed(2), "0.01", "0.005 is above the tie");
eq((1e20).toFixed(2), "100000000000000000000.00", "large");
eq((1e21).toFixed(2), "1e+21", "1e21 toFixed");
eq((1.23).toFixed(-0.5), "1", "ToIntegerOrInfinity(-0.5) is 0");
eq((25).toPrecision(1), "3e+1", "25 toPrecision(1)");
eq((1.25).toPrecision(2), "1.3", "1.25 toPrecision(2)");
eq((0.000001234).toPrecision(2), "0.0000012", "small toPrecision");
eq((123.456).toPrecision(4), "123.5", "toPrecision");
eq((0).toPrecision(3), "0.00", "0 toPrecision");
eq((0.000001).toExponential(), "1e-6", "toExponential()");
eq((123456).toExponential(2), "1.23e+5", "toExponential(2)");
eq((0).toExponential(), "0e+0", "0 toExponential()");
eq((1.5).toExponential(0), "2e+0", "tie toExponential");
eq(NaN.toExponential(1000), "NaN", "NaN is checked before the range");
let threw = false;
try { (1).toFixed(101); } catch (e) { threw = e instanceof RangeError; }
eq(threw, true, "toFixed(101)");

// string -> number: any length, correctly rounded
eq(Number("0".repeat(500) + "1"), 1, "leading zeros");
eq(Number("1" + "0".repeat(400)), Infinity, "overflow");
eq(Number("0." + "0".repeat(500) + "1"), 0, "underflow");
eq(parseFloat("0".repeat(500) + "1.5"), 1.5, "parseFloat leading zeros");
eq(Number("9007199254740993"), 9007199254740992, "2^53 + 1 rounds to even");
eq(Number("9007199254740993.0000000000000000000001"), 9007199254740994, "just above the tie");
eq(Number("0x20000000000001"), 9007199254740992, "hex tie rounds to even");
eq(Number("0x20000000000003"), 9007199254740996, "hex tie rounds to even (up)");
eq(0x20000000000001, 9007199254740992, "hex literal");
eq(0x2000_0000_0000_03, 9007199254740996, "hex literal with separators");
eq(1_000.5e1_0, 10005000000000, "decimal literal with separators");
eq(isNaN(Number("1_000")), true, "no separators in strings");
eq(isNaN(Number("-0x10")), true, "no sign on hex");
eq(Number("  +Infinity\n"), Infinity, "Infinity");
eq(isNaN(Number("infinity")), true, "infinity is not Infinity");
eq(Number(".5e1"), 5, ".5e1");
eq(1 / Number("-0"), -Infinity, "-0");
eq(JSON.parse("[" + "0".repeat(0) + "1e-400, 123456789012345678901234567890]")[1], 1.2345678901234568e29, "JSON numbers");
eq(JSON.parse("0.30000000000000004"), 0.1 + 0.2, "JSON round trip");
