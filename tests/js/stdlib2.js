// Map, Set, WeakMap, Date, RegExp, Reflect.
const m = new Map([["a", 1], [2, "two"]]);
m.set(NaN, "nan").set(-0, "zero");
assertEq(m.get("a"), 1);
assertEq(m.get(NaN), "nan");
assertEq(m.get(0), "zero");
assertEq(m.size, 4);
m.delete("a");
assertEq([...m.keys()].length, 3);
assertEq([...m].length, 3);
let fe = 0;
m.forEach((v, k) => fe++);
assertEq(fe, 3);
const s = new Set([1, 2, 2, 3, "3"]);
assertEq(s.size, 4);
assertEq([...s], [1, 2, 3, "3"]);
assert(s.has("3") && !s.has(4));
const o1 = {};
const wm = new WeakMap([[o1, "meta"]]);
assertEq(wm.get(o1), "meta");
try { wm.set(1, 2); assert(false); } catch (e) { assertEq(e.name, "TypeError"); }
const byLen = Map.groupBy(["a", "bb", "cc"], x => x.length);
assertEq(byLen.get(2), ["bb", "cc"]);
assertEq(Object.groupBy([1, 2, 3], x => (x % 2 ? "odd" : "even")).odd, [1, 3]);

const d = new Date(Date.UTC(2024, 1, 29, 12, 30, 15, 250));
assertEq(d.toISOString(), "2024-02-29T12:30:15.250Z");
assertEq(d.getUTCDay(), 4);
assertEq(new Date("2024-02-29T12:30:15.250Z").getTime(), d.getTime());
assertEq(Date.parse("2000-01-01"), 946684800000);
assertEq(JSON.stringify({ d }), '{"d":"2024-02-29T12:30:15.250Z"}');
const d2 = new Date(0);
d2.setUTCFullYear(1999);
assertEq(d2.getUTCFullYear(), 1999);
assert(typeof Date.now() === "number" && Date.now() > 1.6e12);
assert(isNaN(new Date("garbage").getTime()));
assertEq(String(new Date(NaN)), "Invalid Date");
assert(new Date(2020, 0, 1) < new Date(2021, 0, 1));
assertEq(new Date(2020, 11, 31, 23, 59).getFullYear(), 2020);

const re = /(\d{4})-(\d{2})-(?<day>\d{2})/;
const mt = "on 2024-03-15!".match(re);
assertEq(mt[0], "2024-03-15");
assertEq(mt[1], "2024");
assertEq(mt.groups.day, "15");
assertEq(mt.index, 3);
assert(/^hello$/i.test("HeLLo"));
assert(!/^hello$/.test("hello world"));
assertEq("a1b22c333".replace(/\d+/g, "#"), "a#b#c#");
assertEq("John Smith".replace(/(\w+)\s(\w+)/, "$2, $1"), "Smith, John");
assertEq("x-y_z".split(/[-_]/), ["x", "y", "z"]);
assertEq("aaa".replace(/a*?/g, "-"), "-a-a-a-");
assertEq("Björk Motörhead".match(/\w+/g), ["Bj", "rk", "Mot", "rhead"]);
assertEq("ÉCOLE".replace(/é/i, "e"), "eCOLE");
assertEq([..."a1b2".matchAll(/[a-z](\d)/g)].map(m => m[1]), ["1", "2"]);
assertEq("abc".search(/c/), 2);
assertEq(/(a)|(b)/.exec("b")[1], undefined);
assertEq(/(?=a)a/.test("a"), true);
assertEq(/q(?!u)/.test("quit"), false);
assertEq(/(\w)\1/.test("hello"), true);
assertEq(/a.c/s.test("a\nc"), true);
assertEq(/^b/m.test("a\nb"), true);
assertEq(String(/x+/gi), "/x+/gi");
const gre = /o/g;
gre.exec("foo");
assertEq(gre.lastIndex, 2);
assertEq("camelCaseString".replace(/([A-Z])/g, m => "_" + m.toLowerCase()), "camel_case_string");
try { new RegExp("(a"); assert(false); } catch (e) { assertEq(e.name, "SyntaxError"); }
let catastrophic = "caught";
try { /(a+)+$/.test("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!"); catastrophic = "finished"; } catch (e) { catastrophic = e.name; }
assert(catastrophic === "RangeError" || catastrophic === "finished");

assertEq(Reflect.ownKeys({ a: 1, [Symbol.iterator]: 2 }).length, 2);
assertEq(Reflect.has({ x: 1 }, "x"), true);
assertEq(Reflect.get({ get y() { return this.z; } }, "y", { z: 5 }), 5);
class R { constructor(a) { this.a = a; } }
assertEq(Reflect.construct(R, [4]).a, 4);
assertEq([3, 1, 2].toSorted(), [1, 2, 3]);
assertEq([1, 2, 3].with(1, 9), [1, 9, 3]);

print("stdlib2 ok");
