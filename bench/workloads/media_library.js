// Media metadata workload: a large JSON library -> parse -> sort ->
// filter -> string operations (search, formatting).
const TRACKS = 1500;
const genres = ["Rock", "Jazz", "Electronic", "Classical", "Hip-Hop", "Folk", "Metal", "Ambient"];
const words = ["night", "blue", "river", "echo", "neon", "silver", "storm", "garden", "ocean", "dream"];

function makeLibrary() {
    const tracks = [];
    for (let i = 0; i < TRACKS; i++) {
        tracks.push({
            id: i,
            title: words[i % 10] + " " + words[(i * 7) % 10] + " " + (i % 13),
            artist: "Artist " + (i % 97),
            album: "Album " + (i % 211),
            genre: genres[i % genres.length],
            year: 1970 + (i % 53),
            duration: 120 + ((i * 37) % 300),
            rating: (i * 13) % 6,
            path: "ms0:/MUSIC/Artist " + (i % 97) + "/Album " + (i % 211) + "/" + (i % 20) + ".mp3",
        });
    }
    return JSON.stringify({ version: 3, tracks });
}

const text = makeLibrary();
const t0 = now();
const lib = JSON.parse(text).tracks;
const t1 = now();
const byTitle = lib.slice().sort((a, b) => a.title.localeCompare(b.title));
const byYear = lib.slice().sort((a, b) => a.year - b.year || a.duration - b.duration);
const t2 = now();
let hits = 0, chars = 0;
for (const q of ["neon", "RIVER", "echo 1", "storm garden"]) {
    const ql = q.toLowerCase();
    const found = lib.filter(t => t.title.toLowerCase().includes(ql) || t.artist.toLowerCase().includes(ql));
    hits += found.length;
    for (const t of found.slice(0, 30)) {
        const m = Math.floor(t.duration / 60), s = t.duration % 60;
        const line = `${t.artist} - ${t.title} (${m}:${String(s).padStart(2, "0")}) [${t.genre}, ${t.year}]`;
        chars += line.length + t.path.split("/").length;
    }
}
const perGenre = {};
for (const t of lib) perGenre[t.genre] = (perGenre[t.genre] || 0) + t.duration;
const t3 = now();
print("media_library: " + (t3 - t0).toFixed(1) + " ms (parse " + (t1 - t0).toFixed(1) + ", sort " +
      (t2 - t1).toFixed(1) + ", search/format " + (t3 - t2).toFixed(1) + "), " + text.length + " bytes, checksum " +
      (hits + chars + byTitle[0].id + byYear[TRACKS - 1].id + Object.keys(perGenre).length));
