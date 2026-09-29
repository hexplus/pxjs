// Built-in metadata, checked generically: every function reachable from
// the global namespaces has { writable: false, enumerable: false,
// configurable: true } `length` and `name`; methods are writable,
// non-enumerable and configurable; prototype links are sound.
(function () {
const problems = [];
function bad(path, what) { problems.push(path + ": " + what); }

function checkFunction(f, path, expectName) {
    const n = Object.getOwnPropertyDescriptor(f, "name"), l = Object.getOwnPropertyDescriptor(f, "length");
    if (!n) bad(path, "no own name");
    else {
        if (n.writable || n.enumerable || !n.configurable) bad(path, "name attributes");
        if (typeof n.value !== "string") bad(path, "name is not a string");
        else if (expectName !== undefined && n.value !== expectName) bad(path, "name is " + JSON.stringify(n.value) + ", not " + JSON.stringify(expectName));
    }
    if (!l) bad(path, "no own length");
    else if (l.writable || l.enumerable || !l.configurable || typeof l.value !== "number") bad(path, "length attributes");
}

function keyName(k) { return typeof k === "symbol" ? "[" + k.description + "]" : k; }

// one function object under two names keeps its first name
const aliases = {
    "globalThis.Array.prototype.[Symbol.iterator]": "values",
    "globalThis.String.prototype.trimLeft": "trimStart",
    "globalThis.String.prototype.trimRight": "trimEnd",
    "globalThis.Map.prototype.[Symbol.iterator]": "entries",
    "globalThis.Set.prototype.[Symbol.iterator]": "values",
    "globalThis.Set.prototype.keys": "values",
    "globalThis.Date.prototype.toGMTString": "toUTCString",
};
const seen = new Set();
function walk(obj, path, isCtor) {
    if (seen.has(obj)) return;
    seen.add(obj);
    for (const k of Reflect.ownKeys(obj)) {
        const d = Object.getOwnPropertyDescriptor(obj, k), p = path + "." + keyName(k);
        if (k === "constructor" || k === "caller" || k === "arguments" || k === "length" || k === "name") continue;
        if ("value" in d && typeof d.value === "function") {
            if (d.enumerable) bad(p, "enumerable method");
            if (k !== "prototype" && typeof k === "string" && /^[A-Z]/.test(k) && path === "globalThis") {
                // a constructor: its prototype is fixed, and links back
                const pd = Object.getOwnPropertyDescriptor(d.value, "prototype");
                if (pd && (pd.writable || pd.enumerable || pd.configurable)) bad(p + ".prototype", "attributes");
                if (pd && pd.value && Object.prototype.hasOwnProperty.call(pd.value, "constructor") && pd.value.constructor !== d.value)
                    bad(p + ".prototype.constructor", "does not link back");
                checkFunction(d.value, p, k);
                walk(d.value, p, true);
                if (pd && pd.value && typeof pd.value === "object") walk(pd.value, p + ".prototype", false);
            } else if (k !== "prototype") {
                if (!d.writable || !d.configurable) {
                    /* @@hasInstance and @@toPrimitive are not writable */
                    if (k !== Symbol.hasInstance && k !== Symbol.toPrimitive) bad(p, "method attributes");
                }
                checkFunction(d.value, p, p in aliases ? aliases[p] : keyName(k));
            }
        } else if ("get" in d) {
            if (d.enumerable) bad(p, "enumerable accessor");
            if (d.get) checkFunction(d.get, p + " getter", "get " + keyName(k));
            if (d.set) checkFunction(d.set, p + " setter", "set " + keyName(k));
        } else if (k === Symbol.toStringTag) {
            if (d.writable || d.enumerable || !d.configurable) bad(p, "@@toStringTag attributes");
        } else if (typeof d.value === "number" && isCtor || path === "globalThis.Math") {
            if (typeof d.value === "number" && (d.writable || d.enumerable || d.configurable)) bad(p, "constant attributes");
        }
    }
}

for (const name of ["Object", "Function", "Array", "String", "Number", "Boolean", "Symbol", "Error", "TypeError",
                    "RangeError", "ReferenceError", "SyntaxError", "EvalError", "URIError", "AggregateError", "Proxy",
                    "Map", "Set", "WeakMap", "WeakSet", "Promise", "Date", "RegExp", "ArrayBuffer", "DataView"]) {
    const d = Object.getOwnPropertyDescriptor(globalThis, name);
    assert(d && !d.enumerable && d.writable && d.configurable, name + " global attributes");
}
walk(globalThis, "globalThis", false);
walk(Math, "globalThis.Math", false);
walk(JSON, "globalThis.JSON", false);
walk(Reflect, "globalThis.Reflect", false);

function check(c, what) { if (!c) problems.push(what); }
// aliases are one function object
check(Array.prototype[Symbol.iterator] === Array.prototype.values, "Array.prototype[@@iterator]");
check(Number.parseFloat === parseFloat && Number.parseInt === parseInt, "Number.parseFloat/parseInt");
check(String.prototype.trimLeft === String.prototype.trimStart, "trimLeft");
check(String.prototype.trimRight === String.prototype.trimEnd, "trimRight");
check(Object.getOwnPropertyDescriptor(Array, Symbol.species).get.name === "get [Symbol.species]", "species getter name");
// built-in methods are not constructors; built-in constructors are
for (const f of [Math.abs, parseInt, [].map, Object.keys, JSON.parse, Reflect.get])
    check((() => { try { Reflect.construct(function () {}, [], f); return false; } catch (e) { return e instanceof TypeError; } })(), "not a constructor");

if (problems.length) throw new Error(problems.length + " metadata problems:\n" + problems.join("\n"));
print("builtins_meta ok");
})();
