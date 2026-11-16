/* prism's app page: its backend's state now and then, drawn. The backend
   is prism serve (../cli/serve.c, its calls under api/) when the page
   comes from it, else prism in the browser itself (web.js). */

const T = {
    tagline: "Your Minecraft worlds as 3D maps to walk around in the browser.",
    maps: "Your maps", worlds: "Bake a world", noMaps: "No maps yet. Pick a world below to start.",
    noWorlds: "No saves found in:", mcAt: "Minecraft folder:", regions: "regions", version: "version",
    open: "Open", update: "Update", folder: "Folder", notReady: "not baked yet",
    range: "Area", near1: "3×3 regions around spawn", near2: "5×5 regions around spawn", near4: "9×9 regions around spawn", all: "The whole world",
    game: "Textures from game version", start: "Bake", about: "about", min: "min", sec: "s", busy: "Another map is baking",
    baking: "Baking", of: "/", left: "about", cancel: "Cancel", done: "Done", failed: "The bake failed",
    cancelled: "Cancelled", log: "Log", someFailed: "regions failed", mb: "MB", gb: "GB",
    foot: "Minecraft's textures belong to Mojang; prism is not affiliated with Mojang. Maps are kept in:",
    noJar: "No game versions found. Play any version once with the Minecraft launcher first.", lowMem: "Little memory free: baking may be slow.",
    drop: "Drop a world folder here", pickWorld: "or choose a world folder",
    dropHint: "Worlds are in %APPDATA%\\.minecraft\\saves (press Win+R, paste that, press Enter to open it), or in the game: Singleplayer → pick the world → Edit → Open World Folder. The whole saves folder works too. Everything happens in your browser; nothing is uploaded.",
    noWorldIn: "No world in there (a folder with a level.dat).", reading: "Reading…",
    jarWeb: "Textures: {v}, downloaded from Mojang (about 40 MB, once)", jarHave: "Textures: {v} (downloaded)", downloading: "Downloading the game's textures",
    dropAgain: "To update it, drop this world's folder here again.",
    webFoot: "Minecraft's textures belong to Mojang; prism is not affiliated with Mojang. Maps are kept in this browser.",
    noWeb: "This browser can't bake in the page (it needs a recent Chrome or Edge).",
};

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...kids) => {
    const e = Object.assign(document.createElement(tag), props);
    for (const k of kids) if (k != null) e.append(k);
    return e;
};
const size = (b) => (b >= 1 << 30 ? (b / (1 << 30)).toFixed(1) + " " + T.gb : Math.max(1, Math.round(b / (1 << 20))) + " " + T.mb);
const dur = (s) => (s < 90 ? Math.max(1, Math.round(s)) + " " + T.sec : Math.round(s / 60) + " " + T.min);

$("tagline").textContent = T.tagline;
$("h-maps").textContent = T.maps;
$("h-worlds").textContent = T.worlds;

let state = null, openWorld = null;

/* a region's bake on this computer, roughly: about 110 CPU-seconds, half
   as much again in the browser */
const perRegion = (cpus) => (state.web ? 160 : 110) / Math.min(Math.max(cpus, 1), 32);

/* prism serve's calls */
function serverBackend() {
    const call = (path, body) => fetch("api/" + path, {
        method: "POST", headers: { "Content-Type": "application/json", "X-Prism": "1" }, body: JSON.stringify(body || {}),
    }).then((r) => r.json());
    return {
        state: () => fetch("api/state").then((r) => r.json()),
        bake: (b) => call("bake", b),
        cancel: () => call("cancel"),
        open: (id) => call("open", { map: id }),
        iconUrl: (w) => "api/icon?w=" + encodeURIComponent(w.id),
        mapUrl: (id) => "viewer/?map=" + encodeURIComponent("/maps/" + id + "/"),
        logUrl: (id) => "maps/" + encodeURIComponent(id) + ".log",
        canOpenFolder: true,
    };
}

/* The browser's own: its threads need the page cross-origin isolated,
   which sw.js does (a reload, the first time). */
async function browserBackend() {
    if (!("serviceWorker" in navigator) || !("showDirectoryPicker" in window)) return null;
    if (!crossOriginIsolated) {
        await navigator.serviceWorker.register("sw.js");
        await navigator.serviceWorker.ready;
        let tries = 0;
        try { tries = +sessionStorage.getItem("prism-reload") || 0; } catch {}
        if (tries < 2) {
            try { sessionStorage.setItem("prism-reload", tries + 1); } catch {}
            location.reload();
            return new Promise(() => {});
        }
        return null;
    }
    try { sessionStorage.removeItem("prism-reload"); } catch {}
    return (await import("./web.js")).webBackend();
}

async function pickBackend() {
    try {
        const r = await fetch("api/state");
        if (r.ok && (r.headers.get("Content-Type") || "").includes("json")) return serverBackend();
    } catch {}
    return browserBackend();
}
let backend;

function drawJob() {
    const j = state.job, box = $("job");
    if (!j) { box.hidden = true; return; }
    box.hidden = false;
    box.replaceChildren();
    const frac = j.total ? j.done / j.total : 0;
    const title = j.running ? `${T.baking} ${j.map}` : j.cancelled ? `${j.map}: ${T.cancelled}`
        : j.code ? `${j.map}: ${T.failed}` : `${j.map}: ${T.done}`;
    box.append(el("div", { className: "name" }, title));
    if (j.running) {
        const avg = j.baked ? j.secs / j.baked : perRegion(state.cpus);
        const rest = Math.max(0, j.total - j.done) * avg;
        if (j.download != null) {
            box.append(el("div", { className: "bar" }, el("div", { style: `width:${(j.download * 100).toFixed(1)}%` })),
                el("div", { className: "row" }, el("span", { className: "meta" }, `${T.downloading} ${Math.round(j.download * 100)}%`),
                    el("button", { onclick: () => backend.cancel().then(refresh) }, T.cancel)));
            return;
        }
        box.append(
            el("div", { className: "bar" }, el("div", { style: `width:${(frac * 100).toFixed(1)}%` })),
            el("div", { className: "row" },
                el("span", { className: "meta" }, `${j.done} ${T.of} ${j.total || "?"} ${T.regions}` +
                    (j.current ? ` · ${j.current}` : "") + (j.total ? ` · ${T.left} ${dur(rest)}` : "")),
                el("button", { onclick: () => backend.cancel().then(refresh) }, T.cancel)));
    } else {
        const row = el("div", { className: "row" });
        if (!j.code && !j.cancelled) row.append(el("a", { className: "btn go", href: backend.mapUrl(j.map) }, T.open));
        if (j.failed) row.append(el("span", { className: "bad" }, `${j.failed} ${T.someFailed}`));
        if ((j.code || j.failed) && backend.logUrl(j.map)) row.append(el("a", { className: "btn", href: backend.logUrl(j.map), target: "_blank" }, T.log));
        box.append(row);
    }
    if (j.warning) box.append(el("div", { className: "meta bad" }, j.warning));
}

function drawMaps() {
    const box = $("maps"), busy = state.job && state.job.running;
    box.replaceChildren();
    if (!state.maps.length) { box.append(el("p", { className: "empty" }, T.noMaps)); return; }
    for (const m of state.maps) {
        const w = state.worlds.find((w) => w.id === m.id);
        const meta = m.ready ? `${m.regions} ${T.regions} · ${size(m.bytes)} · ${new Date(m.baked * 1000).toLocaleDateString()}` : T.notReady;
        const row = el("div", { className: "row" });
        if (m.ready) row.append(el("a", { className: "btn go", href: backend.mapUrl(m.id) }, T.open));
        if (m.settings && w) row.append(el("button", {
            disabled: busy, onclick: () => backend.bake(m.settings).then(refresh),
        }, T.update));
        else if (m.settings && state.web) row.append(el("span", { className: "meta", title: T.dropAgain }, T.dropAgain));
        if (backend.canOpenFolder) row.append(el("button", { onclick: () => backend.open(m.id) }, T.folder));
        box.append(el("div", { className: "card" },
            el("div", { className: "top" }, icon(w),
                el("div", { style: "min-width:0" }, el("div", { className: "name", title: m.id }, w ? w.name : (m.settings && m.settings.name) || m.id),
                    el("div", { className: "meta" }, meta))),
            row));
    }
}

function icon(w) {
    const src = w && w.icon && backend.iconUrl(w);
    return src ? el("img", { src, alt: "" }) : el("div", { className: "noicon" });
}

function drawWorlds() {
    const box = $("worlds");
    box.replaceChildren();
    $("mc").textContent = state.web ? "" : `${T.mcAt} ${state.mc}`;
    if (state.web) box.append(dropCard());
    if (!state.worlds.length && !state.web) { box.append(el("p", { className: "empty" }, `${T.noWorlds} ${state.mc}`)); return; }
    const worlds = [...state.worlds].sort((a, b) => b.played - a.played);
    for (const w of worlds) {
        const card = el("div", { className: "card pick" + (openWorld === w.id ? " open" : "") },
            el("div", { className: "top" }, icon(w),
                el("div", { style: "min-width:0" }, el("div", { className: "name", title: w.id }, w.name),
                    el("div", { className: "meta" }, `${w.version ? T.version + " " + w.version + " · " : ""}${w.regions} ${T.regions}`))));
        card.onclick = (e) => {
            if (e.target.closest(".opts")) return;
            openWorld = openWorld === w.id ? null : w.id;
            drawWorlds();
        };
        if (openWorld === w.id) card.append(options(w));
        box.append(card);
    }
}

function options(w) {
    const opts = el("div", { className: "opts" });
    if (!state.jars.length && !state.web) { opts.append(el("div", { className: "bad" }, T.noJar)); return opts; }
    const choices = [[1, T.near1], [2, T.near2], [4, T.near4], [-1, T.all]].filter(([r]) => r < 0 || (2 * r + 1) ** 2 < w.regions);
    const count = (r) => (r < 0 ? w.regions : Math.min(w.regions, (2 * r + 1) ** 2));
    let near = choices[0][0];
    const est = el("span", { className: "meta" });
    const showEst = () => { est.textContent = `${T.about} ${dur(count(near) * perRegion(state.cpus))}`; };
    const radios = el("div", {}, el("div", { className: "meta" }, T.range));
    for (const [r, label] of choices) {
        const input = el("input", { type: "radio", name: "near-" + w.id, checked: r === near, onchange: () => { near = r; showEst(); } });
        radios.append(el("label", {}, input, `${label} (${count(r)})`));
    }
    /* the world's own version when it is there, else the newest played */
    const jars = [...state.jars].sort((a, b) => b.mtime - a.mtime);
    const sel = el("select", {});
    for (const j of jars) sel.append(el("option", { value: j.ver, selected: j.ver === w.version }, j.ver));
    const busy = state.job && state.job.running;
    const go = el("button", { className: "go", disabled: busy }, busy ? T.busy : T.start);
    go.onclick = () => {
        go.disabled = true;
        backend.bake({ world: w.id, jar: sel.value, near }).then((r) => {
            if (r.error) alert(r.error);
            openWorld = null;
            refresh();
        });
    };
    showEst();
    /* in the browser, the world's own version, downloaded when needed */
    if (state.web) {
        const v = w.version || "?", have = state.jars.some((j) => j.ver === w.version);
        sel.replaceChildren(el("option", { value: w.version }, v));
        opts.append(radios, el("div", { className: "meta" }, (have ? T.jarHave : T.jarWeb).replace("{v}", v)));
    } else opts.append(radios, el("label", {}, T.game, sel));
    if (state.mem >= 0 && state.mem < 1800) opts.append(el("div", { className: "meta bad" }, T.lowMem));
    opts.append(el("div", { className: "row" }, go, est));
    return opts;
}

/* In the browser: worlds come as folders dropped on the page or chosen
   (the .minecraft folder can't be picked as a whole: Chrome refuses
   anything under AppData) */
function dropCard() {
    const msg = el("div", { className: "meta" });
    const input = el("input", { type: "file", webkitdirectory: true, multiple: true, hidden: true });
    const add = async (p) => {
        msg.textContent = T.reading;
        try {
            const n = await p;
            msg.textContent = n ? "" : T.noWorldIn;
        } catch (e) { msg.textContent = String(e && e.message || e); }
        drawnKey = "";
        refresh();
    };
    input.onchange = () => input.files.length && add(backend.addPicked(input.files));
    const card = el("div", { className: "card drop" },
        el("div", { className: "name" }, T.drop),
        el("div", { className: "row" }, el("button", { onclick: () => input.click() }, T.pickWorld), input),
        el("div", { className: "meta" }, T.dropHint), msg);
    dropTarget = (items) => add(backend.addDropped(items));
    return card;
}
let dropTarget = null;
document.addEventListener("dragover", (e) => { if (dropTarget) { e.preventDefault(); document.body.classList.add("dragging"); } });
document.addEventListener("dragleave", (e) => { if (!e.relatedTarget) document.body.classList.remove("dragging"); });
document.addEventListener("drop", (e) => {
    document.body.classList.remove("dragging");
    if (!dropTarget) return;
    e.preventDefault();
    dropTarget(e.dataTransfer.items);
});

let timer = 0, drawnKey = "";
async function refresh() {
    clearTimeout(timer);
    try {
        state = await backend.state();
        drawJob();
        /* the cards only when they changed: an open world's options keep
           what was picked in them */
        const key = JSON.stringify([state.maps, state.worlds, state.jars, !!(state.job && state.job.running)]);
        if (key !== drawnKey) { drawnKey = key; drawMaps(); drawWorlds(); }
        $("foot").textContent = state.web ? T.webFoot : `${T.foot} ${state.data}`;
    } catch {}
    timer = setTimeout(refresh, state && state.job && state.job.running ? 1000 : 4000);
}
backend = await pickBackend();
if (backend) refresh();
else $("worlds").replaceChildren(el("p", { className: "bad" }, T.noWeb));
