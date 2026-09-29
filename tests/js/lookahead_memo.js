// The compiler remembers where groups and function bodies end (so it lexes
// them once); these check that hoisting and `arguments` still see through
// nested blocks, functions and literals.
function f() {
    { { var a = 1; } }
    if (true) { for (;;) { var b = 2; break; } }
    const g = () => { { return arguments[0]; } };
    return [a, b, g()];
}
assertEq(f(5).join(), "1,2,5");

function h() {
    const inner = function () { { var z = 3; } return z; };
    { let q = 1; { var w = q + 1; } }
    return inner() + w;
}
assertEq(h(), 5);

function k() {
    const o = { a: { b: { c: () => { var d = 4; return d; } } } };
    { var e = o.a.b.c(); }
    return e;
}
assertEq(k(), 4);

function m() {
    const arrows = [(x) => ({ y: [x, (z) => z * 2] })];
    { var n = arrows[0](3).y[1](arguments.length); }
    return n;
}
assertEq(m(1, 2, 3), 6);

class C {
    method() { { var v = [1, [2, [3]]]; } return v.flat(2).length; }
    get g() { if (true) { { var u = "u"; } } return u; }
}
assertEq(new C().method(), 3);
assertEq(new C().g, "u");
print("lookahead_memo ok");
