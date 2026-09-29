// REST-like workload: a JSON response -> JSON.parse -> traversal ->
// filter/map -> a UI model -> JSON.stringify of the changes.
// Runs on the pxjs host runner and on the PSP bench (print, now).
const ITEMS = 200, ROUNDS = 20;

function makeResponse(n) {
    const items = [];
    for (let i = 0; i < n; i++) {
        items.push({
            id: 1000 + i,
            name: "Product " + i,
            price: Math.round((i * 7.31) % 500 * 100) / 100,
            stock: (i * 13) % 41,
            rating: ((i * 37) % 50) / 10,
            tags: ["tag" + (i % 7), "cat" + (i % 3)],
            vendor: { id: i % 17, name: "Vendor " + (i % 17), verified: i % 2 === 0 },
            description: "Item number " + i + " with a \"quoted\" word and a tab\t.",
        });
    }
    return JSON.stringify({ page: 1, total: n, items });
}

const text = makeResponse(ITEMS);
let checksum = 0;
const t0 = now();
let tParse = 0, tWork = 0, tOut = 0;
for (let r = 0; r < ROUNDS; r++) {
    let a = now();
    const data = JSON.parse(text);
    let b = now();
    tParse += b - a;
    const visible = data.items
        .filter(it => it.stock > 0 && it.rating >= 1)
        .map(it => ({
            key: it.id,
            title: it.name.toUpperCase(),
            label: it.vendor.name + " - " + it.price.toFixed(2),
            badge: it.vendor.verified ? "verified" : "",
            tags: it.tags.join(", "),
        }));
    visible.sort((x, y) => x.label < y.label ? -1 : x.label > y.label ? 1 : 0);
    for (const v of visible) checksum += v.title.length + v.tags.length;
    a = now();
    tWork += a - b;
    const out = JSON.stringify({ updated: visible.slice(0, 50), count: visible.length });
    checksum += out.length;
    tOut += now() - a;
}
const total = now() - t0;
print("rest_api: " + total.toFixed(1) + " ms (parse " + tParse.toFixed(1) + ", work " + tWork.toFixed(1) +
      ", stringify " + tOut.toFixed(1) + "), " + text.length + " bytes x " + ROUNDS + ", checksum " + checksum);
