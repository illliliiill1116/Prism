/* A single region from prism's region.glb (prism <jar> <region dir> <rx> <rz>
 * <out dir> [threads] glb), lit as the map's tiles are.
 */
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";

export function setupGlb(app) {
    const { MSAA, depthMat, draw, info, layerOf, patchCutout, patchLight, patchWater, scene, setFx, shadowMeshes,
            shadowOn, t0 } = app;
    const loadGlb = url => new GLTFLoader().load(url, gltf => {
        if (app.dead) return;
        scene.add(gltf.scene);
        let tris = 0;
        gltf.scene.traverse(o => {
            if (!o.isMesh) return;
            layerOf(o);
            tris += o.geometry.index.count / 3;
            const m = o.material, g = o.geometry;
            for (const a of ["sun", "sunwin", "blk", "bnc", "bamp"])
                if (g.getAttribute("_" + a)) {
                    g.setAttribute(a, g.getAttribute("_" + a));
                    g.deleteAttribute("_" + a);
                }
            /* the bounce packed as the tiles have it (packBounce) */
            const bnc = g.getAttribute("bnc"), bamp = g.getAttribute("bamp");
            if (bnc && bamp) {
                const bb = new Float32Array(bnc.count), q = x => Math.round(x * 255);
                for (let i = 0; i < bnc.count; i++)
                    bb[i] = q(bamp.getX(i)) | q(bnc.getX(i)) << 5 | q(bnc.getY(i)) << 8 | q(bnc.getZ(i)) << 11;
                g.setAttribute("bb", new THREE.BufferAttribute(bb, 1));
                g.deleteAttribute("bnc");
                g.deleteAttribute("bamp");
            }
            shadowMeshes.set(o, depthMat(m));
            if (m.userData.__patched) return;
            m.userData.__patched = true;
            const sunlit = !!g.getAttribute("sunwin");
            if (m.name === "WATER") patchWater(m); else patchLight(m, shadowOn && sunlit, sunlit);
            if (m.name === "MASK") m.side = THREE.FrontSide; /* as regionMaterials says */
            if (m.name === "MASK" && MSAA) patchCutout(m, sunlit);
            m.needsUpdate = true;
        });
        info.textContent = `${(tris / 1e6).toFixed(2)} M triangles, loaded in ${((performance.now() - t0) / 1000).toFixed(1)} s`;
        draw();
        requestAnimationFrame(() => { draw(); app.ready = true; setFx(app.fx); });
    }, undefined, e => { info.textContent = "load failed: " + e; info.classList.add("on"); app.ready = "error: " + e; });

    Object.assign(app, { loadGlb });
}
