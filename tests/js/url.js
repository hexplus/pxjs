// URL and URLSearchParams (src/js/url.js). Parsing and the setters are
// checked against the Web Platform Tests' data by tools/url_wpt.py; these
// cover the rest: URLSearchParams, the link between the two, and the lazy
// globals.
function eq(got, want, what) {
    if (got !== want) throw new Error(what + ": got " + JSON.stringify(got) + ", want " + JSON.stringify(want));
}
function throws(f, name, what) {
    try { f(); } catch (e) { eq(e.name, name, what); return; }
    throw new Error(what + ": no exception");
}

// the globals: accessors until first use, then ordinary classes
let d = Object.getOwnPropertyDescriptor(globalThis, "URL");
eq(typeof d.get, "function", "URL starts as an accessor");
eq(typeof URL, "function", "URL");
d = Object.getOwnPropertyDescriptor(globalThis, "URL");
eq(d.value, URL, "then a data property");
eq(d.enumerable, false, "not enumerable");
eq(typeof Object.getOwnPropertyDescriptor(globalThis, "URLSearchParams").value, "function", "both are loaded at once");

// URL basics
const u = new URL("../c?x=1#f", "http://EXAMPLE.com:80/a/b");
eq(u.href, "http://example.com/c?x=1#f", "relative");
eq(u.port, "", "default port dropped");
eq(URL.canParse("nope"), false, "canParse");
eq(URL.parse("nope"), null, "parse");
eq(URL.parse("/x", "http://h").href, "http://h/x", "parse with base");
throws(() => new URL("nope"), "TypeError", "invalid URL");
throws(() => new URL(), "TypeError", "no argument");
eq(Object.prototype.toString.call(u), "[object URL]", "toStringTag");
eq(JSON.stringify({ u }), '{"u":"http://example.com/c?x=1#f"}', "toJSON");
eq(new URL("http://münchen.de/").hostname, "xn--mnchen-3ya.de", "IDN");

// URLSearchParams
let p = new URLSearchParams("?a=1&b=2&a=3&c=%20x+y&d");
eq(p.get("a"), "1", "get");
eq(p.getAll("a").join(), "1,3", "getAll");
eq(p.get("c"), " x y", "decoding");
eq(p.get("d"), "", "no =");
eq(p.get("z"), null, "absent");
eq(p.has("a", "3"), true, "has with value");
eq(p.has("a", "4"), false, "has with another value");
eq(p.size, 5, "size");
p.set("a", "9");
eq(p.toString(), "a=9&b=2&c=+x+y&d=", "set keeps the first, drops the rest");
p.delete("b");
p.append("e", "é &=");
eq(p.toString(), "a=9&c=+x+y&d=&e=%C3%A9+%26%3D", "append, encoding");
p.delete("e", "other");
eq(p.has("e"), true, "delete with a value that does not match");
p = new URLSearchParams("z=1&a=2&z=0&a=1");
p.sort();
eq(p.toString(), "a=2&a=1&z=1&z=0", "sort is stable");
eq(new URLSearchParams([["x", 1], ["y", "2"]]).toString(), "x=1&y=2", "from pairs");
eq(new URLSearchParams({ k: "v", n: 5 }).toString(), "k=v&n=5", "from a record");
eq(new URLSearchParams(new Map([["m", "1"]])).toString(), "m=1", "from an iterable");
throws(() => new URLSearchParams([["x"]]), "TypeError", "a pair of one");
throws(() => new URLSearchParams([["x", 1, 2]]), "TypeError", "a pair of three");
eq(new URLSearchParams("a=\uD800").get("a"), "�", "lone surrogates");
const seen = [];
new URLSearchParams("a=1&b=2").forEach((v, k) => seen.push(k + v));
eq(seen.join(), "a1,b2", "forEach");
eq([...new URLSearchParams("a=1&b=2").keys()].join(), "a,b", "keys");
eq([...new URLSearchParams("a=1&b=2").values()].join(), "1,2", "values");
// iterators are live
p = new URLSearchParams("a=1&b=2");
const it = p.entries();
it.next();
p.append("c", "3");
eq([...it].map(e => e[0]).join(), "b,c", "live iteration");
eq(Object.prototype.toString.call(p), "[object URLSearchParams]", "toStringTag");
throws(() => URLSearchParams.prototype.get.call({}, "a"), "TypeError", "brand check");

// the URL and its searchParams stay in step
const v = new URL("http://h/p?a=1");
v.searchParams.append("b", "2 3");
eq(v.href, "http://h/p?a=1&b=2+3", "params -> URL");
v.search = "?x=y";
eq(v.searchParams.get("x"), "y", "URL -> params");
eq(v.searchParams.get("a"), null, "the old ones are gone");
v.searchParams.delete("x");
eq(v.href, "http://h/p", "an emptied query disappears");
v.href = "http://h/?q=1";
eq(v.searchParams.get("q"), "1", "href -> params");
eq(v.searchParams, v.searchParams, "the same object");

// assigning before use replaces the global
globalThis.URLSearchParams = 42;
eq(URLSearchParams, 42, "assignable");
print("url ok");
