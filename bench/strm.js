(() => { let n = 0; for (let i = 0; i < 20000; i++) { const s = "item-" + i; n += s.split("-")[1].length + s.indexOf("-") + s.toUpperCase().length; } return n; })();
