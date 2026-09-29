// RegExp and the String methods built on it (the regexp contributor's regressions).

// exec results, captures reset per iteration, lookbehind right to left
assertEq(String(/a(b)c/.exec("xabcx")), "abc,b");
assertEq(/a(b)c/.exec("xabcx").index, 1);
assertEq(JSON.stringify(/(z)((a+)?(b+)?(c))*/.exec("zaacbbbcac")), '["zaacbbbcac","z","ac","a",null,"c"]');
assertEq(JSON.stringify(/(a)|b/.exec("b")), '["b",null]');
assertEq(String(/(?<=(\d+)(\d+))$/.exec("1053").slice(1)), "1,053");
assertEq(/(?<!a)b/.exec("abcb").index, 3);
assertEq(/(?<=\$)\d+/.exec("cost $42")[0], "42");
assertEq(JSON.stringify(/(?<=(a+))b/.exec("aaab")), '["b","aaa"]');
assertEq(/(?<=\1(a))b/.exec("aab")[0], "b");
assertEq(JSON.stringify(/(a*)?/.exec("b")), '["",null]');
assertEq(/(\2)(a)/.exec("a")[0], "a");

// quantifiers: counted loops, huge bounds, lazy, empty iterations
assertEq("aaa".replace(/a*?/g, "-"), "-a-a-a-");
assertEq(/^(?:a|ab){2,3}c$/.test("aababc"), true);
assertEq(new RegExp("b{" + Number.MAX_SAFE_INTEGER + "}", "u").test(""), false);
assertEq(/(?:){5}x/.test("x"), true);
assertEq(/x{2,}?/.exec("xxxx")[0], "xx");
assertEq(/(?:a|()){3}b/.exec("aab")[0], "aab");

// named groups, match indices
const m = /(?<y>\d{4})-(?<m>\d\d)/.exec("on 2024-05");
assertEq(m.groups.y + "/" + m.groups.m, "2024/05");
assertEq(Object.getPrototypeOf(m.groups), null);
assertEq(/\k<a>(?<a>x)/.test("x"), true);
const d = /a(?<z>b)?/d.exec("xab");
assertEq(String(d.indices[0]) + ";" + String(d.indices.groups.z), "1,3;2,3");
assertEq("2024-05".replace(/(?<y>\d+)-(?<m>\d+)/, "$<m>/$<y>"), "05/2024");

// Unicode mode, properties, case folding
assertEq(/^.$/u.test("\u{1F600}") && !/^.$/.test("\u{1F600}"), true);
assertEq(/\u{1F600}/u.test("\u{1F600}"), true);
assertEq(/[\u{1F600}-\u{1F64F}]/u.test("\u{1F610}"), true);
assertEq(/\p{Lu}+/u.exec("abcDEFghi")[0], "DEF");
assertEq(/\p{Script=Greek}/u.test("\u{10140}") && /\p{L}/u.test("\u{10400}"), true);
assertEq(/\p{scx=Hira}/u.test("ー") && !/\p{sc=Hira}/u.test("ー"), true);
assertEq(/\p{ID_Start}/u.test("\u{1D4D1}") && !/\p{ID_Start}/u.test("1"), true);
assertEq(/\p{Emoji_Presentation}/u.test("\u{1F600}"), true);
assertEq(/ſ/iu.test("S") && /\w/iu.test("ſ") && !/\W/iu.test("ſ"), true);
assertEq(/ſ/i.test("S"), false); // without u: no mapping from outside ASCII into it
assertEq(/[a-z]/i.test("K") && /K/iu.test("k") && /\u{10400}/iu.test("\u{10428}"), true);
assertEq(/ß/iu.test("ẞ") && !/ß/i.test("ẞ"), true);
assertEq(/[^a]/i.test("A"), false);
assertEq(/(a)\1/i.test("aA"), true);
assertEq(/\b/iu.test("ſ") && !/\b/i.test("ſ"), true);
const lastPair = /\udf06/u;
assertEq(lastPair.exec("𝌆"), null);

// syntax: Annex B without u, strict with it
assertEq(/[\d-x]/.test("-") && /\8/.test("8") && /a{/.test("a{") && /]/.test("]"), true);
assertEq(/\1(a)/.test("a") && /\11/.test("\t"), true);
assertEq(/[\c_]/.test("\x1f") && /\c/.test("\\c"), true);
assertEq(/(?=a)*/.test(""), true);
for (const bad of ["(?<a>.)(?<a>.)", "\\k<b>(?<a>.)", "a**", "(?<=a)+", "[b-a]", "(", "x{2,1}"])
    assertEq((() => { try { new RegExp(bad); return "ok"; } catch (e) { return e.name; } })(), "SyntaxError", bad);
for (const bad of ["\\-", "{", "]", "\\8", "[\\d-x]", "\\u{110000}", "(?=a)*", "\\c", "\\p{Nope}", "\\P{ASCII=F}"])
    assertEq((() => { try { new RegExp(bad, "u"); return "ok"; } catch (e) { return e.name; } })(), "SyntaxError", bad);
assertEq((() => { try { new RegExp("a", "gg"); } catch (e) { return e.name; } })(), "SyntaxError");

// lastIndex and sticky
const g = /a/g;
g.lastIndex = 1;
assertEq(g.exec("aba").index, 2);
assertEq(g.lastIndex, 3);
assertEq(g.exec("aba"), null);
assertEq(g.lastIndex, 0);
const y = /b/y;
assertEq(y.test("ab"), false);
y.lastIndex = 1;
assertEq(y.test("ab"), true);

// the String methods and the symbol protocol
assertEq("abcabc".replace(/b/g, "[$&$`$'$$]"), "a[bacabc$]ca[babcac$]c");
assertEq("abc".replace(/(b)/, "$01$2$10"), "ab$2b0c");
assertEq("x".replaceAll("x", "$$"), "$");
assertEq("aXbXc".split("X", 2).join(), "a,b");
assertEq("a,b,,c".split(/,/).join("|"), "a|b||c");
assertEq("a1b2c".split(/(\d)/).join("|"), "a|1|b|2|c");
assertEq("\u{1F600}\u{1F600}".split(/(?:)/u).length, 2);
assertEq("abc".search(/c/), 2);
assertEq([..."a1b22c333".matchAll(/\d+/g)].map(r => r[0] + "@" + r.index).join(), "1@1,22@3,333@6");
assertEq("abc".replace(/b/, (s, i, str) => s.toUpperCase() + i + str.length), "aB13c");
const custom = { [Symbol.replace](s, r) { return s + "|" + r; } };
assertEq("abc".replace(custom, "z"), "abc|z");
assertEq("atruebtruec".split(true).join(), "a,b,c");
class R extends RegExp {
    exec(s) { const r = super.exec(s); if (r) r[0] = r[0].toUpperCase(); return r; }
}
assertEq("abcb".replace(new R("b", "g"), "<$&>"), "a<B>c<B>");
assertEq("a-b-c".split(new R("-")).join(), "a,b,c");
assertEq(new R("x") instanceof R && RegExp[Symbol.species] === RegExp, true);
assertEq(RegExp.prototype.flags + "|" + /a/dgimsuy.flags, "|dgimsuy");
assertEq(String(new RegExp("/")) + " " + new RegExp("\n").source + " " + RegExp.prototype.source, "/\\// \\n (?:)");
assertEq(RegExp.prototype.global, undefined);
const re = /a/g;
assertEq(RegExp(re) === re && new RegExp(re) !== re, true);

// hostile patterns fail cleanly
let err = "";
try { /(a+)+b/.test("a".repeat(40)); } catch (e) { err = e.name; }
assertEq(err, "RangeError");
err = "";
try { new RegExp("(".repeat(300) + ")".repeat(300)); } catch (e) { err = e.name; }
assertEq(err, "SyntaxError");
assertEq(new RegExp("(".repeat(200) + "x" + ")".repeat(200)).exec("x").length, 201);
assertEq(/^(?:a|b)*$/.test("ab".repeat(20000)), true);
assertEq(/x*y/.test("x".repeat(100000)), false);
print("regexp ok");
