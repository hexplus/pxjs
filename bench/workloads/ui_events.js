// UI event workload: a native input event -> a JS callback -> property
// reads/writes -> render-state updates. On the PSP bench the events come
// from C through callJS (native -> JS); elsewhere from a JS loop.
const EVENTS = 20000;
const state = { x: 0, y: 0, selected: 0, pressed: 0, items: [], dirty: false, frame: 0 };
for (let i = 0; i < 20; i++) state.items.push({ id: i, label: "Item " + i, focused: false, x: i * 24, y: 40 });
const buttons = ["up", "down", "left", "right", "cross", "circle"];
let seq = 0;

function onInput() {
    const b = buttons[seq++ % buttons.length];
    const s = state;
    if (b === "up") s.y -= 1;
    else if (b === "down") s.y += 1;
    else if (b === "left") s.selected = (s.selected + s.items.length - 1) % s.items.length;
    else if (b === "right") s.selected = (s.selected + 1) % s.items.length;
    else s.pressed++;
    const item = s.items[s.selected];
    item.focused = true;
    s.x = item.x;
    s.dirty = true;
}

function render() {
    let n = 0;
    for (const it of state.items) {
        if (it.focused) n += it.label.length;
        it.focused = false;
    }
    state.dirty = false;
    state.frame++;
    return n;
}

const t0 = now();
let sum = 0;
if (typeof callJS === "function") {
    for (let f = 0; f < EVENTS / 20; f++) {
        callJS(onInput, 20);
        sum += render();
    }
} else {
    for (let f = 0; f < EVENTS / 20; f++) {
        for (let i = 0; i < 20; i++) onInput();
        sum += render();
    }
}
const ms = now() - t0;
print("ui_events: " + ms.toFixed(1) + " ms, " + ((ms * 1000) / EVENTS).toFixed(2) + " us/event, frames " +
      state.frame + ", checksum " + (sum + state.pressed + state.y));
