// Number representation edges: the SMI/heap-number boundary, ToInt32 done
// on the IEEE-754 bits, -0, and arithmetic across the boundary.
const is = (a, b) => assert(Object.is(a, b), `${a} is not ${b}`);

// SMI range is [-2^30, 2^30 - 1]; both sides of each edge.
is(1073741823 + 0, 1073741823);
is(1073741823 + 1, 1073741824);
is(-1073741824 - 0, -1073741824);
is(-1073741824 - 1, -1073741825);
is(2 ** 30, 1073741824);
is(-(2 ** 30), -1073741824);
is(0.5 + 0.5, 1);
is(1.5 - 0.5, 1);
is(-0.5 + 0.5, 0);
is(-0 + -0, -0);
is(0 * -1, -0);
is(-0 - 0, -0);
is(1 / (0 * -1), -Infinity);
is(3 * 0.5, 1.5);
is(2 ** 29 + 2 ** 29, 2 ** 30);

// ToInt32 / ToUint32
is(2 ** 31 | 0, -2147483648);
is((2 ** 31 - 1) | 0, 2147483647);
is(-(2 ** 31) | 0, -2147483648);
is(-(2 ** 31 + 1) | 0, 2147483647);
is((2 ** 32 + 5) | 0, 5);
is((2 ** 32 - 0.5) | 0, -1);
is(4294967295.9 | 0, -1);
is(-4294967297 | 0, -1);
is(-1.5 | 0, -1);
is(1.9999 | 0, 1);
is(0.9 | 0, 0);
is(-0.9 | 0, 0);
is(-0 | 0, 0);
is(1e20 | 0, 1661992960);
is(-1e20 | 0, -1661992960);
is((2 ** 53 + 2) | 0, 2);
is(2 ** 63 | 0, 0);
is(1.7976931348623157e308 | 0, 0);
is(5e-324 | 0, 0);
is(NaN | 0, 0);
is(Infinity | 0, 0);
is(-Infinity | 0, 0);
is(-1 >>> 0, 4294967295);
is(-1.5 >>> 0, 4294967295);
is((2 ** 32 + 3) >>> 0, 3);
is(1e20 >> 3, 207749120);
is(~(2 ** 31), 2147483647);
is(~1e20, -1661992961);
is(Math.imul(0xFFFFFFFF, 5), -5);
is(new Int32Array([2 ** 31 + 7])[0], -2147483641);
is(new Uint8Array([257.9, -1])[0], 1);
is(new Uint8Array([257.9, -1])[1], 255);

// The int32 accumulate pattern (s + i) | 0 across the boundary.
{
    let s = 0;
    for (let i = 0; i < 100000; i++) s = (s + i * 7919) | 0;
    is(s, -699451824);
}
// Doubles in a loop.
{
    let s = 0.5;
    for (let i = 0; i < 1000; i++) s += 0.25;
    is(s, 250.5);
}

// Comparisons of mixed representations.
assert(1073741824 > 1073741823);
assert(-1073741825 < -1073741824);
assert(0.5 < 1);
assert(!(NaN < 1) && !(NaN >= 1));
assert(2 ** 40 >= 2 ** 40);
assert("10" > 9);
assert("a" < "b");

// Number -> string at the edges.
assertEq(String(1073741824), "1073741824");
assertEq(String(-1073741825), "-1073741825");
assertEq(String(2 ** 53), "9007199254740992");
assertEq(String(-0.5), "-0.5");
print("numbers ok");
