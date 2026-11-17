/*
 * prism serve: prism as an app in the browser, for whoever would rather not
 * type commands. A small web server on this computer only (127.0.0.1)
 * that lists the worlds in Minecraft's saves folder, bakes the one picked
 * (prism map, in a process of its own) and shows the maps baked so far in
 * the viewer. Running prism with no arguments (double-clicking prism.exe)
 * does this and opens the browser.
 *
 *   prism serve [--port n] [--mc dir] [--data dir] [--no-open] [--www dir]
 *
 * --mc is Minecraft's folder (by default %APPDATA%\.minecraft, or
 * ~/.minecraft), --data where the maps go (%LOCALAPPDATA%\Prism, or
 * ~/.local/share/prism): <data>/maps/<world folder's name>/, each a map as
 * prism map makes it, with prism's own settings for it in prism.json and
 * the bake's output in <name>.log next to it. --www serves the page's files
 * (app/, viewer/) from that folder instead of the copies built in, for
 * working on them.
 *
 * The page: / (app/), the viewer at /viewer/, the maps at /maps/ (Range
 * requests, as the viewer reads tiles), and its calls at /api/: GET state
 * (worlds, game versions, maps, the bake under way), GET icon?w= (a
 * world's icon.png), GET viewer-files (the viewer's scripts, for the
 * page's Export), GET regions?w= and overview?w=&jar=&r= (the area
 * picker's), POST bake {world, jar, near | clip}, POST cancel, POST open
 * {map} (its folder in the file manager), POST delete {map}. A POST must
 * carry X-Prism: 1 (a page elsewhere can't send that without asking first, which this server
 * never allows) and every request a Host of this server's own (so no other
 * name can be made to point here).
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define close_sock closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define close_sock close
#endif
#include "cli.h"
#include <dirent.h>
#include <sched.h>
#include <strings.h>
#include "nbt_io.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#else
#define fseek64 fseeko
#endif

typedef struct { const char *path; const unsigned char *data; size_t len; } embedded_t;
extern const embedded_t embedded[]; /* app/ and viewer/, see scripts/embed.sh */

static char mc_dir[2048], data_dir[2048], maps_dir[2048], www_dir[2048];
static int port;

/* -------- the bake under way -------- */

static struct {
    int running, code, cancelled;
    plat_proc_t proc;
    char map[512], log[2600];
    time_t started;
} job;
static atomic_flag job_busy = ATOMIC_FLAG_INIT;
static void job_lock(void) { while (atomic_flag_test_and_set_explicit(&job_busy, memory_order_acquire)) sched_yield(); }
static void job_unlock(void) { atomic_flag_clear_explicit(&job_busy, memory_order_release); }

/* -------- small helpers -------- */

static int query_get(const char *q, const char *key, char *out, size_t len);

static long long file_size(const char *path, time_t *mtime) {
    struct stat st;
    if (stat(path, &st)) return -1;
    if (mtime) *mtime = st.st_mtime;
    return (long long)st.st_size;
}

/* the bytes of all files under path */
static long long tree_size(const char *path) {
    DIR *d = opendir(path);
    if (!d) { long long n = file_size(path, NULL); return n < 0 ? 0 : n; }
    long long sum = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        sum += plat_is_dir(sub) ? tree_size(sub) : (file_size(sub, NULL) > 0 ? file_size(sub, NULL) : 0);
    }
    closedir(d);
    return sum;
}

/* whether the file (its first 64 KB) has text in it */
static int file_has(const char *path, const char *text) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static _Thread_local char buf[65537];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, text) != NULL;
}

static int count_regions(const char *rdir) {
    DIR *d = opendir(rdir);
    int n = 0, rx, rz, len;
    for (struct dirent *e; d && (e = readdir(d));)
        if (sscanf(e->d_name, "r.%d.%d.mca%n", &rx, &rz, &len) == 2 && !e->d_name[len]) n++;
    if (d) closedir(d);
    return n;
}

/* A folder's name fit to be a map's: no separators, no dots in front. */
static int good_name(const char *s) {
    if (!*s || *s == '.' || strlen(s) > 200) return 0;
    for (; *s; s++)
        if (*s == '/' || *s == '\\' || *s == ':' || (unsigned char)*s < 0x20) return 0;
    return 1;
}

/* -------- the bake's progress, from its log -------- */

static void job_json(str_t *o) {
    job_lock();
    if (job.running) {
        int code;
        if (plat_proc_poll(&job.proc, &code)) { job.running = 0; job.code = code; }
    }
    int running = job.running, code = job.code, cancelled = job.cancelled;
    char map[512], log[2600];
    snprintf(map, sizeof map, "%s", job.map);
    snprintf(log, sizeof log, "%s", job.log);
    time_t started = job.started;
    job_unlock();
    if (!map[0]) { sappend(o, "null"); return; }
    /* prism map prints "regions: n", then a line a region: "r.x.z: up to
       date" or "r.x.z: baking (why)... 7.1 s" (the time once it is done) */
    size_t len = 0;
    FILE *f = fopen(log, "rb");
    char *text = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && (text = malloc((size_t)n + 1)) && fread(text, 1, (size_t)n, f) == (size_t)n) len = (size_t)n;
        fclose(f);
    }
    int total = 0, done = 0, baked = 0, failed = 0;
    double secs = 0;
    char current[64] = "", warning[256] = "";
    for (size_t i = 0; text && i < len;) {
        char *line = text + i, *nl = memchr(line, '\n', len - i);
        size_t ll = nl ? (size_t)(nl - line) : len - i;
        i += ll + 1;
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s", (int)(ll < 511 ? ll : 511), line);
        int rx, rz;
        double s;
        if (sscanf(buf, "regions: %d", &total) == 1) continue;
        if (!strncmp(buf, "warning:", 8) || !strncmp(buf, "  warning:", 10) || !strncmp(buf, "can't", 5) || !strncmp(buf, "no region", 9))
            snprintf(warning, sizeof warning, "%s", buf);
        if (sscanf(buf, "r.%d.%d:", &rx, &rz) != 2) continue;
        if (!nl) { snprintf(current, sizeof current, "r.%d.%d", rx, rz); continue; }
        done++;
        char *dots = strstr(buf, "... ");
        if (dots && sscanf(dots + 4, "%lf s", &s) == 1) { secs += s; baked++; }
        else if (dots && strstr(dots, "failed")) failed++;
    }
    free(text);
    sappend(o, "{\"map\":");
    json_str(o, map);
    sappend(o, ",\"running\":%s,\"code\":%d,\"cancelled\":%s,\"elapsed\":%lld,\"total\":%d,\"done\":%d,\"baked\":%d,"
            "\"failed\":%d,\"secs\":%.1f,\"current\":", running ? "true" : "false", code, cancelled ? "true" : "false",
            (long long)(time(NULL) - started), total, done, baked, failed, secs);
    json_str(o, current);
    sappend(o, ",\"warning\":");
    json_str(o, warning);
    sappend(o, "}");
}

/* -------- the calls -------- */

static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* the names in dir, sorted (free each and the list) */
static int list_dir(const char *dir, char ***out) {
    DIR *d = opendir(dir);
    char **v = NULL;
    int n = 0, cap = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) v = realloc(v, (size_t)(cap = cap ? cap * 2 : 32) * sizeof *v);
        v[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    if (n) qsort(v, (size_t)n, sizeof *v, by_str);
    *out = v;
    return n;
}

static void api_state(str_t *o) {
    char path[4096], rdir[4096];
    char **names;
    sappend(o, "{\"mc\":");
    json_str(o, mc_dir);
    sappend(o, ",\"data\":");
    json_str(o, maps_dir);
    sappend(o, ",\"cpus\":%d,\"mem\":%ld,\"worlds\":[", plat_ncpu(), plat_mem_available_mb());
    snprintf(path, sizeof path, "%s/saves", mc_dir);
    int n = list_dir(path, &names), first = 1;
    for (int i = 0; i < n; i++) {
        char world[4096], dat[4096];
        snprintf(world, sizeof world, "%s/saves/%s", mc_dir, names[i]);
        snprintf(dat, sizeof dat, "%s/level.dat", world);
        time_t mt = 0;
        if (file_size(dat, &mt) < 0) { free(names[i]); continue; }
        level_t lv = { 0 };
        read_level(world, &lv);
        region_dir(world, rdir, sizeof rdir);
        snprintf(dat, sizeof dat, "%s/icon.png", world);
        sappend(o, "%s{\"id\":", first ? "" : ",");
        json_str(o, names[i]);
        sappend(o, ",\"name\":");
        json_str(o, lv.name[0] ? lv.name : names[i]);
        sappend(o, ",\"version\":");
        json_str(o, lv.version);
        sappend(o, ",\"spawn\":[%d,%d],\"regions\":%d,\"icon\":%s,\"played\":%lld}", lv.spawn_x, lv.spawn_z,
                count_regions(rdir), file_size(dat, NULL) > 0 ? "true" : "false", (long long)mt);
        first = 0;
        free(names[i]);
    }
    free(names);
    sappend(o, "],\"jars\":[");
    snprintf(path, sizeof path, "%s/versions", mc_dir);
    n = list_dir(path, &names);
    first = 1;
    for (int i = 0; i < n; i++) {
        char jar[4096], js[4096];
        time_t mt = 0;
        snprintf(jar, sizeof jar, "%s/versions/%s/%s.jar", mc_dir, names[i], names[i]);
        snprintf(js, sizeof js, "%s/versions/%s/%s.json", mc_dir, names[i], names[i]);
        /* a mod loader's version builds on another ("inheritsFrom") and its
           jar, if any, is a copy or a stub */
        if (file_size(jar, &mt) > (4 << 20) && !file_has(js, "\"inheritsFrom\"")) {
            sappend(o, "%s{\"ver\":", first ? "" : ",");
            json_str(o, names[i]);
            sappend(o, ",\"mtime\":%lld}", (long long)mt);
            first = 0;
        }
        free(names[i]);
    }
    free(names);
    sappend(o, "],\"maps\":[");
    n = list_dir(maps_dir, &names);
    first = 1;
    for (int i = 0; i < n; i++) {
        char dir[4096], mj[4096], pj[4096];
        snprintf(dir, sizeof dir, "%s/%s", maps_dir, names[i]);
        if (!plat_is_dir(dir) || !good_name(names[i]) || !strcmp(names[i], "skins")) { free(names[i]); continue; }
        snprintf(mj, sizeof mj, "%s/map.json", dir);
        snprintf(pj, sizeof pj, "%s/prism.json", dir);
        time_t mt = 0;
        int ready = file_size(mj, &mt) > 0;
        size_t plen = 0;
        char *settings = NULL;
        FILE *f = fopen(pj, "rb");
        if (f) {
            settings = calloc(1, 4097);
            if (settings) plen = fread(settings, 1, 4096, f);
            fclose(f);
        }
        int regions = 0;
        char **sub;
        int ns = list_dir(dir, &sub);
        for (int k = 0; k < ns; k++) {
            int rx, rz, len = 0;
            if (sscanf(sub[k], "r.%d.%d%n", &rx, &rz, &len) == 2 && !sub[k][len]) regions++;
            free(sub[k]);
        }
        free(sub);
        sappend(o, "%s{\"id\":", first ? "" : ",");
        json_str(o, names[i]);
        sappend(o, ",\"ready\":%s,\"baked\":%lld,\"regions\":%d,\"bytes\":%lld,\"settings\":%s}", ready ? "true" : "false",
                (long long)mt, regions, tree_size(dir), plen && settings[0] == '{' ? settings : "null");
        free(settings);
        first = 0;
        free(names[i]);
    }
    free(names);
    sappend(o, "],\"job\":");
    job_json(o);
    sappend(o, "}");
}

/* POST bake {world, jar, near} or {world, jar, clip}: near is a radius in
 * regions around the spawn, or -1 for all of the world; clip, [x0, z0, x1,
 * z1, ymin], an area of it (prism map --clip), a map of its own */
static int api_bake(const char *body, size_t len, str_t *o) {
    json_pool_t pool;
    json_pool_init(&pool);
    json_value_t *root, *v;
    char world_id[512] = "", jar_ver[256] = "";
    int radius = -1, clip[5], nclip = 0;
    if (json_parse(&pool, body, len, &root) == JSON_OK && root->type == JSON_OBJECT) {
        if ((v = json_object_getz(&pool, root, "clip")) && v->type == JSON_ARRAY && json_array_count(v) == 5)
            for (nclip = 0; nclip < 5; nclip++) {
                json_value_t *e = json_array_at(&pool, v, (uint32_t)nclip);
                if (e->type != JSON_NUMBER) break;
                clip[nclip] = (int)e->number;
            }
        if ((v = json_object_getz(&pool, root, "world")) && v->type == JSON_STRING)
            snprintf(world_id, sizeof world_id, "%.*s", (int)v->string.len, v->string.ptr);
        if ((v = json_object_getz(&pool, root, "jar")) && v->type == JSON_STRING)
            snprintf(jar_ver, sizeof jar_ver, "%.*s", (int)v->string.len, v->string.ptr);
        if ((v = json_object_getz(&pool, root, "near")) && v->type == JSON_NUMBER) radius = (int)v->number;
    }
    json_pool_free(&pool);
    if (!good_name(world_id) || !good_name(jar_ver)) { sappend(o, "{\"error\":\"bad world or version\"}"); return 400; }
    if (nclip && nclip != 5) { sappend(o, "{\"error\":\"bad area\"}"); return 400; }
    /* an area of a world is a map of its own, named after it (app.js's areaId) */
    char map_id[600], clipArg[96] = "";
    if (nclip) {
        if (clip[0] > clip[2]) { int t = clip[0]; clip[0] = clip[2]; clip[2] = t; }
        if (clip[1] > clip[3]) { int t = clip[1]; clip[1] = clip[3]; clip[3] = t; }
        snprintf(clipArg, sizeof clipArg, "%d,%d,%d,%d,%d", clip[0], clip[1], clip[2], clip[3], clip[4]);
        snprintf(map_id, sizeof map_id, "%s %d,%d to %d,%d", world_id, clip[0], clip[1], clip[2], clip[3]);
    } else snprintf(map_id, sizeof map_id, "%s", world_id);
    if (!good_name(map_id)) { sappend(o, "{\"error\":\"bad world\"}"); return 400; }

    char world[4096], jar[4096], out[4096], log[4096], pj[4096];
    snprintf(world, sizeof world, "%s/saves/%s", mc_dir, world_id);
    snprintf(jar, sizeof jar, "%s/versions/%s/%s.jar", mc_dir, jar_ver, jar_ver);
    snprintf(out, sizeof out, "%s/%s", maps_dir, map_id);
    snprintf(log, sizeof log, "%s/%s.log", maps_dir, map_id);
    if (!plat_is_dir(world) || file_size(jar, NULL) <= 0) { sappend(o, "{\"error\":\"no such world or version\"}"); return 404; }
    level_t lv = { 0 };
    read_level(world, &lv);
    char nearArg[64];
    snprintf(nearArg, sizeof nearArg, "%d,%d,%d", lv.spawn_x, lv.spawn_z, radius);
    const char *argv[] = { "prism", "map", jar, world, out, nclip ? "--clip" : radius >= 0 ? "--near" : NULL,
                           nclip ? clipArg : nearArg, NULL };

    job_lock();
    if (job.running) { job_unlock(); sappend(o, "{\"error\":\"a bake is under way\"}"); return 409; }
    plat_mkdir(out);
    str_t s = { 0 };
    sappend(&s, "{\"world\":");
    json_str(&s, world_id);
    sappend(&s, ",\"jar\":");
    json_str(&s, jar_ver);
    sappend(&s, ",\"near\":%d", nclip ? -1 : radius);
    if (nclip) sappend(&s, ",\"clip\":[%s]", clipArg);
    sappend(&s, "}\n");
    snprintf(pj, sizeof pj, "%s/prism.json", out);
    write_atomic(pj, s.p, s.n);
    free(s.p);
    remove(log);
    int rc = plat_proc_start(&job.proc, argv, log);
    if (!rc) {
        job.running = 1;
        job.code = 0;
        job.cancelled = 0;
        job.started = time(NULL);
        snprintf(job.map, sizeof job.map, "%s", map_id);
        snprintf(job.log, sizeof job.log, "%s", log);
    }
    job_unlock();
    if (rc) { sappend(o, "{\"error\":\"can't start prism\"}"); return 500; }
    sappend(o, "{\"ok\":true}");
    return 200;
}

/* GET regions?w=: the world's region files, [[rx, rz], ...] (the
 * picker's, see app/picker.js) */
static int api_regions(const char *w, str_t *o) {
    char world[4096], rdir[4096];
    if (!good_name(w)) { sappend(o, "{\"error\":\"bad world\"}"); return 400; }
    snprintf(world, sizeof world, "%s/saves/%s", mc_dir, w);
    region_dir(world, rdir, sizeof rdir);
    DIR *d = opendir(rdir);
    if (!d) { sappend(o, "{\"error\":\"no such world\"}"); return 404; }
    sappend(o, "{\"regions\":[");
    int n = 0, rx, rz, len;
    for (struct dirent *e; (e = readdir(d));) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", rdir, e->d_name);
        if (sscanf(e->d_name, "r.%d.%d.mca%n", &rx, &rz, &len) == 2 && !e->d_name[len] && file_size(path, NULL) > 0)
            sappend(o, "%s[%d,%d]", n++ ? "," : "", rx, rz);
    }
    closedir(d);
    sappend(o, "]}");
    return 200;
}

/* One prism overview at a time: each takes a bake's memory */
static atomic_flag ov_busy = ATOMIC_FLAG_INIT;

/* GET overview?w=&jar=&r=rx,rz: a region's picture (prism overview), made
 * when there is none newer than the region file and prism, kept under
 * <data>/overviews/<world>/<version>/; its json with "png", the picture's
 * URL, added. */
static int api_overview(const char *q, str_t *o) {
    char w[512], ver[256], rs[64], world[4096], rdir[4096], jar[4096], dir[4096], js[4096], mca[4096];
    int rx, rz;
    if (!query_get(q, "w", w, sizeof w) || !good_name(w) || !query_get(q, "jar", ver, sizeof ver) || !good_name(ver) ||
        !query_get(q, "r", rs, sizeof rs) || sscanf(rs, "%d,%d", &rx, &rz) != 2) {
        sappend(o, "{\"error\":\"bad request\"}");
        return 400;
    }
    snprintf(world, sizeof world, "%s/saves/%s", mc_dir, w);
    region_dir(world, rdir, sizeof rdir);
    snprintf(mca, sizeof mca, "%s/r.%d.%d.mca", rdir, rx, rz);
    snprintf(jar, sizeof jar, "%s/versions/%s/%s.jar", mc_dir, ver, ver);
    time_t reg_t, js_t;
    if (file_size(mca, &reg_t) <= 0 || file_size(jar, NULL) <= 0) { sappend(o, "{\"error\":\"no such region or version\"}"); return 404; }
    snprintf(dir, sizeof dir, "%s/overviews/%s/%s", data_dir, w, ver);
    snprintf(js, sizeof js, "%s/r.%d.%d.json", dir, rx, rz);
    if (file_size(js, &js_t) <= 0 || js_t <= reg_t || (long long)js_t <= plat_self_mtime()) {
        while (atomic_flag_test_and_set_explicit(&ov_busy, memory_order_acquire)) plat_sleep_ms(50);
        char sub[4096], log[4096], sx[16], sz[16];
        snprintf(sub, sizeof sub, "%s/overviews", data_dir);
        plat_mkdir(sub);
        snprintf(sub, sizeof sub, "%s/overviews/%s", data_dir, w);
        plat_mkdir(sub);
        plat_mkdir(dir);
        snprintf(log, sizeof log, "%s/r.%d.%d.log", dir, rx, rz);
        snprintf(sx, sizeof sx, "%d", rx);
        snprintf(sz, sizeof sz, "%d", rz);
        const char *argv[] = { "prism", "overview", jar, world, dir, sx, sz, NULL };
        plat_proc_t p;
        int rc = plat_proc_start(&p, argv, log) || plat_proc_wait(&p);
        atomic_flag_clear_explicit(&ov_busy, memory_order_release);
        if (rc || file_size(js, &js_t) <= 0) { sappend(o, "{\"error\":\"can't draw it\"}"); return 500; }
        remove(log);
    }
    FILE *f = fopen(js, "rb");
    char text[512] = "";
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    text[n] = 0;
    char *end = strrchr(text, '}');
    if (!end) { sappend(o, "{\"error\":\"can't read it\"}"); return 500; }
    *end = 0;
    sappend(o, "%s,\"png\":", text);
    char url[1024];
    snprintf(url, sizeof url, "overviews/%s/%s/r.%d.%d.png?%lld", w, ver, rx, rz, (long long)js_t);
    json_str(o, url);
    sappend(o, "}");
    return 200;
}

static int api_cancel(str_t *o) {
    job_lock();
    if (job.running) { plat_proc_kill(&job.proc); job.cancelled = 1; }
    job_unlock();
    sappend(o, "{\"ok\":true}");
    return 200;
}

/* A call's {map}: into id, "" when it has none */
static void body_map(const char *body, size_t len, char id[512]) {
    json_pool_t pool;
    json_pool_init(&pool);
    json_value_t *root, *v;
    id[0] = 0;
    if (json_parse(&pool, body, len, &root) == JSON_OK && root->type == JSON_OBJECT &&
        (v = json_object_getz(&pool, root, "map")) && v->type == JSON_STRING)
        snprintf(id, 512, "%.*s", (int)v->string.len, v->string.ptr);
    json_pool_free(&pool);
}

static int api_open(const char *body, size_t len, str_t *o) {
    char id[512];
    body_map(body, len, id);
    char dir[4096];
    if (id[0] && !good_name(id)) { sappend(o, "{\"error\":\"bad map\"}"); return 400; }
    snprintf(dir, sizeof dir, "%s%s%s", maps_dir, id[0] ? "/" : "", id);
#ifdef _WIN32
    for (char *p = dir; *p; p++) if (*p == '/') *p = '\\';
#endif
    if (!plat_is_dir(dir)) { sappend(o, "{\"error\":\"no such map\"}"); return 404; }
    plat_open(dir);
    sappend(o, "{\"ok\":true}");
    return 200;
}

/* POST delete {map}: the map's folder and its bake's log, gone (the
 * world is not touched); not while it is being baked. */
static int api_delete(const char *body, size_t len, str_t *o) {
    char id[512];
    body_map(body, len, id);
    if (!id[0] || !good_name(id) || !strcmp(id, "skins")) { sappend(o, "{\"error\":\"bad map\"}"); return 400; }
    char dir[4096], log[4096];
    snprintf(dir, sizeof dir, "%s/%s", maps_dir, id);
    snprintf(log, sizeof log, "%s/%s.log", maps_dir, id);
    if (!plat_is_dir(dir)) { sappend(o, "{\"error\":\"no such map\"}"); return 404; }
    job_lock();
    if (job.running && !strcmp(job.map, id)) { job_unlock(); sappend(o, "{\"error\":\"it is being baked\"}"); return 409; }
    if (!strcmp(job.map, id)) job.map[0] = 0; /* its bake's box goes too */
    job_unlock();
    plat_rmtree(dir);
    remove(log);
    if (plat_is_dir(dir)) { sappend(o, "{\"error\":\"can't delete it all (a file in use?)\"}"); return 500; }
    sappend(o, "{\"ok\":true}");
    return 200;
}

/* -------- HTTP -------- */

typedef struct {
    sock_t s;
    char buf[16384];
    size_t have;
} conn_t;

typedef struct {
    char method[8], path[2048], query[2048];
    char host[256], range[128], ims[64], xprism[8], conn[32];
    long long clen;
} req_t;

static int send_all(sock_t s, const void *p, size_t n) {
    const char *c = p;
    while (n) {
#ifdef MSG_NOSIGNAL
        long w = send(s, c, n > (1 << 20) ? (1 << 20) : (int)n, MSG_NOSIGNAL);
#else
        long w = send(s, c, n > (1 << 20) ? (1 << 20) : (int)n, 0);
#endif
        if (w <= 0) return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 416: return "Range Not Satisfiable";
    default: return "Internal Server Error";
    }
}

static int respond(conn_t *c, int code, const char *type, const void *body, size_t n, const char *extra, int head) {
    char h[1024];
    int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
                      "X-Content-Type-Options: nosniff\r\n\r\n", code, status_text(code), type, n, extra ? extra : "");
    if (send_all(c->s, h, (size_t)hl)) return -1;
    return head || !n ? 0 : send_all(c->s, body, n);
}

static const char *mime(const char *path) {
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".js")) return "text/javascript; charset=utf-8";
    if (!strcmp(e, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".png")) return "image/png";
    if (!strcmp(e, ".svg")) return "image/svg+xml";
    if (!strcmp(e, ".log")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void http_date(time_t t, char *out, size_t len) {
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(out, len, "%s, %02d %s %04d %02d:%02d:%02d GMT", wd[tm.tm_wday], tm.tm_mday, mo[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* A file from disk, a part of it for a Range request. */
static int serve_file(conn_t *c, const req_t *r, const char *path, int head) {
    time_t mt;
    long long size = file_size(path, &mt);
    if (size < 0 || plat_is_dir(path)) return respond(c, 404, "text/plain", "not found", 9, NULL, head);
    char date[64], extra[512];
    http_date(mt, date, sizeof date);
    if (!r->range[0] && r->ims[0] && !strcmp(r->ims, date)) {
        snprintf(extra, sizeof extra, "Last-Modified: %s\r\nCache-Control: no-cache\r\n", date);
        return respond(c, 304, mime(path), NULL, 0, extra, 1);
    }
    long long a = 0, b = size - 1;
    int code = 200;
    if (r->range[0]) {
        long long x = -1, y = -1;
        if (sscanf(r->range, "bytes=%lld-%lld", &x, &y) >= 1 && x >= 0) {
            if (x >= size) {
                snprintf(extra, sizeof extra, "Content-Range: bytes */%lld\r\n", size);
                return respond(c, 416, "text/plain", "", 0, extra, head);
            }
            a = x;
            if (y >= x && y < size) b = y;
            code = 206;
        } else if (sscanf(r->range, "bytes=-%lld", &y) == 1 && y > 0) {
            a = size > y ? size - y : 0;
            code = 206;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) return respond(c, 404, "text/plain", "not found", 9, NULL, head);
    int n = snprintf(extra, sizeof extra, "Last-Modified: %s\r\nCache-Control: no-cache\r\nAccept-Ranges: bytes\r\n", date);
    if (code == 206) snprintf(extra + n, sizeof extra - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", a, b, size);
    char h[1024];
    long long len = size ? b - a + 1 : 0;
    int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\n%s\r\n", code,
                      status_text(code), mime(path), len, extra);
    int bad = send_all(c->s, h, (size_t)hl);
    if (!bad && !head && len) {
        fseek64(f, a, SEEK_SET);
        static _Thread_local char chunk[1 << 16];
        while (len > 0 && !bad) {
            size_t want = len > (long long)sizeof chunk ? sizeof chunk : (size_t)len, got = fread(chunk, 1, want, f);
            if (!got) { bad = 1; break; }
            bad = send_all(c->s, chunk, got);
            len -= (long long)got;
        }
    }
    fclose(f);
    return bad ? -1 : 0;
}

/* %XX decoded in place; 0 if it is malformed or decodes to a NUL */
static int url_decode(char *s) {
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '%') {
            int v;
            if (!p[1] || !p[2] || sscanf(p + 1, "%2x", &v) != 1 || !v) return 0;
            *w++ = (char)v;
            p += 2;
        } else *w++ = *p == '+' ? ' ' : *p;
    }
    *w = 0;
    return 1;
}

/* a path under a folder of ours: no "..", no ".", no backslash or colon */
static int safe_rel(const char *p) {
    if (!*p || strchr(p, '\\') || strchr(p, ':')) return 0;
    for (const char *s = p; *s;) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += n + (e ? 1 : 0);
    }
    return 1;
}

static int query_get(const char *q, const char *key, char *out, size_t len) {
    size_t kl = strlen(key);
    for (const char *s = q; s && *s;) {
        const char *e = strchr(s, '&');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n > kl && !strncmp(s, key, kl) && s[kl] == '=') {
            snprintf(out, len, "%.*s", (int)(n - kl - 1), s + kl + 1);
            return url_decode(out);
        }
        s = e ? e + 1 : NULL;
    }
    return 0;
}

static int serve_static(conn_t *c, const req_t *r, const char *rel, int head) {
    if (www_dir[0]) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", www_dir, rel);
        return serve_file(c, r, path, head);
    }
    for (const embedded_t *e = embedded; e->path; e++)
        if (!strcmp(e->path, rel)) return respond(c, 200, mime(rel), e->data, e->len, "Cache-Control: no-cache\r\n", head);
    return respond(c, 404, "text/plain", "not found", 9, NULL, head);
}

/* The viewer's scripts, as a JSON list of paths under viewer/: what the
 * app's Export puts in a map's site (app/export.js). */
static void api_viewer_files(str_t *o) {
    sappend(o, "[");
    int n = 0;
    if (www_dir[0]) {
        char dir[4096];
        snprintf(dir, sizeof dir, "%s/viewer/js", www_dir);
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d));) {
            size_t len = strlen(e->d_name);
            if (len > 3 && !strcmp(e->d_name + len - 3, ".js") && good_name(e->d_name))
                sappend(o, "%s\"js/%s\"", n++ ? "," : "", e->d_name);
        }
        if (d) closedir(d);
    } else
        for (const embedded_t *e = embedded; e->path; e++)
            if (!strncmp(e->path, "viewer/js/", 10)) sappend(o, "%s\"%s\"", n++ ? "," : "", e->path + 7);
    sappend(o, "]");
}

static int handle(conn_t *c, req_t *r, const char *body) {
    int head = !strcmp(r->method, "HEAD"), get = head || !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    /* only our own name: see the top */
    char h1[64], h2[64];
    snprintf(h1, sizeof h1, "127.0.0.1:%d", port);
    snprintf(h2, sizeof h2, "localhost:%d", port);
    if (strcmp(r->host, h1) && strcmp(r->host, h2)) return respond(c, 403, "text/plain", "forbidden", 9, NULL, head);
    char *path = r->path;
    if (!url_decode(path)) return respond(c, 400, "text/plain", "bad path", 8, NULL, head);

    if (!strncmp(path, "/api/", 5)) {
        str_t o = { 0 };
        int code = 200;
        if (post && strcmp(r->xprism, "1")) code = 403, sappend(&o, "{\"error\":\"forbidden\"}");
        else if (get && !strcmp(path, "/api/state")) api_state(&o);
        else if (get && !strcmp(path, "/api/viewer-files")) api_viewer_files(&o);
        else if (get && !strcmp(path, "/api/regions")) {
            char w[512];
            code = query_get(r->query, "w", w, sizeof w) ? api_regions(w, &o) : (sappend(&o, "{\"error\":\"bad world\"}"), 400);
        } else if (get && !strcmp(path, "/api/overview")) code = api_overview(r->query, &o);
        else if (get && !strcmp(path, "/api/icon")) {
            char w[512], p[4096];
            if (!query_get(r->query, "w", w, sizeof w) || !good_name(w)) return respond(c, 400, "text/plain", "bad world", 9, NULL, head);
            snprintf(p, sizeof p, "%s/saves/%s/icon.png", mc_dir, w);
            return serve_file(c, r, p, head);
        } else if (post && !strcmp(path, "/api/bake")) code = api_bake(body, (size_t)r->clen, &o);
        else if (post && !strcmp(path, "/api/cancel")) code = api_cancel(&o);
        else if (post && !strcmp(path, "/api/open")) code = api_open(body, (size_t)r->clen, &o);
        else if (post && !strcmp(path, "/api/delete")) code = api_delete(body, (size_t)r->clen, &o);
        else code = 404, sappend(&o, "{\"error\":\"no such call\"}");
        int rc = respond(c, code, "application/json", o.p, o.n, "Cache-Control: no-store\r\n", head);
        free(o.p);
        return rc;
    }
    if (!get) return respond(c, 405, "text/plain", "method not allowed", 18, NULL, 0);
    if (!strncmp(path, "/maps/", 6)) {
        if (!safe_rel(path + 6)) return respond(c, 400, "text/plain", "bad path", 8, NULL, head);
        char file[4096];
        snprintf(file, sizeof file, "%s/%s", maps_dir, path + 6);
        return serve_file(c, r, file, head);
    }
    if (!strncmp(path, "/overviews/", 11)) {
        if (!safe_rel(path + 11)) return respond(c, 400, "text/plain", "bad path", 8, NULL, head);
        char file[4096];
        snprintf(file, sizeof file, "%s/overviews/%s", data_dir, path + 11);
        return serve_file(c, r, file, head);
    }
    if (!strcmp(path, "/")) return serve_static(c, r, "app/index.html", head);
    if (!strcmp(path, "/viewer") || !strcmp(path, "/viewer/")) return serve_static(c, r, "viewer/index.html", head);
    if ((!strncmp(path, "/viewer/", 8) || !strncmp(path, "/app/", 5)) && safe_rel(path + 1))
        return serve_static(c, r, path + 1, head);
    return respond(c, 404, "text/plain", "not found", 9, NULL, head);
}

static void header_value(const char *line, const char *name, char *out, size_t len) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) || line[n] != ':') return;
    line += n + 1;
    while (*line == ' ' || *line == '\t') line++;
    snprintf(out, len, "%s", line);
}

static void *conn_main(void *arg) {
    conn_t *c = arg;
    for (;;) {
        /* the head of a request */
        char *end = NULL;
        while (!(end = c->have >= 4 ? strstr(c->buf, "\r\n\r\n") : NULL)) {
            if (c->have >= sizeof c->buf - 1) goto done;
            long n = recv(c->s, c->buf + c->have, (int)(sizeof c->buf - 1 - c->have), 0);
            if (n <= 0) goto done;
            c->have += (size_t)n;
            c->buf[c->have] = 0;
        }
        *end = 0;
        size_t used = (size_t)(end + 4 - c->buf);
        req_t r = { 0 };
        char *line = c->buf, *nl = strstr(line, "\r\n");
        if (nl) *nl = 0;
        char target[4096] = "";
        if (sscanf(line, "%7s %4095s", r.method, target) != 2) goto done;
        char *q = strchr(target, '?');
        if (q) { *q = 0; snprintf(r.query, sizeof r.query, "%s", q + 1); }
        snprintf(r.path, sizeof r.path, "%s", target);
        while (nl) {
            line = nl + 2;
            nl = strstr(line, "\r\n");
            if (nl) *nl = 0;
            char cl[32] = "";
            header_value(line, "Host", r.host, sizeof r.host);
            header_value(line, "Range", r.range, sizeof r.range);
            header_value(line, "If-Modified-Since", r.ims, sizeof r.ims);
            header_value(line, "X-Prism", r.xprism, sizeof r.xprism);
            header_value(line, "Connection", r.conn, sizeof r.conn);
            header_value(line, "Content-Length", cl, sizeof cl);
            if (cl[0]) r.clen = atoll(cl);
        }
        /* its body: the calls' are small */
        if (r.clen < 0 || r.clen > 65536) goto done;
        char *body = malloc((size_t)r.clen + 1);
        if (!body) goto done;
        size_t got = c->have - used < (size_t)r.clen ? c->have - used : (size_t)r.clen;
        memcpy(body, c->buf + used, got);
        used += got;
        while (got < (size_t)r.clen) {
            long n = recv(c->s, body + got, (int)((size_t)r.clen - got), 0);
            if (n <= 0) { free(body); goto done; }
            got += (size_t)n;
        }
        body[r.clen] = 0;
        memmove(c->buf, c->buf + used, c->have - used);
        c->have -= used;
        c->buf[c->have] = 0;
        int rc = handle(c, &r, body);
        free(body);
        if (rc || !strcasecmp(r.conn, "close")) break;
    }
done:
    close_sock(c->s);
    free(c);
    return NULL;
}

/* -------- start -------- */

static void default_dirs(void) {
#ifdef _WIN32
    const char *app = getenv("APPDATA"), *local = getenv("LOCALAPPDATA");
    if (!mc_dir[0] && app) snprintf(mc_dir, sizeof mc_dir, "%s\\.minecraft", app);
    if (!data_dir[0] && local) snprintf(data_dir, sizeof data_dir, "%s\\Prism", local);
#else
    const char *home = getenv("HOME");
    if (!home) home = ".";
    if (!mc_dir[0]) {
#ifdef __APPLE__
        snprintf(mc_dir, sizeof mc_dir, "%s/Library/Application Support/minecraft", home);
#else
        snprintf(mc_dir, sizeof mc_dir, "%s/.minecraft", home);
#endif
    }
    if (!data_dir[0]) {
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg) snprintf(data_dir, sizeof data_dir, "%s/prism", xdg);
        else snprintf(data_dir, sizeof data_dir, "%s/.local/share/prism", home);
    }
#endif
    if (!data_dir[0]) snprintf(data_dir, sizeof data_dir, "prism-data");
}

int cmd_serve(int argc, char **argv) {
    int want_port = 8820, open_page = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) want_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mc") && i + 1 < argc) snprintf(mc_dir, sizeof mc_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) snprintf(data_dir, sizeof data_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--www") && i + 1 < argc) snprintf(www_dir, sizeof www_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-open")) open_page = 0;
        else {
            fprintf(stderr, "usage: prism serve [--port n] [--mc dir] [--data dir] [--no-open] [--www dir]\n");
            return 2;
        }
    }
    default_dirs();
    snprintf(maps_dir, sizeof maps_dir, "%s/maps", data_dir);
    if (plat_mkdir(data_dir) || plat_mkdir(maps_dir)) { fprintf(stderr, "can't make %s\n", maps_dir); return 1; }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { fprintf(stderr, "no sockets\n"); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    sock_t ls = INVALID_SOCKET;
    for (int p = want_port; p < want_port + 20; p++) {
        ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == INVALID_SOCKET) break;
#ifndef _WIN32
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)p);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 64) == 0) { port = p; break; }
        close_sock(ls);
        ls = INVALID_SOCKET;
    }
    if (ls == INVALID_SOCKET) { fprintf(stderr, "no free port from %d\n", want_port); return 1; }

    char url[64];
    snprintf(url, sizeof url, "http://localhost:%d/", port);
    plat_console_utf8();
    printf("prism is running at %s\n"
           "  Minecraft: %s\n"
           "  maps:      %s\n"
           "Close this window (or press Ctrl+C) to stop it.\n", url, mc_dir, maps_dir);
    fflush(stdout);
    if (open_page) plat_open(url);
    for (;;) {
        sock_t s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET) continue;
        conn_t *c = calloc(1, sizeof *c);
        if (!c) { close_sock(s); continue; }
        c->s = s;
        pthread_t th;
        if (pthread_create(&th, NULL, conn_main, c)) { close_sock(s); free(c); continue; }
        pthread_detach(th);
    }
}
