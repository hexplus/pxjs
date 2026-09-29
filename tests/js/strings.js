// String fast paths: Latin-1 case mapping, integer-to-string, split
// limits, short concatenation.
assertEq("Björk".toUpperCase(), "BJÖRK");
assertEq("MOTÖRHEAD".toLowerCase(), "motörhead");
assertEq("straße".toUpperCase(), "STRASSE");
assertEq("ÿ".toUpperCase(), "Ÿ");
assertEq("ÀÉÎÕÜ".toLowerCase(), "àéîõü");
assertEq("x÷×y".toUpperCase(), "X÷×Y");
assertEq("Ωmega".toLowerCase(), "ωmega");
assertEq("".toUpperCase(), "");

assertEq(String(0), "0");
assertEq(String(-0), "0");
assertEq(String(7), "7");
assertEq(String(-1073741824), "-1073741824");
assertEq(String(1073741823), "1073741823");
assertEq(String(2147483647), "2147483647");
assertEq(String(-2147483648), "-2147483648");
assertEq(String(1e21), "1e+21");
assertEq(String(123456789012), "123456789012");
assertEq(`${-42}|${3.5}|${1 / 3}`, "-42|3.5|0.3333333333333333");
assertEq("item " + 12, "item 12");
assertEq((255).toString(16), "ff");

assertEq("a-b-c".split("-").join(), "a,b,c");
assertEq("a-b-c".split("-", 2).join(), "a,b");
assertEq("a-b-c".split("-", 0).length, 0);
assertEq("a-b-c".split("-", -1).length, 3);   // ToUint32(-1) = 2^32 - 1
assertEq("abc".split("", 2).join(), "a,b");
assertEq("a1b2c".split(/\d/).join(), "a,b,c");

assertEq("ab".charAt(1), "b");
assertEq("ab".charAt(-1), "");
assertEq("ab".charAt(1.9), "b");
assertEq("ab".charCodeAt(0), 97);
assert(Number.isNaN("ab".charCodeAt(5)));
assertEq("ab".at(-1), "b");
assertEq("hello".indexOf("l"), 2);
assertEq("hello".lastIndexOf("l"), 3);
assertEq("hello".substring(4, 1), "ell");
assertEq("hello".substring(-5, 2), "he");
assertEq("hello".substr(1, 3), "ell");
assertEq("hello".substr(-3), "llo");
assertEq("hello".slice(-3, -1), "ll");
assertEq("hello".slice(2), "llo");
print("strings ok");
