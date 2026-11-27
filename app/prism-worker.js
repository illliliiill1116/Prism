/* One run of prism (the browser build, prism.js + prism.wasm, see make
   web) in a worker of its own, as a process would be: it is ended when the
   run is done, which gives its memory back (WebAssembly's can only grow).

   The page posts { args, files, preload, collect, pool }:
     args     the command line after "prism"
     files    [{ name, data: File, mtime }], read-only under /mc (lazily:
              only what the run reads is read)
     preload  [{ path, data: ArrayBuffer }], written before the run
     collect  a folder whose files are posted back after it
     pool     the threads to start beforehand (the most the run starts:
              one can't be started while the run waits for it)
   and gets { line } for each line printed, then { done, code, files:
   [{ path, data }] } (paths under collect). */
import createPrism from "./prism.js";

self.onmessage = async ({ data: job }) => {
    const print = (line) => self.postMessage({ line });
    try {
        const M = await createPrism({ pool: job.pool || 0, print, printErr: print });
        const { FS } = M;
        FS.mkdir("/mc");
        FS.mount(M.WORKERFS, { blobs: job.files || [] }, "/mc");
        /* a blob's time is now unless told: prism map compares them */
        for (const f of job.files || []) if (f.mtime) FS.utime("/mc/" + f.name, f.mtime, f.mtime);
        for (const p of job.preload || []) {
            mkdirs(FS, p.path.slice(0, p.path.lastIndexOf("/")));
            FS.writeFile(p.path, new Uint8Array(p.data));
        }
        /* the output's parent: a bake makes its folder, not the ones above */
        if (job.collect) mkdirs(FS, job.collect.slice(0, job.collect.lastIndexOf("/")));
        let code;
        try {
            code = M.callMain(job.args);
        } catch (e) {
            if (e && e.name === "ExitStatus") code = e.status;
            else throw e;
        }
        const files = [];
        if (job.collect && FS.analyzePath(job.collect).exists) walk(FS, job.collect, "", files);
        self.postMessage({ done: true, code, files }, files.map((f) => f.data));
    } catch (e) {
        print("error: " + (e && e.message ? e.message : e));
        self.postMessage({ done: true, code: 1, files: [] });
    }
};

function mkdirs(FS, dir) {
    let p = "";
    for (const part of dir.split("/").filter(Boolean)) {
        p += "/" + part;
        if (!FS.analyzePath(p).exists) FS.mkdir(p);
    }
}

function walk(FS, dir, rel, out) {
    for (const name of FS.readdir(dir)) {
        if (name === "." || name === "..") continue;
        const path = dir + "/" + name, r = rel ? rel + "/" + name : name;
        if (FS.isDir(FS.stat(path).mode)) walk(FS, path, r, out);
        else out.push({ path: r, data: FS.readFile(path).buffer });
    }
}
