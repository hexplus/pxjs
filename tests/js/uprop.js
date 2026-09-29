// Unicode property escapes (with the u flag).
assert(/\p{L}/u.test("é") && /\p{L}/u.test("Ж") && /\p{L}/u.test("漢") && !/\p{L}/u.test("1"));
assert(/^\p{Lu}\p{Ll}+$/u.test("Björk") && !/^\p{Lu}/u.test("björk"));
assertEq("a1b²c٣".match(/\p{N}/gu).join(""), "1²٣");
assertEq("a1b²c٣".match(/\p{Nd}/gu).join(""), "1٣");
assertEq("Hi, you! ¿Qué?".replace(/\p{P}/gu, ""), "Hi you Qué");
assertEq("x = 1 + 2 € 3".match(/\p{S}/gu).join(""), "=+€");
assert(/\p{Zs}/u.test("\u00A0") && /\p{White_Space}/u.test("\u2028"));
assertEq("héllo wörld".replace(/\P{L}/gu, "_"), "héllo_wörld");
assertEq("Ωmega Ärger ÿes".match(/[\p{Lu}\d]/gu).join(""), "ΩÄ");
assertEq("abc123".replace(/[^\p{L}]/gu, ""), "abc");
assert(/\p{Script=Greek}/u.test("λ") && !/\p{sc=Greek}/u.test("l"));
assert(/^\p{Script=Han}+$/u.test("漢字") && /\p{sc=Hiragana}/u.test("ひ") && /\p{sc=Katakana}/u.test("カ"));
assert(/\p{Script=Cyrillic}/u.test("Ж") && /\p{Script=Latin}/u.test("ñ") && !/\p{Script=Latin}/u.test("ж"));
assert(/\P{Script=Latin}/u.test("ж") && !/\P{Script=Latin}/u.test("a"));
assertEq("aπb".replace(/[\P{Script=Greek}]/gu, ""), "π");
assert(/\p{ASCII}/u.test("~") && !/\p{ASCII}/u.test("é") && /\p{Any}/u.test("\uFFFF"));
assert(/\p{General_Category=Letter}/u.test("x") && /\p{gc=Lu}/u.test("X"));
assert(/\p{Alphabetic}/u.test("ⅻ") && /\p{Uppercase}/u.test("É") && /\p{Lowercase}/u.test("é"));
// without u, \p is just "p"
assert(/\p{L}/.test("p{L}"));
let err = "";
try { new RegExp("\\p{Nope}", "u"); } catch (e) { err = e.name; }
assertEq(err, "SyntaxError");
print("uprop ok");
