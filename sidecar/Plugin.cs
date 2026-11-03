// CardBridge sidecar: a BepInEx plugin for Koikatsu that loads character cards in the real game and writes out what the game
// actually built (skinned vertex positions, bones, materials with their colours, floats and textures, a screenshot), so
// CardBridge can be compared against the game numerically. Development tool only; not shipped.
//
// Request: write <io>\request.txt = first line the output folder, then one card path per line. The plugin picks it up while the
// game sits at the title screen, writes <out>\<card name>\..., then <out>\done.txt, and quits the game.
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Text;
using BepInEx;
using Manager;
using UnityEngine;

[BepInPlugin("cardbridge.sidecar", "CardBridgeSidecar", "0.1")]
[BepInProcess("Koikatu")]
public class CardBridgeSidecar : BaseUnityPlugin
{
    static readonly string[] TexProps = {
        "_MainTex", "_ColorMask", "_DetailMask", "_NormalMap", "_NormalMapDetail", "_NormalMask", "_Texture2", "_Texture3", "_Texture4",
        "_Texture5", "_Texture6", "_Texture7", "_overtex1", "_overtex2", "_overtex3", "_liquidmask", "_AlphaMask", "_LineMask",
        "_HairGloss", "_AnotherRamp", "_BumpMap", "_PatternMask1", "_PatternMask2", "_PatternMask3", "_Texture", "_SpecularMask" };
    static readonly string[] ColorProps = {
        "_Color", "_Color2", "_Color3", "_Color4", "_Color5", "_Color6", "_Color7", "_ShadowColor", "_LineColor", "_SpecularColor",
        "_overcolor1", "_overcolor2", "_overcolor3", "_EmissionColor", "_ambientshadow", "_ambientshadowG", "_Color1_2", "_Color2_2" };
    static readonly string[] FloatProps = {
        "_SpecularPower", "_SpecularHeight", "_Cutoff", "_Blend", "_LineWidthS", "_LineWidth", "_Metallic", "_Smoothness", "_nipsize",
        "_nip_specular", "_DetailNormalMapScale", "_NormalMapScale", "_rimpower", "_rimV", "_ShadowExtend", "_UseMaskAlpha" };
    static readonly string[] VecProps = { "_grad", "_paint1", "_paint2", "_hokuro", "_MainTex_ST", "_Color_ST", "_overtex1_ST", "_overtex2_ST", "_overtex3_ST", "_Texture2_ST", "_Texture3_ST" };

    string io, outDir;
    bool running;
    // io/maker.flag: leave the Title scene for the character maker (CustomScene) first, so the dump and the screenshots see the maker's
    // own light, ambient and camera effects instead of the Title scene's empty one.
    bool makerMode, makerRequested;
    float makerAt;
    int frame;
    readonly Dictionary<int, string> texFiles = new Dictionary<int, string>();
    StringBuilder log = new StringBuilder();

    void Awake()
    {
        io = Environment.GetEnvironmentVariable("CARDBRIDGE_SIDECAR_DIR");
        if (string.IsNullOrEmpty(io)) io = @"D:\Project K\cardbridge\sidecar\io";
        Directory.CreateDirectory(io);
        Logger.LogInfo("CardBridgeSidecar watching " + io);
    }

    void Update()
    {
        if (running || ++frame % 60 != 0) return;
        if (File.Exists(Path.Combine(io, "maker.flag"))) {
            makerMode = true;
            if (!makerRequested) {
                if (!Singleton<Character>.IsInstance() || Singleton<Character>.Instance.chaListCtrl == null) return;
                if (UnityEngine.SceneManagement.SceneManager.GetActiveScene().name != "Title") return;
                makerRequested = true;
                makerAt = Time.realtimeSinceStartup;
                Manager.Scene.Data sd = new Manager.Scene.Data();
                sd.levelName = "CustomScene";
                sd.isAdd = false;
                sd.isFade = false;
                sd.isAsync = true;
                Singleton<Manager.Scene>.Instance.LoadReserve(sd, false);
                Logger.LogInfo("CardBridgeSidecar: loading CustomScene");
                return;
            }
            // wait for the maker to settle before dumping
            if (UnityEngine.SceneManagement.SceneManager.GetActiveScene().name != "CustomScene" || Time.realtimeSinceStartup - makerAt < 25f) return;
        }
        string req = Path.Combine(io, "request.txt");
        if (!File.Exists(req)) return;
        if (!Singleton<Character>.IsInstance() || Singleton<Character>.Instance.chaListCtrl == null) return;
        string work = Path.Combine(io, "request.working");
        File.Copy(req, work, true);
        File.Delete(req);
        running = true;
        StartCoroutine(Run(File.ReadAllLines(work)));
    }

    IEnumerator Run(string[] lines)
    {
        outDir = lines[0].Trim();
        Directory.CreateDirectory(outDir);
        for (int i = 1; i < lines.Length; ++i) {
            string card = lines[i].Trim();
            if (card.Length == 0) continue;
            for (int attempt = 0; attempt < 3; ++attempt) {
                IEnumerator it = DoCard(card);
                bool failed = false;
                while (true) {
                    bool more;
                    try { more = it.MoveNext(); }
                    catch (Exception e) {
                        failed = true;
                        string msg = e.GetType().FullName + ": " + e.Message + "\n" + e.StackTrace;
                        Logger.LogError("card " + card + " attempt " + attempt + ": " + msg);
                        File.WriteAllText(Path.Combine(outDir, "error_" + i + "_" + attempt + ".txt"), msg);
                        break;
                    }
                    if (!more) break;
                    yield return it.Current;
                }
                if (!failed) break;
                yield return new WaitForSeconds(3f);
            }
        }
        File.WriteAllText(Path.Combine(outDir, "done.txt"), "done " + DateTime.Now);
        yield return new WaitForSeconds(1f);
        System.Diagnostics.Process.GetCurrentProcess().Kill();
    }

    IEnumerator DoCard(string cardPath)
    {
        string name = Path.GetFileNameWithoutExtension(cardPath);
        string dir = Path.Combine(outDir, name);
        Directory.CreateDirectory(dir);
        texFiles.Clear();
        if (makerMode) foreach (ChaControl other in UnityEngine.Object.FindObjectsOfType<ChaControl>()) other.gameObject.SetActive(false);   // the maker's own character
        ChaFileControl cf = new ChaFileControl();
        if (!cf.LoadCharaFile(cardPath, 1, false, false)) { File.WriteAllText(Path.Combine(dir, "error.txt"), "LoadCharaFile failed"); yield break; }
        ChaControl chara = Singleton<Character>.Instance.CreateFemale(null, 0, cf);
        Singleton<Character>.Instance.loading.Load(0, chara, false);
        float t0 = Time.realtimeSinceStartup;
        while (!Singleton<Character>.Instance.loading.IsEnd(chara)) {
            if (Time.realtimeSinceStartup - t0 > 120f) { File.WriteAllText(Path.Combine(dir, "error.txt"), "load timeout"); yield break; }
            yield return null;
        }
        chara.transform.position = Vector3.zero;
        chara.transform.rotation = Quaternion.identity;
        // The game applies shape sliders and blend shapes from its own per-frame updates; drive them explicitly so the dump holds
        // the finished character even if this scene does not run them.
        chara.UpdateShapeBodyValueFromCustomInfo();
        chara.UpdateShapeFaceValueFromCustomInfo();
        for (int f = 0; f < 10; ++f) {
            // each step on its own: mods patch UpdateVisible and it can throw before the shape updates run
            try { chara.UpdateForce(); } catch (Exception) { }
            try { chara.UpdateShapeBody(); } catch (Exception) { }
            try { chara.UpdateShapeFace(); } catch (Exception) { }
            try { chara.UpdateAlwaysShapeBody(); } catch (Exception) { }
            yield return null;
        }
        yield return new WaitForEndOfFrame();

        Dump(chara, dir);
        try { DumpEnvironment(dir); } catch (Exception e) { File.WriteAllText(Path.Combine(dir, "environment_error.txt"), e.ToString()); }
        yield return StartCoroutine(Screenshot(chara, dir));
        File.WriteAllText(Path.Combine(dir, "ok.txt"), "ok");
        Singleton<Character>.Instance.DeleteChara(chara, false);
    }

    // The lighting the game renders this character under: colour space, ambient, every active light, the camera effects and the
    // shader globals the character code sets. CardBridge's viewer needs these to match the game's look (environment.json).
    void DumpEnvironment(string dir)
    {
        StringBuilder sb = new StringBuilder();
        sb.Append("{\"colorSpace\":" + J(QualitySettings.activeColorSpace.ToString()));
        sb.Append(",\"ambientMode\":" + J(RenderSettings.ambientMode.ToString()));
        Color al = RenderSettings.ambientLight;
        sb.Append(",\"ambientLight\":[" + F(al.r) + "," + F(al.g) + "," + F(al.b) + "],\"ambientIntensity\":" + F(RenderSettings.ambientIntensity));
        Color sk = RenderSettings.ambientSkyColor, eq = RenderSettings.ambientEquatorColor, gr = RenderSettings.ambientGroundColor;
        sb.Append(",\"ambientSky\":[" + F(sk.r) + "," + F(sk.g) + "," + F(sk.b) + "],\"ambientEquator\":[" + F(eq.r) + "," + F(eq.g) + "," + F(eq.b) + "],\"ambientGround\":[" + F(gr.r) + "," + F(gr.g) + "," + F(gr.b) + "]");
        sb.Append(",\"scene\":" + J(UnityEngine.SceneManagement.SceneManager.GetActiveScene().name));
        sb.Append(",\"lights\":[");
        bool first = true;
        foreach (Light l in UnityEngine.Object.FindObjectsOfType<Light>()) {
            if (!first) sb.Append(","); first = false;
            Color c = l.color;
            sb.Append("{\"name\":" + J(l.name) + ",\"enabled\":" + (l.enabled && l.gameObject.activeInHierarchy ? "true" : "false") + ",\"type\":" + J(l.type.ToString()) +
                      ",\"color\":[" + F(c.r) + "," + F(c.g) + "," + F(c.b) + "],\"intensity\":" + F(l.intensity) + ",\"range\":" + F(l.range) + ",\"shadows\":" + J(l.shadows.ToString()) +
                      ",\"cullingMask\":" + l.cullingMask + ",\"position\":" + V3(l.transform.position) + ",\"forward\":" + V3(l.transform.forward) + "}");
        }
        sb.Append("],\"cameras\":[");
        first = true;
        foreach (Camera cam in Camera.allCameras) {
            if (!first) sb.Append(","); first = false;
            sb.Append("{\"name\":" + J(cam.name) + ",\"hdr\":" + (cam.allowHDR ? "true" : "false") + ",\"renderingPath\":" + J(cam.actualRenderingPath.ToString()) + ",\"components\":[");
            bool f2 = true;
            foreach (Component comp in cam.GetComponents<Component>()) {
                if (comp == null) continue;
                if (!f2) sb.Append(","); f2 = false;
                sb.Append(J(comp.GetType().FullName));
            }
            sb.Append("]}");
        }
        sb.Append("],\"globals\":{");
        string[] vecs = { "_ambientshadowG", "_LineColorG", "_linewidthG", "_FaceShadowG", "_FaceNormalG", "_RampG_ST" };
        for (int i = 0; i < vecs.Length; ++i) {
            Vector4 v = Shader.GetGlobalVector(vecs[i]);
            sb.Append((i > 0 ? "," : "") + J(vecs[i]) + ":[" + F(v.x) + "," + F(v.y) + "," + F(v.z) + "," + F(v.w) + "]");
        }
        sb.Append("}}");
        File.WriteAllText(Path.Combine(dir, "environment.json"), sb.ToString());
        if (Camera.main != null) DumpEffects(Camera.main, dir);
    }

    // Every public field and property of the main camera's image effects (colour grading, bloom...), textures saved next to it.
    void DumpEffects(Camera cam, string dir)
    {
        StringBuilder sb = new StringBuilder("{");
        bool firstComp = true;
        foreach (MonoBehaviour mb in cam.GetComponents<MonoBehaviour>()) {
            if (mb == null) continue;
            string tn = mb.GetType().FullName;
            if (!(tn.Contains("Amplify") || tn.Contains("Bloom") || tn.Contains("Blur") || tn.Contains("Sepia") || tn.Contains("Effect") || tn.Contains("CustomRender"))) continue;
            if (!firstComp) sb.Append(","); firstComp = false;
            sb.Append(J(tn) + ":{\"enabled\":" + (mb.enabled ? "true" : "false"));
            foreach (System.Reflection.FieldInfo f in mb.GetType().GetFields(System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)) {
                object v = null;
                try { v = f.GetValue(mb); } catch (Exception) { continue; }
                string val;
                if (v == null) val = "null";
                else if (v is Texture) {
                    Texture t = (Texture)v;
                    string file = "";
                    try { file = SaveTexture(t, dir); } catch (Exception) { }
                    val = "{\"texture\":" + J(t.name) + ",\"w\":" + t.width + ",\"h\":" + t.height + ",\"file\":" + J(file) + "}";
                }
                else if (v is float) val = F((float)v);
                else if (v is int || v is bool) val = v.ToString().ToLower();
                else if (v is Color) { Color c = (Color)v; val = "[" + F(c.r) + "," + F(c.g) + "," + F(c.b) + "," + F(c.a) + "]"; }
                else if (v is Vector4) { Vector4 c = (Vector4)v; val = "[" + F(c.x) + "," + F(c.y) + "," + F(c.z) + "," + F(c.w) + "]"; }
                else if (v is Vector3) val = V3((Vector3)v);
                else if (v is Enum || v is string) val = J(v.ToString());
                else continue;
                sb.Append("," + J(f.Name) + ":" + val);
            }
            sb.Append("}");
        }
        sb.Append("}");
        File.WriteAllText(Path.Combine(dir, "effects.json"), sb.ToString());
    }

    // ---------------------------------------------------------------- dump
    static string J(string s) { return "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\""; }
    static string F(float f) { return f.ToString("R", System.Globalization.CultureInfo.InvariantCulture); }
    static string V3(Vector3 v) { return "[" + F(v.x) + "," + F(v.y) + "," + F(v.z) + "]"; }
    static string PathOf(Transform t, Transform root)
    {
        string p = t.name;
        for (Transform a = t.parent; a != null && a != root.parent; a = a.parent) p = a.name + "/" + p;
        return p;
    }

    void Dump(ChaControl chara, string dir)
    {
        Transform root = chara.transform;
        StringBuilder sb = new StringBuilder();
        // bones
        sb.Append("[");
        bool first = true;
        foreach (Transform t in root.GetComponentsInChildren<Transform>(true)) {
            if (!first) sb.Append(",\n");
            first = false;
            Quaternion q = t.rotation;
            sb.Append("{\"path\":" + J(PathOf(t, root)) + ",\"name\":" + J(t.name) + ",\"active\":" + (t.gameObject.activeInHierarchy ? "true" : "false") +
                      ",\"pos\":" + V3(t.position) + ",\"rot\":[" + F(q.x) + "," + F(q.y) + "," + F(q.z) + "," + F(q.w) + "],\"scale\":" + V3(t.lossyScale) + ",\"localScale\":" + V3(t.localScale) + "}");
        }
        sb.Append("]");
        File.WriteAllText(Path.Combine(dir, "bones.json"), sb.ToString());

        // renderers
        sb = new StringBuilder("[");
        first = true;
        int idx = 0;
        foreach (Renderer r in root.GetComponentsInChildren<Renderer>(true)) {
            SkinnedMeshRenderer smr = r as SkinnedMeshRenderer;
            MeshRenderer mr = r as MeshRenderer;
            Mesh mesh = smr != null ? smr.sharedMesh : (mr != null ? mr.GetComponent<MeshFilter>().sharedMesh : null);
            if (!first) sb.Append(",\n");
            first = false;
            sb.Append("{\"index\":" + idx + ",\"path\":" + J(PathOf(r.transform, root)) + ",\"name\":" + J(r.name) +
                      ",\"skinned\":" + (smr != null ? "true" : "false") + ",\"enabled\":" + (r.enabled ? "true" : "false") +
                      ",\"active\":" + (r.gameObject.activeInHierarchy ? "true" : "false") + ",\"mesh\":" + J(mesh != null ? mesh.name : "") +
                      ",\"vertices\":" + (mesh != null ? mesh.vertexCount : 0) + ",\"readable\":" + (mesh != null && mesh.isReadable ? "true" : "false"));
            Bounds b = r.bounds;
            sb.Append(",\"boundsCenter\":" + V3(b.center) + ",\"boundsSize\":" + V3(b.size));
            if (mesh != null && r.enabled && r.gameObject.activeInHierarchy && mesh.isReadable) {
                try { WriteMesh(r, smr, mesh, Path.Combine(dir, "mesh_" + idx + ".bin")); sb.Append(",\"meshFile\":\"mesh_" + idx + ".bin\""); }
                catch (Exception e) { sb.Append(",\"meshError\":" + J(e.Message)); }
            }
            sb.Append(",\"materials\":[");
            Material[] mats = r.sharedMaterials;
            for (int m = 0; m < mats.Length; ++m) {
                if (m > 0) sb.Append(",");
                sb.Append(MaterialJson(r.materials != null && m < r.materials.Length ? r.materials[m] : mats[m], dir));
            }
            sb.Append("]}");
            ++idx;
        }
        sb.Append("]");
        File.WriteAllText(Path.Combine(dir, "renderers.json"), sb.ToString());
    }

    // Float32: vertex count, then x y z per vertex in world space. Skinned meshes are skinned by hand (bone world matrix * bind pose
    // weighted by the four bone weights) and also baked by Unity (local space, to compare), which includes blend shapes.
    void WriteMesh(Renderer r, SkinnedMeshRenderer smr, Mesh mesh, string file)
    {
        Vector3[] v = mesh.vertices;
        Vector3[] world = new Vector3[v.Length];
        if (smr != null && mesh.boneWeights.Length == v.Length && smr.bones.Length > 0) {
            BoneWeight[] bw = mesh.boneWeights;
            Matrix4x4[] bind = mesh.bindposes;
            Transform[] bones = smr.bones;
            Matrix4x4[] m = new Matrix4x4[bones.Length];
            for (int i = 0; i < bones.Length; ++i) m[i] = bones[i] != null ? bones[i].localToWorldMatrix * bind[i] : Matrix4x4.identity;
            for (int i = 0; i < v.Length; ++i) {
                Vector3 p = Vector3.zero;
                BoneWeight w = bw[i];
                if (w.weight0 > 0) p += m[w.boneIndex0].MultiplyPoint3x4(v[i]) * w.weight0;
                if (w.weight1 > 0) p += m[w.boneIndex1].MultiplyPoint3x4(v[i]) * w.weight1;
                if (w.weight2 > 0) p += m[w.boneIndex2].MultiplyPoint3x4(v[i]) * w.weight2;
                if (w.weight3 > 0) p += m[w.boneIndex3].MultiplyPoint3x4(v[i]) * w.weight3;
                world[i] = p;
            }
        } else {
            Matrix4x4 l2w = r.transform.localToWorldMatrix;
            for (int i = 0; i < v.Length; ++i) world[i] = l2w.MultiplyPoint3x4(v[i]);
        }
        Vector3[] baked = null;
        if (smr != null) {
            Mesh bm = new Mesh();
            smr.BakeMesh(bm);
            baked = bm.vertices;
            Matrix4x4 l2w = r.transform.localToWorldMatrix;
            for (int i = 0; i < baked.Length; ++i) baked[i] = l2w.MultiplyPoint3x4(baked[i]);
            UnityEngine.Object.Destroy(bm);
        }
        using (BinaryWriter w = new BinaryWriter(File.Create(file))) {
            w.Write(v.Length);
            for (int i = 0; i < v.Length; ++i) { w.Write(world[i].x); w.Write(world[i].y); w.Write(world[i].z); }
            w.Write(baked != null ? baked.Length : 0);
            if (baked != null) for (int i = 0; i < baked.Length; ++i) { w.Write(baked[i].x); w.Write(baked[i].y); w.Write(baked[i].z); }
        }
    }

    string MaterialJson(Material m, string dir)
    {
        if (m == null) return "null";
        StringBuilder sb = new StringBuilder("{\"name\":" + J(m.name) + ",\"shader\":" + J(m.shader != null ? m.shader.name : "") + ",\"renderQueue\":" + m.renderQueue);
        sb.Append(",\"colors\":{");
        bool first = true;
        foreach (string p in ColorProps) {
            if (!m.HasProperty(p)) continue;
            Color c = m.GetColor(p);
            if (!first) sb.Append(",");
            first = false;
            sb.Append(J(p) + ":[" + F(c.r) + "," + F(c.g) + "," + F(c.b) + "," + F(c.a) + "]");
        }
        sb.Append("},\"floats\":{");
        first = true;
        foreach (string p in FloatProps) {
            if (!m.HasProperty(p)) continue;
            if (!first) sb.Append(",");
            first = false;
            sb.Append(J(p) + ":" + F(m.GetFloat(p)));
        }
        sb.Append("},\"vectors\":{");
        first = true;
        foreach (string p in VecProps) {
            if (!m.HasProperty(p)) continue;
            Vector4 c = m.GetVector(p);
            if (!first) sb.Append(",");
            first = false;
            sb.Append(J(p) + ":[" + F(c.x) + "," + F(c.y) + "," + F(c.z) + "," + F(c.w) + "]");
        }
        sb.Append("},\"textures\":{");
        first = true;
        foreach (string p in TexProps) {
            if (!m.HasProperty(p)) continue;
            Texture t = m.GetTexture(p);
            if (!first) sb.Append(",");
            first = false;
            sb.Append(J(p) + ":" + (t == null ? "null" : "{\"name\":" + J(t.name) + ",\"width\":" + t.width + ",\"height\":" + t.height + ",\"file\":" + J(SaveTexture(t, dir)) + "}"));
        }
        sb.Append("}}");
        return sb.ToString();
    }

    // Any texture (readable or not, render textures too) copied through a render texture and written as PNG, at most 1024 px.
    string SaveTexture(Texture t, string dir)
    {
        int id = t.GetInstanceID();
        string cached;
        if (texFiles.TryGetValue(id, out cached)) return cached;
        string name = "tex_" + texFiles.Count + ".png";
        texFiles[id] = name;
        try {
            int w = Mathf.Min(t.width, 1024), h = Mathf.Min(t.height, 1024);
            if (w <= 0 || h <= 0) return "";
            RenderTexture rt = RenderTexture.GetTemporary(w, h, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
            Graphics.Blit(t, rt);
            RenderTexture prev = RenderTexture.active;
            RenderTexture.active = rt;
            Texture2D tmp = new Texture2D(w, h, TextureFormat.ARGB32, false, true);
            tmp.ReadPixels(new Rect(0, 0, w, h), 0, 0);
            tmp.Apply();
            RenderTexture.active = prev;
            RenderTexture.ReleaseTemporary(rt);
            File.WriteAllBytes(Path.Combine(dir, name), tmp.EncodeToPNG());
            UnityEngine.Object.Destroy(tmp);
        } catch (Exception e) { Logger.LogWarning("texture " + t.name + ": " + e.Message); }
        return name;
    }

    // ---------------------------------------------------------------- screenshot
    IEnumerator Screenshot(ChaControl chara, string dir)
    {
        Bounds b = new Bounds(chara.transform.position, Vector3.zero);
        bool any = false;
        foreach (Renderer r in chara.GetComponentsInChildren<Renderer>()) {
            if (!r.enabled || !r.gameObject.activeInHierarchy) continue;
            if (!any) { b = r.bounds; any = true; } else b.Encapsulate(r.bounds);
        }
        GameObject go;
        Camera cam;
        Camera main = makerMode ? Camera.main : null;
        List<MonoBehaviour> effects = new List<MonoBehaviour>();
        if (main != null) {
            // the maker's own camera with its image effects; only its follow/control scripts are switched off
            go = UnityEngine.Object.Instantiate(main.gameObject);
            go.name = "cbCam";
            cam = go.GetComponent<Camera>();
            foreach (MonoBehaviour mb in go.GetComponentsInChildren<MonoBehaviour>(true)) {
                string tn = mb.GetType().FullName;
                bool effect = tn.Contains("Effect") || tn.Contains("Bloom") || tn.Contains("Color") || tn.Contains("Antialias") || tn.Contains("Occlusion") ||
                              tn.Contains("Vignette") || tn.Contains("Blur") || tn.Contains("Tonemap") || tn.Contains("DepthOfField") || tn.Contains("Sun") || tn.Contains("Fog");
                if (!effect) mb.enabled = false;
                else effects.Add(mb);
            }
            foreach (AudioListener al in go.GetComponentsInChildren<AudioListener>(true)) UnityEngine.Object.Destroy(al);
        } else {
            go = new GameObject("cbCam");
            cam = go.AddComponent<Camera>();
        }
        cam.orthographic = true;
        cam.clearFlags = CameraClearFlags.SolidColor;
        cam.backgroundColor = new Color(226f / 255f, 228f / 255f, 232f / 255f);
        cam.cullingMask = ~0;
        cam.orthographicSize = Mathf.Max(b.size.y * 0.52f, 0.1f);
        float cx = b.center.x, cy = b.center.y;
        for (int view = 0; view < 3; ++view) {
            float yaw = view * 90f; // front, left, back
            Quaternion rot = Quaternion.Euler(0, 180f + yaw, 0);
            go.transform.rotation = rot;
            go.transform.position = new Vector3(cx, cy, b.center.z) - rot * Vector3.forward * 10f;
            cam.nearClipPlane = 0.1f; cam.farClipPlane = 30f;
            RenderTexture rt = RenderTexture.GetTemporary(640, 800, 24, RenderTextureFormat.ARGB32);
            cam.targetTexture = rt;
            cam.Render();
            RenderTexture prev = RenderTexture.active;
            RenderTexture.active = rt;
            Texture2D tex = new Texture2D(640, 800, TextureFormat.RGB24, false);
            tex.ReadPixels(new Rect(0, 0, 640, 800), 0, 0);
            tex.Apply();
            if (view == 0 && effects.Count > 0) {   // the same view with the image effects (colour grading, bloom) off
                foreach (MonoBehaviour mb in effects) mb.enabled = false;
                cam.Render();
                RenderTexture.active = rt;
                Texture2D tex2 = new Texture2D(640, 800, TextureFormat.RGB24, false);
                tex2.ReadPixels(new Rect(0, 0, 640, 800), 0, 0);
                tex2.Apply();
                File.WriteAllBytes(Path.Combine(dir, "shot_noeffects_0.png"), tex2.EncodeToPNG());
                UnityEngine.Object.Destroy(tex2);
                foreach (MonoBehaviour mb in effects) mb.enabled = true;
            }
            RenderTexture.active = prev;
            cam.targetTexture = null;
            RenderTexture.ReleaseTemporary(rt);
            File.WriteAllBytes(Path.Combine(dir, "shot_" + view + ".png"), tex.EncodeToPNG());
            UnityEngine.Object.Destroy(tex);
            yield return null;
        }
        if (effects.Count > 0) ColourCube(cam, effects, dir);
        UnityEngine.Object.Destroy(go);
    }

    // What the camera's image effects do to every colour: an N^3 grid of flat colours rendered through the camera with the effects on
    // and off (cube_on.bin / cube_off.bin, N*N*N RGB bytes, red fastest), so the viewer can apply the same grading.
    void ColourCube(Camera cam, List<MonoBehaviour> effects, string dir)
    {
        const int N = 17;
        cam.cullingMask = 0;
        cam.clearFlags = CameraClearFlags.SolidColor;
        RenderTexture rt = RenderTexture.GetTemporary(8, 8, 24, RenderTextureFormat.ARGB32);
        cam.targetTexture = rt;
        Texture2D tex = new Texture2D(8, 8, TextureFormat.RGB24, false);
        for (int pass = 0; pass < 2; ++pass) {
            foreach (MonoBehaviour mb in effects) mb.enabled = pass == 0 && mb.GetType().FullName.Contains("Amplify");
            byte[] data = new byte[N * N * N * 3];
            int at = 0;
            for (int b = 0; b < N; ++b)
                for (int g = 0; g < N; ++g)
                    for (int r = 0; r < N; ++r) {
                        cam.backgroundColor = new Color(r / (float)(N - 1), g / (float)(N - 1), b / (float)(N - 1));
                        cam.Render();
                        RenderTexture.active = rt;
                        tex.ReadPixels(new Rect(0, 0, 8, 8), 0, 0);
                        Color c = tex.GetPixel(4, 4);
                        data[at++] = (byte)Mathf.RoundToInt(c.r * 255f); data[at++] = (byte)Mathf.RoundToInt(c.g * 255f); data[at++] = (byte)Mathf.RoundToInt(c.b * 255f);
                    }
            File.WriteAllBytes(Path.Combine(dir, pass == 0 ? "cube_on.bin" : "cube_off.bin"), data);
        }
        RenderTexture.active = null;
        cam.targetTexture = null;
        RenderTexture.ReleaseTemporary(rt);
        UnityEngine.Object.Destroy(tex);
        foreach (MonoBehaviour mb in effects) mb.enabled = true;
    }
}
