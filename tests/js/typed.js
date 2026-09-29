// Typed arrays, ArrayBuffer, DataView and Date: regressions from the
// Test262 conformance work (the `typed` area).

// %TypedArray%: the constructors' prototype, abstract itself
const TA = Object.getPrototypeOf(Int8Array);
assertEq(TA.name, "TypedArray");
assertEq(Object.getPrototypeOf(Float64Array.prototype), TA.prototype);
assert(!Object.prototype.hasOwnProperty.call(Int8Array, "from"), "from is inherited");
let threw = false;
try { new TA(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "new TypedArray() throws");
threw = false;
try { Int8Array(4); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "calling a typed array constructor throws");
assertEq(TA[Symbol.species], TA);
assertEq(ArrayBuffer[Symbol.species], ArrayBuffer);
assertEq(TA.prototype.toString, Array.prototype.toString);
assertEq(TA.prototype[Symbol.iterator], TA.prototype.values);
assert(TA.prototype.forEach !== Array.prototype.forEach, "own methods, not Array's");
assertEq(ArrayBuffer.prototype[Symbol.toStringTag], "ArrayBuffer");
assertEq(DataView.prototype[Symbol.toStringTag], "DataView");

// the methods use the array's own length, not a length property
const u = new Uint8Array([3, 1, 2]);
Object.defineProperty(u, "length", { value: 0 });
assertEq(u.map((x) => x * 2).join(), "6,2,4");
assertEq([...u.toSorted()], [1, 2, 3]);
assertEq([...u], [3, 1, 2]);
assertEq([...u.with(-1, 9)], [3, 1, 9]);
assertEq([...u.toReversed()], [2, 1, 3]);
assertEq(u.at(-1), 2);
assertEq(u.includes(1, -2), true);
assertEq(u.lastIndexOf(3, -4), -1);

// species
class MyU8 extends Uint8Array {}
const m = new MyU8([1, 2, 3, 4]);
assert(m.slice(1) instanceof MyU8, "slice uses species");
assert(m.subarray(1) instanceof MyU8, "subarray uses species");
assert(m.filter((x) => x & 1) instanceof MyU8, "filter uses species");
assertEq(m.subarray(1, 3).byteOffset, 1);
class ShortSpecies extends Uint8Array {
    static get [Symbol.species]() { return function () { return new Uint8Array(1); }; }
}
threw = false;
try { new ShortSpecies(4).slice(); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "a species result that is too short throws");

// sort: numeric, -0 before +0, NaN last, stable with a comparator
const f = new Float64Array([NaN, 3, -0, 0, -Infinity, 1]);
f.sort();
assertEq(Object.is(f[1], -0) && Object.is(f[2], 0), true);
assertEq(f[0], -Infinity);
assert(Number.isNaN(f[5]), "NaN last");
assertEq([...new Int16Array([5, -2, 9, 0]).sort((a, b) => b - a)], [9, 5, 0, -2]);

// conversions
const c8 = new Uint8ClampedArray([0.5, 1.5, 2.5, 254.5, -0.4, 255.6, NaN]);
assertEq([...c8], [0, 2, 2, 254, 0, 255, 0]);
const i32 = new Int32Array([2 ** 32 + 5, -(2 ** 31) - 1, 1e20, -1.9]);
assertEq([...i32], [5, 2 ** 31 - 1, 1661992960, -1]);
const f32 = new Float32Array([16777217, 0.1]);
assertEq(f32[0], 16777216);
assertEq(f32[1], 0.10000000149011612);

// set: typed sources (overlapping, other kinds), array-likes, offsets
const b = new ArrayBuffer(8);
const bytes = new Uint8Array(b);
bytes.set([1, 2, 3, 4, 5, 6, 7, 8]);
new Uint16Array(b).set(new Uint8Array(b, 0, 4));
assertEq([...bytes], [1, 0, 2, 0, 3, 0, 4, 0]);
threw = false;
try { bytes.set([1], 8); } catch (e) { threw = e instanceof RangeError; }
assert(threw, "set past the end throws a RangeError");
threw = false;
try { bytes.set([1], -1); } catch (e) { threw = e instanceof RangeError; }
assert(threw, "a negative offset throws a RangeError");
bytes.set("12", 6);
assertEq(bytes[7], 2);

// the constructor: iterables are iterated before any conversion
const src = [1, 2, 3];
const conv = new Int8Array([{ valueOf() { src[2] = 9; return 5; } }, 6]);
assertEq([...conv], [5, 6]);
assertEq([...new Uint8Array(new Set([7, 8]))], [7, 8]);
assertEq([...Uint8Array.from({ length: 2, 0: 4, 1: 5 }, (x) => x * 2)], [8, 10]);
assertEq([...Int16Array.of(1, -1)], [1, -1]);
threw = false;
try { new Int32Array(new ArrayBuffer(6)); } catch (e) { threw = e instanceof RangeError; }
assert(threw, "a buffer length that is not a multiple of the element size throws");

// integer-indexed exotic objects: numeric keys never reach the prototype
TA.prototype["-0"] = "inherited";
TA.prototype["1.5"] = "inherited";
const k = new Uint8Array(2);
assertEq(k["-0"], undefined);
assertEq(k["1.5"], undefined);
assertEq("1.5" in k, false);
assertEq("-0" in k, false);
k["1.5"] = 3;
assertEq(Object.keys(k).join(), "0,1");
delete TA.prototype["-0"];
delete TA.prototype["1.5"];
assertEq(Reflect.defineProperty(k, "2", { value: 1 }), false);
assertEq(Reflect.defineProperty(k, "0", { value: 7 }), true);
assertEq(k[0], 7);
let calls = 0;
k[5] = { valueOf() { calls++; return 1; } };
assertEq(calls, 1);

// detached buffers (transfer detaches its source)
const ab = new ArrayBuffer(4);
const view = new Uint8Array(ab);
view[0] = 42;
const moved = ab.transfer();
assertEq(new Uint8Array(moved)[0], 42);
assertEq(ab.detached, true);
assertEq(ab.byteLength, 0);
assertEq(view.length, 0);
assertEq(view.byteOffset, 0);
assertEq(view[0], undefined);
view[0] = 1;
assertEq(Object.keys(view).length, 0);
threw = false;
try { view.fill(0); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "methods on a detached view throw");
threw = false;
try { new DataView(ab); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "a DataView over a detached buffer throws");
const grown = moved.transfer(6);
assertEq([...new Uint8Array(grown)], [42, 0, 0, 0, 0, 0]);

// DataView: both byte orders, exact floats
const dv = new DataView(new ArrayBuffer(16), 1);
dv.setFloat64(0, Math.PI);
assertEq(dv.getFloat64(0), Math.PI);
assertEq(dv.getUint8(0), 0x40);
dv.setFloat32(8, 1 / 3, true);
assertEq(dv.getFloat32(8, true), Math.fround(1 / 3));
dv.setInt16(2, -2, true);
assertEq(dv.getUint16(2, true), 65534);
assertEq(dv.getInt16(2), -257);
threw = false;
try { dv.getInt32(13); } catch (e) { threw = e instanceof RangeError; }
assert(threw, "reading past the end throws a RangeError");

// ArrayBuffer.prototype.slice with species
class SpeciesAB extends ArrayBuffer {}
assert(new SpeciesAB(4).slice(1) instanceof SpeciesAB, "slice uses species");
assertEq(new ArrayBuffer(8).slice(-3, -1).byteLength, 2);

// Date: formats Date.parse reads back, negative years, toJSON, toPrimitive
const d0 = new Date(0);
assertEq(Date.parse(d0.toString()), 0);
assertEq(Date.parse(d0.toUTCString()), 0);
assertEq(Date.parse(d0.toISOString()), 0);
const bc = new Date(Date.UTC(-1, 0, 1));
assertEq(bc.toUTCString(), "Fri, 01 Jan -0001 00:00:00 GMT");
assertEq(Date.parse(bc.toUTCString()), bc.getTime());
assertEq(bc.toISOString(), "-000001-01-01T00:00:00.000Z");
assert(Number.isNaN(Date.parse("-000000-01-01T00:00:00Z")), "-000000 is not a year");
assert(Number.isNaN(Date.parse("2021-02-30")), "no February 30th");
assertEq(Date.parse("2000-01-01T24:00:00Z"), Date.UTC(2000, 0, 2));
assertEq(Date.UTC(-0.5), Date.UTC(1900, 0));
assertEq(Object.is(new Date(0).getTimezoneOffset() * 0, 0), true);
assertEq(Date.prototype.toJSON.call({ valueOf() { return NaN; }, toISOString() { throw 1; } }), null);
assertEq(Date.prototype.toJSON.call({ valueOf() { return 1; }, toISOString() { return "x"; } }), "x");
assertEq(d0[Symbol.toPrimitive]("number"), 0);
assertEq(typeof d0[Symbol.toPrimitive]("default"), "string");
threw = false;
try { d0[Symbol.toPrimitive]("bogus"); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "an invalid hint throws");
const invalid = new Date(NaN);
invalid.setFullYear(2000);
assertEq(invalid.getFullYear(), 2000);
assertEq(invalid.getHours(), 0);
class MyDate extends Date {}
assert(new MyDate(5) instanceof MyDate, "Date subclasses");
assertEq(new MyDate(5).getTime(), 5);
print("typed ok");
