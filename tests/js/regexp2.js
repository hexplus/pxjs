// Lookbehind.
assertEq(/(?<=\$)\d+/.exec("cost $42")[0], "42");
assertEq("a1b2$3".replace(/(?<!\$)\d/g, "#"), "a#b#$3");
assertEq("x,y,z".match(/(?<=^|,)\w/g).join("|"), "x|y|z");
assert(/(?<=ab|c)d/.test("abd") && /(?<=ab|c)d/.test("cd") && !/(?<=ab|c)d/.test("bd"));
assert(/(?<!\w)cat/.test("a cat") && !/(?<!\w)cat/.test("bobcat"));
assertEq("2024-01-02".replace(/(?<=-)0/g, ""), "2024-1-2");
assertEq("price: 10 EUR, 20 USD".match(/\d+(?= USD)/)[0], "20");
assertEq("aXbXc".split(/(?<=X)/).join("|"), "aX|bX|c");
// unbounded body (a loop): still correct, just slower
assert(/(?<=a+)b/.test("aaab") && !/(?<=a+)b/.test("cb"));
// named groups inside
assertEq(/(?<=(?<cur>[$€]))\d+/.exec("€15").groups.cur, "€");
// nested lookarounds
assert(/(?<=(?<!x)a)b/.test("ab") && !/(?<=(?<!x)a)b/.test("xab"));
// case-insensitive, multiline
assert(/(?<=^foo)bar/im.test("x\nFOObar"));
// long input: bounded work
{
    const s = "ab".repeat(5000) + "$7";
    assertEq(s.match(/(?<=\$)\d/)[0], "7");
    assertEq(s.replace(/(?<!a)b/g, "").length, s.length);
}
print("regexp2 ok");
