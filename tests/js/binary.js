// ArrayBuffer, typed arrays, DataView, TextEncoder/TextDecoder.
const u8 = new Uint8Array([1, 2, 255, 256, -1]);
assertEq([...u8], [1, 2, 255, 0, 255]);
assertEq(u8.length, 5);
const i16 = new Int16Array(u8.buffer.byteLength >= 4 ? 2 : 0);
i16[0] = 40000;
assertEq(i16[0], 40000 - 65536);
const f32 = Float32Array.from([1.5, 2.25]);
assertEq(f32[1], 2.25);
const c = new Uint8ClampedArray([300, -5, 1.5]);
assertEq([...c], [255, 0, 2]);
const buf = new ArrayBuffer(8);
const view = new DataView(buf);
view.setUint32(0, 0xDEADBEEF);
view.setFloat32(4, 0.5, true);
assertEq(view.getUint32(0).toString(16), "deadbeef");
assertEq(new Uint8Array(buf)[0], 0xDE);
assertEq(view.getFloat32(4, true), 0.5);
const sub = new Uint8Array(buf, 2, 2);
assertEq([...sub], [0xBE, 0xEF]);
assertEq(new Uint8Array(buf).subarray(0, 2).byteOffset, 0);
const sl = new Uint8Array([1, 2, 3, 4]).slice(1, 3);
sl[0] = 9;
assertEq([...sl], [9, 3]);
const t = new Uint16Array(4);
t.set([1, 2], 1);
assertEq([...t], [0, 1, 2, 0]);
assertEq(new Int32Array([5, 1, 3]).sort().join(","), "1,3,5");
assertEq(Object.prototype.toString.call(u8), "[object Uint8Array]");
const enc = new TextEncoder().encode("héllo €");
assertEq(enc.length, 10);
assertEq(new TextDecoder().decode(enc), "héllo €");
assertEq(new TextDecoder().decode(new Uint8Array([0xE2, 0x82])), "��");
try { new Uint8Array(1e9); assert(false); } catch (e) { assertEq(e.name, "RangeError"); }
const cw = new Uint16Array([1, 2, 3, 4, 5, 6]);
assertEq(cw.copyWithin(2, 0, 3).join(), "1,2,1,2,3,6"); // overlapping, forwards
assertEq(new Int8Array([1, 2, 3, 4, 5]).copyWithin(0, -2).join(), "4,5,3,4,5");
assertEq(new Uint8Array([1, 2, 3]).copyWithin(1, 0, 99).join(), "1,1,2");
print("binary ok");
