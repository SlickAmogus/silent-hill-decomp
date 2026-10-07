using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// The converter UI flows — Model → OBJ export, OBJ → Model import (high-poly
    /// and simple), and the model viewer opener — hoisted out of ModManagerForm so
    /// the Mod Manager buttons and the Model Viewer's menu are two entry points
    /// over ONE implementation. Every method takes the dialog owner and the game
    /// root explicitly; nothing here holds state.
    /// </summary>
    internal static class ConverterActions
    {
        /// <summary>"Model → OBJ…": write a character model out as an editable
        /// .obj + .mtl + .ilmmeta.json set.</summary>
        public static void ExportModel(IWin32Window owner, string gameRoot)
        {
            string ilm;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = Loc.T("Select a character model (.ILM)");
                ofd.Filter = Loc.T("Model files") + " (*.ilm)|*.ilm|" + Loc.T("All files") + " (*.*)|*.*";
                string gamedata = Path.Combine(gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                ilm = ofd.FileName;
            }
            ExportModelFrom(owner, ilm);
        }

        /// <summary>Export with the source .ILM already chosen (the viewer's menu
        /// exports the model it is showing without re-asking for it).</summary>
        public static void ExportModelFrom(IWin32Window owner, string ilm)
        {
            string outObj;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = Loc.T("Save model as OBJ");
                sfd.Filter = "Wavefront OBJ (*.obj)|*.obj|" + Loc.T("All files") + " (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(ilm);
                sfd.FileName = Path.GetFileNameWithoutExtension(ilm) + ".obj";
                if (sfd.ShowDialog(owner) != DialogResult.OK) return;
                outObj = sfd.FileName;
            }

            IlmObjConverter.ExportResult res = null;
            try
            {
                ProgressDialog.Run(owner, Loc.T("Exporting model…"),
                    r => { res = IlmObjConverter.Export(ilm, outObj); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner, Loc.F("Export failed:\n\n{0}", ex.Message),
                    Loc.T("Model → OBJ"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (res == null || !string.IsNullOrEmpty(res.Error))
            {
                MessageBox.Show(owner, Loc.F("Export failed:\n\n{0}", res != null ? res.Error : Loc.T("unknown error")),
                    Loc.T("Model → OBJ"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            // Paint the real in-game texture onto the MTL so the mesh shows textured in
            // Blender: the raw exporter writes a colourless MTL because a character's
            // texture is a paletted CLUT composite, not a plain image. Import reads only
            // the OBJ + .ilmmeta.json and never the MTL, so a map_Kd here cannot affect
            // the round-trip. Silently skipped when no .TIM sits beside the .ILM.
            string texNote = TextureExportedObj(ilm, res.ObjPath, res.MtlPath);

            string msg = Loc.F("Wrote {0} body part(s), {1} vertices, {2} face(s) and {3} material(s):",
                               res.Parts, res.Vertices, res.Prims, res.Materials) + "\n" + res.ObjPath +
                "\n\n" + Loc.F("Beside it: {0} and {1} — keep the .ilmmeta.json, \"{2}\" needs it.",
                               Path.GetFileName(res.MtlPath), Path.GetFileName(res.MetaPath), Loc.T("OBJ → Model ▾")) +
                "\n\n" + Loc.T("In Blender, each object is one rigid animated body part: move and reshape " +
                               "vertices freely, but do not rename, add or remove objects.");
            if (texNote != null) msg += "\n\n" + texNote;
            if (res.Dangling > 0)
                msg += "\n\n" + Loc.F("Note: {0} face corner(s) point at a vertex outside their own part, " +
                       "so the OBJ substitutes that part's first vertex there — those few corners look wrong in " +
                       "Blender and are restored on import.", res.Dangling);
            // res.Warnings carries the exporter's rest-pose diagnosis: when res.AnmName is null it explains
            // that every part is in its own local space and will pile on the origin in Blender — a failure the
            // user cannot diagnose from the model itself, so it must not be swallowed by the success dialog.
            if (res.Warnings.Count > 0)
                msg += "\n\n" + Loc.F("Warnings ({0}):", res.Warnings.Count) + "\n - " +
                       string.Join("\n - ", res.Warnings.Take(8)) +
                       (res.Warnings.Count > 8 ? "\n - …" : "");
            bool warn = res.Dangling > 0 || res.Warnings.Count > 0;
            if (MessageBox.Show(owner, msg + "\n\n" + Loc.T("Open the output folder?"),
                    Loc.T("Model → OBJ"), MessageBoxButtons.YesNo,
                    warn ? MessageBoxIcon.Warning : MessageBoxIcon.Information) == DialogResult.Yes)
            {
                try { System.Diagnostics.Process.Start(Path.GetDirectoryName(res.ObjPath)); } catch { }
            }
        }

        /// <summary>"TMD → OBJ…": item models (ITEM/IT_00x banks, UNQ* close-ups,
        /// FOOK). Separate from ExportModel because a TMD is a different format with
        /// no bones — see TmdObjConverter for the conventions.</summary>
        public static void ExportTmd(IWin32Window owner, string gameRoot)
        {
            string tmd;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = Loc.T("Select an item model (.TMD)");
                ofd.Filter = Loc.T("PSX item models") + " (*.tmd)|*.tmd|" + Loc.T("All files") + " (*.*)|*.*";
                string items = Path.Combine(Path.Combine(gameRoot, "gamedata"), "ITEM");
                string gamedata = Path.Combine(gameRoot, "gamedata");
                if (Directory.Exists(items)) ofd.InitialDirectory = items;
                else if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                tmd = ofd.FileName;
            }
            ExportTmdFrom(owner, tmd);
        }

        /// <summary>Export with the .TMD already chosen (the viewer exports the model
        /// it is showing).</summary>
        public static void ExportTmdFrom(IWin32Window owner, string tmd)
        {
            string outObj;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = Loc.T("Save item model as OBJ");
                sfd.Filter = "Wavefront OBJ (*.obj)|*.obj|" + Loc.T("All files") + " (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(tmd);
                sfd.FileName = Path.GetFileNameWithoutExtension(tmd) + ".obj";
                if (sfd.ShowDialog(owner) != DialogResult.OK) return;
                outObj = sfd.FileName;
            }

            // VRAM page 14 hosts TIM01..06, one per map, so a model that samples it
            // is ambiguous from the TMD alone — but only THEN, and only when the file
            // is not one of the IT_00x banks, whose bank is fixed by the same
            // map-group switch that loads them. Ask in the one case that is really a
            // question; the other two are answered from the file itself.
            string tpage14 = null;
            {
                string terr;
                var probe = TmdFile.Load(tmd, out terr);
                if (probe == null)
                {
                    MessageBox.Show(owner, Loc.F("Could not read the model:\n\n{0}", terr), Loc.T("TMD → OBJ"),
                        MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                if (TmdViewSceneBuilder.UsesTpage14(probe))
                {
                    tpage14 = TmdViewSceneBuilder.StockTpage14ForFile(tmd);
                    if (tpage14 == null)
                    {
                        using (var pick = new TmdPageDialog())
                        {
                            if (pick.ShowDialog(owner) != DialogResult.OK) return;
                            tpage14 = pick.Selected;
                        }
                    }
                }
            }

            TmdObjConverter.ExportResult res = null;
            try
            {
                ProgressDialog.Run(owner, Loc.T("Exporting item model…"),
                    r => { res = TmdObjConverter.Export(tmd, outObj, tpage14); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner, Loc.F("Export failed:\n\n{0}", ex.Message), Loc.T("TMD → OBJ"),
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (res == null || !string.IsNullOrEmpty(res.Error))
            {
                MessageBox.Show(owner, Loc.F("Export failed:\n\n{0}", res != null ? res.Error : Loc.T("unknown error")),
                    Loc.T("TMD → OBJ"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string msg = Loc.F("Wrote {0} object(s), {1} vertices, {2} face(s) and {3} material(s):",
                               res.Objects, res.Vertices, res.Prims, res.Materials) + "\n" + res.ObjPath +
                "\n\n" + (res.Pages > 0
                    ? Loc.F("Beside it: {0} and {1} plus {2} texture page PNG(s).",
                            Path.GetFileName(res.MtlPath), Path.GetFileName(res.MetaPath), res.Pages)
                    : Loc.F("Beside it: {0} and {1}.", Path.GetFileName(res.MtlPath), Path.GetFileName(res.MetaPath)));
            if (res.Objects > 1)
                msg += "\n\n" + Loc.F("This is a BANK: its {0} items are separate objects that all sit " +
                       "on the origin (a TMD stores no placement), so they overlap in Blender. " +
                       "Hide all but the one you are editing.", res.Objects);
            msg += "\n\n" + Loc.F("Edit vertex positions freely, but do not add, remove or rename objects, " +
                   "and do not add, delete or triangulate faces — \"{0}\" patches the original " +
                   "file and needs the topology intact.", Loc.T("Item model (.TMD) — reshape…"));
            if (res.Warnings.Count > 0)
                msg += "\n\n" + Loc.F("Warnings ({0}):", res.Warnings.Count) + "\n - " +
                       string.Join("\n - ", res.Warnings.Take(8)) +
                       (res.Warnings.Count > 8 ? "\n - …" : "");
            if (MessageBox.Show(owner, msg + "\n\n" + Loc.T("Open the output folder?"), Loc.T("TMD → OBJ"),
                    MessageBoxButtons.YesNo,
                    res.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information) == DialogResult.Yes)
            { try { System.Diagnostics.Process.Start(Path.GetDirectoryName(res.ObjPath)); } catch { } }
        }

        /// <summary>"OBJ → TMD…": patch an edited OBJ back over the original item
        /// model. Same shape as SimpleImport — the original is the template and every
        /// non-geometry byte is carried over from it.</summary>
        public static void ImportTmd(IWin32Window owner, string gameRoot)
        {
            string obj;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = Loc.T("Select your edited item model (.obj)");
                ofd.Filter = "Wavefront OBJ (*.obj)|*.obj|" + Loc.T("All files") + " (*.*)|*.*";
                string gamedata = Path.Combine(gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                obj = ofd.FileName;
            }

            string tmd = GuessTmdFor(obj);
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = Loc.T("Select the ORIGINAL item model (.TMD) this OBJ came from");
                ofd.Filter = Loc.T("PSX item models") + " (*.tmd)|*.tmd|" + Loc.T("All files") + " (*.*)|*.*";
                if (tmd != null) { ofd.InitialDirectory = Path.GetDirectoryName(tmd); ofd.FileName = Path.GetFileName(tmd); }
                else ofd.InitialDirectory = Path.GetDirectoryName(obj);
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                tmd = ofd.FileName;
            }

            string outTmd;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = Loc.T("Save the patched item model");
                sfd.Filter = Loc.T("PSX item models") + " (*.tmd)|*.tmd|" + Loc.T("All files") + " (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(obj);
                sfd.FileName = Path.GetFileName(tmd);
                if (sfd.ShowDialog(owner) != DialogResult.OK) return;
                outTmd = sfd.FileName;
            }
            if (string.Equals(Path.GetFullPath(outTmd), Path.GetFullPath(tmd), StringComparison.OrdinalIgnoreCase))
            {
                MessageBox.Show(owner, Loc.T("Choose a different output file — the original is the template " +
                    "and overwriting it would leave you with nothing to import against next time."),
                    Loc.T("OBJ → TMD"), MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            TmdObjConverter.ImportResult res = null;
            try
            {
                ProgressDialog.Run(owner, Loc.T("Patching item model…"),
                    r => { res = TmdObjConverter.Import(obj, tmd, outTmd); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner, Loc.F("Import failed:\n\n{0}", ex.Message), Loc.T("OBJ → TMD"),
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (res == null || !string.IsNullOrEmpty(res.Error))
            {
                MessageBox.Show(owner, Loc.F("Import failed:\n\n{0}", res != null ? res.Error : Loc.T("unknown error")),
                    Loc.T("OBJ → TMD"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string msg = Loc.F("Patched {0} object(s): {1} vertices, {2} normals and {3} texture coordinate(s).",
                               res.Objects, res.Vertices, res.Normals, res.Uvs) + "\n" + res.OutPath +
                "\n\n" + Loc.F("Drop it into gamedata\\load\\ITEM\\ under the ORIGINAL name ({0}) and set allow_loose_files = 1.",
                               Path.GetFileName(tmd)) +
                "\n\n" + Loc.T("Keep the CLUT and tpage words as they were: a modded TMD has to reuse the " +
                               "stock VRAM pages, because the game decides what is uploaded there.");
            if (res.Warnings.Count > 0)
                msg += "\n\n" + Loc.F("Warnings ({0}):", res.Warnings.Count) + "\n - " +
                       string.Join("\n - ", res.Warnings.Take(10)) +
                       (res.Warnings.Count > 10 ? "\n - …" : "");
            if (MessageBox.Show(owner, msg + "\n\n" + Loc.T("Open the output folder?"), Loc.T("OBJ → TMD"),
                    MessageBoxButtons.YesNo,
                    res.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information) == DialogResult.Yes)
            { try { System.Diagnostics.Process.Start(Path.GetDirectoryName(res.OutPath)); } catch { } }
        }

        /// <summary>"Item model — replace…": rebuild the .TMD outright from the OBJ, so
        /// vertex and face counts are free. The original is still required — it supplies
        /// the VRAM page and palette each material binds to, which geometry cannot.</summary>
        public static void RebuildTmd(IWin32Window owner, string gameRoot)
        {
            string obj;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = Loc.T("Select your replacement model (.obj)");
                ofd.Filter = "Wavefront OBJ (*.obj)|*.obj|" + Loc.T("All files") + " (*.*)|*.*";
                string gamedata = Path.Combine(gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                obj = ofd.FileName;
            }

            string tmd = GuessTmdFor(obj);
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = Loc.T("Select the ORIGINAL item model (.TMD) to replace");
                ofd.Filter = Loc.T("PSX item models") + " (*.tmd)|*.tmd|" + Loc.T("All files") + " (*.*)|*.*";
                if (tmd != null) { ofd.InitialDirectory = Path.GetDirectoryName(tmd); ofd.FileName = Path.GetFileName(tmd); }
                else ofd.InitialDirectory = Path.GetDirectoryName(obj);
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                tmd = ofd.FileName;
            }

            string outTmd;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = Loc.T("Save the rebuilt item model");
                sfd.Filter = Loc.T("PSX item models") + " (*.tmd)|*.tmd|" + Loc.T("All files") + " (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(obj);
                sfd.FileName = Path.GetFileName(tmd);
                if (sfd.ShowDialog(owner) != DialogResult.OK) return;
                outTmd = sfd.FileName;
            }
            if (string.Equals(Path.GetFullPath(outTmd), Path.GetFullPath(tmd), StringComparison.OrdinalIgnoreCase))
            {
                MessageBox.Show(owner, Loc.T("Choose a different output file — the original supplies the texture " +
                    "bindings for every rebuild, so overwriting it would leave you without a template."),
                    Loc.T("OBJ → TMD (replace)"), MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            TmdObjConverter.RebuildResult res = null;
            try
            {
                ProgressDialog.Run(owner, Loc.T("Rebuilding item model…"),
                    r => { res = TmdObjConverter.Rebuild(obj, tmd, outTmd); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner, Loc.F("Rebuild failed:\n\n{0}", ex.Message), Loc.T("OBJ → TMD (replace)"),
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (res == null || !string.IsNullOrEmpty(res.Error))
            {
                MessageBox.Show(owner, Loc.F("Rebuild failed:\n\n{0}", res != null ? res.Error : Loc.T("unknown error")),
                    Loc.T("OBJ → TMD (replace)"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string msg = Loc.F("Rebuilt {0} object(s): {1} vertices, {2} normals, {3} face(s) ({4} tri, {5} quad; " +
                               "{6} textured, {7} flat), {8} bytes.",
                               res.Objects, res.Vertices, res.Normals, res.Prims, res.Tris, res.Quads,
                               res.Textured, res.Untextured, res.Bytes) + "\n" +
                res.OutPath +
                "\n\n" + Loc.F("Drop it into gamedata\\load\\ITEM\\ under the ORIGINAL name ({0}) and set allow_loose_files = 1.",
                               Path.GetFileName(tmd)) + " " +
                Loc.T("Oversized models load through the engine's big-TMD path, which accepts up to 8192 " +
                      "vertices and 8192 faces per object.") +
                "\n\n" + Loc.T("Textures still come from the stock VRAM pages: keep using the tpNN_clutNN material " +
                               "names the exporter writes, and keep UVs inside the 256x256 page.");
            if (res.Warnings.Count > 0)
                msg += "\n\n" + Loc.F("Warnings ({0}):", res.Warnings.Count) + "\n - " +
                       string.Join("\n - ", res.Warnings.Take(10)) +
                       (res.Warnings.Count > 10 ? "\n - …" : "");
            if (MessageBox.Show(owner, msg + "\n\n" + Loc.T("Open the output folder?"), Loc.T("OBJ → TMD (replace)"),
                    MessageBoxButtons.YesNo,
                    res.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information) == DialogResult.Yes)
            { try { System.Diagnostics.Process.Start(Path.GetDirectoryName(res.OutPath)); } catch { } }
        }

        /// <summary>Best-effort guess at the .TMD an exported .obj came from: the meta
        /// file records the source name, so read it rather than guessing on stem.</summary>
        private static string GuessTmdFor(string objPath)
        {
            try
            {
                string dir = Path.GetDirectoryName(objPath);
                string meta = Path.Combine(dir, Path.GetFileNameWithoutExtension(objPath) + ".tmdmeta.json");
                if (File.Exists(meta))
                {
                    foreach (string line in File.ReadAllLines(meta))
                    {
                        int i = line.IndexOf("\"source\"", StringComparison.Ordinal);
                        if (i < 0) continue;
                        int a = line.IndexOf('"', i + 8);
                        if (a < 0) continue;
                        a = line.IndexOf('"', a + 1);
                        if (a < 0) continue;
                        int b = line.IndexOf('"', a + 1);
                        if (b <= a) continue;
                        string name = line.Substring(a + 1, b - a - 1);
                        string cand = Path.Combine(dir, name);
                        if (File.Exists(cand)) return cand;
                    }
                }
                string sib = Path.Combine(dir, Path.GetFileNameWithoutExtension(objPath) + ".TMD");
                if (File.Exists(sib)) return sib;
            }
            catch (Exception) { }
            return null;
        }

        /// <summary>High-poly replacement flow: the checkbox dialog collects the model, the donor
        /// ILM and which automatic fixes to run, then BuildHighPoly does atlas + geometry + v7 in one
        /// pass (or geometry + v7 when auto-texture is off).</summary>
        public static void HighPolyImport(IWin32Window owner, string gameRoot)
        {
            string obj, ilm;
            bool autoTex, doWind, doMirror, doSeams, simple;
            using (var dlg = new HighPolyDialog(gameRoot))
            {
                if (dlg.ShowDialog(owner) != DialogResult.OK) return;
                simple = dlg.Simple;
                obj = dlg.ObjPath; ilm = dlg.IlmPath;
                autoTex = dlg.AutoTexture; doWind = dlg.DoWinding; doMirror = dlg.DoMirror; doSeams = dlg.DoSeams;
            }
            if (simple) { SimpleImport(owner, gameRoot); return; }

            string donorStem = Path.GetFileNameWithoutExtension(ilm);
            string outIlm;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = Loc.T("Save the rebuilt model");
                sfd.Filter = Loc.T("Model files") + " (*.ilm)|*.ilm|" + Loc.T("All files") + " (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(obj);
                sfd.FileName = donorStem + ".ILM";
                if (sfd.ShowDialog(owner) != DialogResult.OK) return;
                outIlm = sfd.FileName;
            }
            string atlasPng = Path.Combine(Path.GetDirectoryName(outIlm), donorStem + ".TIM.png");
            var geo = new GeometryPrep.Options { FixWinding = doWind, MirrorLR = doMirror, CloseSeams = doSeams };

            AtlasPrep.Result hp = null;
            try
            {
                ProgressDialog.Run(owner, Loc.T("Building high-poly model…"),
                    r => { hp = AtlasPrep.BuildHighPoly(obj, ilm, outIlm, atlasPng, geo, autoTex); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner, Loc.F("High-poly build failed:\n\n{0}", ex.Message), Loc.T("OBJ → Model"),
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (hp == null || !string.IsNullOrEmpty(hp.Error))
            {
                MessageBox.Show(owner, Loc.F("High-poly build failed:\n\n{0}", hp != null ? hp.Error : Loc.T("unknown error")),
                    Loc.T("OBJ → Model"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string msg = Loc.T("Built a high-poly model:") + "\n" + outIlm;
            if (hp.AtlasPath != null)
                msg += "\n" + atlasPng + "  " + Loc.F("({0} textures, {1}x{2})", hp.Textures, hp.AtlasW, hp.AtlasH);
            // The weld count is what says the joints will hold in motion, so it is
            // reported next to the file, not buried in the transcript.
            if (hp.Welds > 0)
                msg += "\n\n" + Loc.F("{0} cross-part weld(s): those joints follow the neighbouring part's " +
                       "bone, so they stay closed when the model animates.", hp.Welds);
            msg += "\n\n" + (hp.AtlasPath != null
                ? Loc.F("Drop BOTH files into gamedata\\load\\CHARA\\ under the ORIGINAL names ({0} and {1}) " +
                        "and set allow_loose_files = 1.", donorStem + ".ILM", donorStem + ".TIM.png")
                : Loc.F("Drop the .ILM into gamedata\\load\\CHARA\\ under the ORIGINAL name ({0}) " +
                        "and set allow_loose_files = 1.", donorStem + ".ILM"));
            if (hp.Warnings.Count > 0)
                msg += "\n\n" + Loc.F("Warnings ({0}):", hp.Warnings.Count) + "\n - " +
                       string.Join("\n - ", hp.Warnings.Take(10)) + (hp.Warnings.Count > 10 ? "\n - …" : "");
            var icon = hp.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information;
            if (MessageBox.Show(owner, msg + "\n\n" + Loc.T("Open the output folder?"), Loc.T("OBJ → Model"),
                    MessageBoxButtons.YesNo, icon) == DialogResult.Yes)
            { try { System.Diagnostics.Process.Start(Path.GetDirectoryName(outIlm)); } catch { } }
        }

        /// <summary>Simple edit-existing import: reshape the current character within the vertex
        /// limit (auto patch / grow / replace). Reached from the dialog's "Simple…" button.</summary>
        public static void SimpleImport(IWin32Window owner, string gameRoot)
        {
            string obj;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = Loc.T("Select your edited model (.obj)");
                ofd.Filter = "Wavefront OBJ (*.obj)|*.obj|" + Loc.T("All files") + " (*.*)|*.*";
                string gamedata = Path.Combine(gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                obj = ofd.FileName;
            }

            string ilm = GuessIlmFor(obj);
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = Loc.T("Select the ORIGINAL model (.ILM) this OBJ was exported from");
                ofd.Filter = Loc.T("Model files") + " (*.ilm)|*.ilm|" + Loc.T("All files") + " (*.*)|*.*";
                if (ilm != null) { ofd.InitialDirectory = Path.GetDirectoryName(ilm); ofd.FileName = Path.GetFileName(ilm); }
                else ofd.InitialDirectory = Path.GetDirectoryName(obj);
                if (ofd.ShowDialog(owner) != DialogResult.OK) return;
                ilm = ofd.FileName;
            }

            string outIlm;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = Loc.T("Save the rebuilt model");
                sfd.Filter = Loc.T("Model files") + " (*.ilm)|*.ilm|" + Loc.T("All files") + " (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(obj);
                sfd.FileName = Path.GetFileNameWithoutExtension(ilm) + "_new.ILM";
                if (sfd.ShowDialog(owner) != DialogResult.OK) return;
                outIlm = sfd.FileName;
            }

            // Grow-mode is passed unconditionally rather than exposed as a checkbox. The converter
            // falls through to the byte-identical patch-in-place path whenever the OBJ added
            // nothing, so the flag alone can never alter a same-topology rebuild — which means a
            // checkbox would only add two ways to be surprised (forget to tick it and a legitimate
            // grow is rejected as a vertex-count mismatch; tick it on an unchanged mesh and nothing
            // happens for reasons the user cannot see). Detection is exact instead of guessed, and
            // the confirmation below quotes the converter's own per-part deltas.
            //
            // Full replacement is offered the same way and for the same reason, one rung further
            // down: it is tried only after this call has refused, so the safest expressible path
            // always wins and a rebuild is never the quiet answer to a small edit.
            //
            // It builds to a sibling temp file: a refusal (or a declined confirmation) must not
            // leave a half-written or unwanted .ILM at the path the user picked.
            string tmpIlm = outIlm + ".rebuild.tmp";
            IlmObjConverter.ImportResult res = null;
            try
            {
                ProgressDialog.Run(owner, Loc.T("Rebuilding model…"),
                    r => { res = IlmObjConverter.Import(obj, ilm, tmpIlm, true); });
            }
            catch (Exception ex)
            {
                TryDelete(tmpIlm);
                MessageBox.Show(owner, Loc.F("Rebuild failed:\n\n{0}", ex.Message),
                    Loc.T("OBJ → Model"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (res == null || !string.IsNullOrEmpty(res.Error))
            {
                IlmObjConverter.ImportResult diag = PreferPlainDiagnosis(res, obj, ilm, tmpIlm);
                // Escalate to a full rebuild only after BOTH cheaper paths have refused, and only
                // when they refused because the OBJ's topology no longer matches the template —
                // which is the one thing a rebuild fixes. Anything else (a renamed part, a missing
                // .ilmmeta.json) would refuse identically, so offering a rebuild would just be a
                // second dead end. Ordering the paths cheapest-first is what stops a user who only
                // reshaped a mesh from ever being handed a rebuild: patch-in-place is byte-exact,
                // grow preserves the original geometry and its sharing graph, and only a change
                // neither can express reaches the rebuild — which then still has to be confirmed.
                bool topologyChanged = (res != null && res.ReplaceMayHelp) ||
                                       (diag != null && diag.ReplaceMayHelp);
                IlmObjConverter.ImportResult rep = topologyChanged ? TryReplace(owner, obj, ilm, tmpIlm) : null;
                if (rep == null || !string.IsNullOrEmpty(rep.Error))
                {
                    TryDelete(tmpIlm);
                    ShowImportFailure(owner, diag, rep);
                    return;
                }
                if (!ConfirmReplace(owner, rep, diag)) { TryDelete(tmpIlm); return; }
                res = rep;
            }
            else if (res.Grew && !ConfirmGrow(owner, res)) { TryDelete(tmpIlm); return; }

            try
            {
                // Replace, never delete-then-move: Delete and Move are not atomic, so a Move that
                // loses a race with an AV scanner (or the preview window's own handle on the file
                // just written) after the Delete succeeded leaves NEITHER model on disk. Rebuilding
                // straight onto an existing destination is the documented workflow, not an edge case.
                if (File.Exists(outIlm)) File.Replace(tmpIlm, outIlm, null);
                else                     File.Move(tmpIlm, outIlm);
            }
            catch (Exception ex)
            {
                // The temp is the ONLY copy of the rebuild and the destination may still hold the
                // user's previous model - deleting either here loses work.
                MessageBox.Show(owner, Loc.F("The model rebuilt, but it could not be moved to:\n{0}\n\n{1}\n\n" +
                                             "The rebuilt model is here:\n{2}", outIlm, ex.Message, tmpIlm),
                    Loc.T("OBJ → Model"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            res.IlmPath = outIlm;

            if (res.Replaced)
            {
                ShowReplaceReport(owner, res);
                if (MessageBox.Show(owner, Loc.T("Open the output folder?"), Loc.T("OBJ → Model"),
                        MessageBoxButtons.YesNo, MessageBoxIcon.Information) != DialogResult.Yes)
                    return;
            }
            else if (res.Grew)
            {
                ShowGrowReport(owner, res);
                if (MessageBox.Show(owner, Loc.T("Open the output folder?"), Loc.T("OBJ → Model"),
                        MessageBoxButtons.YesNo, MessageBoxIcon.Information) != DialogResult.Yes)
                    return;
            }
            else
            {
                string msg = Loc.F("Rebuilt {0} body part(s), {1} vertices, {2} normals and {3} face(s):",
                                   res.Parts, res.Vertices, res.Normals, res.Prims) + "\n" + res.IlmPath +
                    "\n\n" + Loc.T("To use it, drop it into gamedata\\load\\<FOLDER>\\ under its ORIGINAL name " +
                                   "(e.g. gamedata\\load\\CHARA\\DOB.ILM) and set allow_loose_files = 1 in config.cfg.");
                // Same contract as the export dialog: res.Warnings carries seam-edit diagnoses the user
                // cannot see in the written file, so a success box must not swallow them.
                if (res.Warnings.Count > 0)
                    msg += "\n\n" + Loc.F("Warnings ({0}):", res.Warnings.Count) + "\n - " +
                           string.Join("\n - ", res.Warnings.Take(8)) +
                           (res.Warnings.Count > 8 ? "\n - …" : "");
                if (MessageBox.Show(owner, msg + "\n\n" + Loc.T("Open the output folder?"),
                        Loc.T("OBJ → Model"), MessageBoxButtons.YesNo,
                        res.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information) != DialogResult.Yes)
                    return;
            }

            try { System.Diagnostics.Process.Start(Path.GetDirectoryName(res.IlmPath)); } catch { }
        }

        /// <summary>"Model Viewer": show the (single, reused) viewer window, empty —
        /// the user opens models from File > Open or by dropping them on it.</summary>
        public static void OpenModelViewer(IWin32Window owner, string gameRoot)
        {
            ModelViewerForm.Open(owner, null, gameRoot);
        }

        /// <summary>Sound-bank browser. Starts in the extracted SND/ folder when one is
        /// findable, since that is where all 90 banks live and hunting for it by hand is
        /// the first thing every user would otherwise have to do.</summary>
        public static void OpenXaTool(IWin32Window owner, string gameRoot)
        {
            XaToolForm.ShowTool(owner, gameRoot);
        }

        public static void OpenAudioTool(IWin32Window owner, string gameRoot)
        {
            string clean = AudioToolForm.SavedCleanSndDir();
            if (clean == null) clean = GuessCleanSndDir(gameRoot);

            string start = clean;
            if (start == null && !string.IsNullOrEmpty(gameRoot))
            {
                string data = Path.Combine(gameRoot, "gamedata");
                string loadSnd = Path.Combine(Path.Combine(data, "load"), "SND");
                start = Directory.Exists(loadSnd) ? loadSnd : Directory.Exists(data) ? data : gameRoot;
            }
            AudioToolForm.ShowTool(owner, gameRoot, start, clean);
        }

        /// <summary>The disc's own SND/ folder, as a disc extract under gamedata/ holds
        /// it; null when there is none.
        ///
        /// gamedata/load is skipped by name. It is where edited banks go, and it sorts
        /// before "Silent Hill (USA)_extracted", so a first-match search picked it —
        /// the Audio tool then took the user's edits for the disc's sounds and could no
        /// longer find the other copies of any sound already replaced. Among the
        /// extracts, the one holding the most banks wins, since a complete SND/ is the
        /// only kind that can answer "where else is this sound".</summary>
        private static string GuessCleanSndDir(string gameRoot)
        {
            if (string.IsNullOrEmpty(gameRoot)) return null;

            string data = Path.Combine(gameRoot, "gamedata");
            if (!Directory.Exists(data)) return null;

            string best = null;
            int bestCount = 0;
            try
            {
                foreach (string sub in Directory.GetDirectories(data))
                {
                    if (string.Equals(Path.GetFileName(sub), "load", StringComparison.OrdinalIgnoreCase)) continue;
                    string snd = Path.Combine(sub, "SND");
                    if (!Directory.Exists(snd)) continue;
                    int n;
                    try { n = Directory.GetFiles(snd, "*.vab").Length; } catch { continue; }
                    if (n > bestCount)
                    {
                        best = snd;
                        bestCount = n;
                    }
                }
            }
            catch { }
            return best;
        }

        // ---- helpers moved verbatim from ModManagerForm -------------------------

        /// <summary>Delete a scratch file, ignoring anything that goes wrong — the caller is
        /// already on an error path and the failure it is reporting is the interesting one.</summary>
        private static void TryDelete(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); } catch { }
        }

        /// <summary>Pick the refusal that names the right part. Grow-detection fires on ANY extra
        /// vertex or face, so an accidental topology change — one stray vertex, a face duplicated by
        /// a mirror or Ctrl+D — is diagnosed by GrowImport's face/vertex pairing check instead of the
        /// plain path's count check. Adding a vertex shifts every later global OBJ index, so the
        /// pairing error blames whichever part the shift lands in, which is almost never the part the
        /// user edited. The plain path's count check names the right part and the right cause, so when
        /// the growth was not deliberate its refusal is the one to show. A ceiling refusal (Report
        /// filled) is already the accurate diagnosis of a REAL grow and must survive untouched.</summary>
        private static IlmObjConverter.ImportResult PreferPlainDiagnosis(
            IlmObjConverter.ImportResult grown, string obj, string ilm, string tmpIlm)
        {
            if (grown == null || grown.Report.Count > 0) return grown;
            IlmObjConverter.ImportResult plain = IlmObjConverter.Import(obj, ilm, tmpIlm, false);
            TryDelete(tmpIlm);
            return (plain != null && !string.IsNullOrEmpty(plain.Error)) ? plain : grown;
        }

        /// <summary>Run the full-rebuild path into the same temp file. Auto-weld needs a rest pose;
        /// when the OBJ was exported without one the converter says so and the user is asked once,
        /// because that model has no other route to a rebuild — and a rebuild without welding is a
        /// real answer, not a workaround: the joints then stay closed on the parts' own overlap,
        /// which is how a model authored with overlapping limbs already works.</summary>
        private static IlmObjConverter.ImportResult TryReplace(IWin32Window owner, string obj, string ilm, string tmpIlm)
        {
            var opts = new IlmObjConverter.ImportOptions { Replace = true };
            IlmObjConverter.ImportResult rep = RunReplace(owner, obj, ilm, tmpIlm, opts);
            if (rep == null || string.IsNullOrEmpty(rep.Error) || !rep.WeldNeedsRestPose) return rep;

            string msg = Loc.F("This model was exported without its animation file, so every body part " +
                "sits on the origin in the .obj rather than where it belongs on the character.\n\n" +
                "Joints can therefore not be found automatically: two vertices being in the same " +
                "place no longer means they meet at a joint, and welding them would fuse parts " +
                "that are nowhere near each other on the real model.\n\n" +
                "Rebuild WITHOUT welding? Each part then carries its own seam vertices, so the " +
                "joints stay closed only while the parts' geometry overlaps — which is fine if you " +
                "modelled them overlapping, and leaves visible gaps if you did not.\n\n" +
                "(The alternative is to answer No, re-export the model with \"{0}\" from " +
                "a copy that has its ANIM folder beside it, and redo the edit.)", Loc.T("Model → OBJ ▾"));
            if (MessageBox.Show(owner, msg, Loc.T("OBJ → Model"), MessageBoxButtons.YesNo,
                    MessageBoxIcon.Warning) != DialogResult.Yes)
                return rep;

            opts.Weld = false;
            return RunReplace(owner, obj, ilm, tmpIlm, opts);
        }

        /// <summary>Anything the converter throws OUTSIDE its own guards (an unreadable path, a
        /// full disk) comes back as an Error like any refusal, so one caller reports one failure.</summary>
        private static IlmObjConverter.ImportResult RunReplace(IWin32Window owner, string obj, string ilm, string tmpIlm,
            IlmObjConverter.ImportOptions opts)
        {
            IlmObjConverter.ImportResult rep = null;
            try
            {
                ProgressDialog.Run(owner, Loc.T("Rebuilding model…"),
                    r => { rep = IlmObjConverter.Import(obj, ilm, tmpIlm, opts); });
            }
            catch (Exception ex)
            {
                rep = new IlmObjConverter.ImportResult();
                rep.Error = ex.Message;
            }
            return rep;
        }

        /// <summary>Ask before committing a full rebuild. Everything the modeller needs to judge it
        /// is in here, not in the success dialog: which parts moved somewhere unexpected, which
        /// vertices changed BONE, and how close the seams that did not weld came. Those are the
        /// signals that say "you got it wrong", and a dialog that shows them only after the file is
        /// written is a dialog that shows them too late.</summary>
        private static bool ConfirmReplace(IWin32Window owner, IlmObjConverter.ImportResult res, IlmObjConverter.ImportResult why)
        {
            var lines = new List<string>();
            lines.Add(Loc.T("REBUILD THE WHOLE MODEL?"));
            lines.Add("");
            Para(lines, Loc.T("Your .obj no longer carries the same vertices and faces as the original, so it " +
                              "cannot be folded back in piece by piece:"), "", 80);
            lines.Add("");
            Para(lines, why != null ? why.Error : Loc.T("the geometry no longer matches"), "    ", 80);
            lines.Add("");
            Para(lines, Loc.T("It CAN be rebuilt from scratch. The original .ILM then supplies only the rig — " +
                              "bone bindings, draw order, materials and palette rows — and every vertex, face, " +
                              "normal and UV comes from your .obj. Part names and part count still may not " +
                              "change: an 'o' object IS a bone."), "", 80);
            lines.Add("");
            lines.Add(Loc.T("REBUILT"));
            lines.Add("    " + Loc.F("{0} body part(s), {1} vertices, {2} normals, {3} face(s).",
                                     res.Parts, res.Vertices, res.Normals, res.Prims));
            lines.Add("");

            AddUnexpected(res, lines);
            AddBoneChanges(res, lines);
            AddJoints(res, lines);
            lines.Add("");
            lines.Add(Loc.T("REBUILD REPORT"));
            AddReport(res, lines);
            lines.Add("");
            lines.Add(Loc.T("Nothing has been written yet."));
            return ShowTextDialog(owner, Loc.T("OBJ → Model — Rebuild?"), lines.ToArray(), true, Loc.T("Rebuild"));
        }

        private static void AddUnexpected(IlmObjConverter.ImportResult res, List<string> lines)
        {
            if (res.Warnings.Count == 0) return;
            lines.Add("!!  " + Loc.F("GEOMETRY IN AN UNEXPECTED PLACE  ({0})", res.Warnings.Count));
            lines.Add("");
            foreach (string w in res.Warnings)
            {
                Para(lines, w, "    ", 78);
                lines.Add("");
            }
        }

        /// <summary>Append <paramref name="text"/> wrapped to <paramref name="width"/> columns,
        /// every line prefixed with <paramref name="indent"/>.</summary>
        private static void Para(List<string> lines, string text, string indent, int width)
        {
            foreach (string l in Wrap(text, width - indent.Length)) lines.Add(indent + l);
        }

        /// <summary>A numbered install step with a hanging indent under its number.</summary>
        private static void Step(List<string> lines, string number, string text)
        {
            bool first = true;
            foreach (string l in Wrap(text, 74))
            {
                lines.Add((first ? "  " + number + "  " : "      ") + l);
                first = false;
            }
        }

        /// <summary>The two install rules a full-rewrite model must meet, shared by the grow and
        /// rebuild reports. Only the reason in step 1 differs between them.</summary>
        private static void AddInstallSteps(List<string> lines, string why, string fileName)
        {
            lines.Add(Loc.T("INSTALLING IT  (both of these or it will not load)"));
            lines.Add("");
            Step(lines, "1.", Loc.F("Tick \"{0}\" at the bottom of the Mod Manager (allow_loose_files = 1 in config.cfg).",
                                    Loc.T("Enable loose file support (required for load-folder mods)")) + " " + why + " " +
                              Loc.T("Without it the game quietly keeps the original and nothing looks wrong."));
            lines.Add("");
            Step(lines, "2.", Loc.T("Copy it to") + "   gamedata\\load\\<FOLDER>\\<ORIGINAL NAME>.ILM");
            lines.Add("      " + Loc.T("e.g.") + "   gamedata\\load\\CHARA\\DOB.ILM");
            Para(lines, Loc.F("Under the ORIGINAL name, in the folder it came from. The game looks the file up " +
                              "by that name — {0} will never be found.", fileName), "      ", 80);
            lines.Add("");
            Para(lines, Loc.T("An enabled TEXTURE PACK is no obstacle: a pack's .png for this character overrides " +
                              "that character's TEXTURE only and never interferes with the loose .ILM."), "", 80);
        }

        /// <summary>The welds that handed a vertex to a different BONE. A weld inside one bone is
        /// invisible at runtime; one that crosses a bone is the only kind that changes how the
        /// model animates, so it gets its own banner rather than a line buried in the transcript.</summary>
        private static void AddBoneChanges(IlmObjConverter.ImportResult res, List<string> lines)
        {
            if (res.CrossedBones.Count == 0) return;
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            lines.Add("!!  " + Loc.F("VERTICES THAT CHANGED BONE  ({0} part pair(s))", res.CrossedBones.Count));
            lines.Add("");
            Para(lines, Loc.T("These vertices were welded onto a part driven by a DIFFERENT bone, so " +
                "they will animate with that part from now on. If those two parts do not actually meet at a " +
                "joint, pull the vertices apart in your modeller and rebuild."), "    ", 78);
            lines.Add("");
            foreach (IlmObjConverter.CrossedBoneInfo c in res.CrossedBones)
                lines.Add("    " + Loc.F("{0} vertex/vertices of {1} moved up to {2} units onto {3}",
                                         c.Count.ToString(inv).PadLeft(4), c.Reader.PadRight(10),
                                         c.MaxDistance.ToString("0.000", inv), c.Owner));
            lines.Add("");
        }

        private static void AddJoints(IlmObjConverter.ImportResult res, List<string> lines)
        {
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            lines.Add(Loc.T("JOINTS"));
            lines.Add("");
            if (!res.WeldEnabled)
            {
                Para(lines, Loc.T("Welding is OFF, so every part keeps its own seam vertices. The " +
                    "joints stay closed only while the parts' geometry overlaps."), "    ", 78);
                return;
            }
            Para(lines, Loc.F("This format has no skinning and no weights: a seam stays closed only " +
                "when the later-drawn part reads the earlier one's vertex. Vertices of two parts that landed " +
                "in the same place (within {0} units) were welded that way — {1} vertices across {2} part pair(s).",
                res.WeldEps.ToString("0.####", inv), res.WeldedVertices.ToString(inv),
                res.WeldPairs.Count.ToString(inv)), "    ", 78);
            lines.Add("");
            foreach (IlmObjConverter.WeldPairInfo p in res.WeldPairs)
                lines.Add("    " + Loc.F("{0} reads {1}", p.Reader.PadRight(10), p.Owner.PadRight(10)) + "  " +
                          p.Count.ToString(inv));
            if (res.WeldPairs.Count == 0)
                lines.Add("    " + Loc.T("(none - no two parts had a vertex in the same place)"));
            if (res.NearMissDistance >= 0.0)
            {
                lines.Add("");
                Para(lines, Loc.F("HINT: the closest pair of vertices in different parts that did NOT " +
                    "weld is {0} units apart. If those were meant to be one seam, snap them to exactly the same " +
                    "place in your modeller and rebuild — widening the radius instead risks welding geometry " +
                    "onto the wrong bone.", res.NearMissDistance.ToString("0.000", inv)), "    ", 78);
            }
        }

        /// <summary>The converter's own transcript. The monospace dialog turns word wrap OFF so its
        /// column-aligned tables line up, so the prose lines in here (HINT, NOTE) are wrapped by
        /// hand — otherwise they run off the right edge and are read by nobody.</summary>
        private static void AddReport(IlmObjConverter.ImportResult res, List<string> lines)
        {
            foreach (string raw in res.Report)
            {
                if (raw.Length <= 80) { lines.Add(raw); continue; }
                int indent = 0;
                while (indent < raw.Length && raw[indent] == ' ') indent++;
                string pad = new string(' ', indent);
                bool first = true;
                foreach (string s in Wrap(raw.Substring(indent), 78 - indent))
                {
                    lines.Add((first ? pad : pad + "  ") + s);
                    first = false;
                }
            }
        }

        /// <summary>Success dialog for a rebuilt model. Same install rules as a grown model (it is
        /// a full rewrite either way), and the same weld/bone signals as the confirmation, because
        /// this is the copy the user keeps on screen while checking the result in the viewer.</summary>
        private static void ShowReplaceReport(IWin32Window owner, IlmObjConverter.ImportResult res)
        {
            string name = Path.GetFileName(res.IlmPath);
            var lines = new List<string>();
            lines.Add(Loc.T("REBUILT FROM YOUR MESH"));
            lines.Add("");
            lines.Add(Loc.F("Written:  {0}", res.IlmPath));
            lines.Add(Loc.F("Rebuilt {0} body part(s): {1} vertices, {2} normals, {3} face(s).",
                            res.Parts, res.Vertices, res.Normals, res.Prims));
            lines.Add("");
            AddInstallSteps(lines, Loc.T("A rebuilt model is a full rewrite and is read ONLY through the loose-file path."), name);
            lines.Add("");
            lines.Add(Loc.F("Check it before you install: \"{0}\" opens the rebuilt .ILM.", Loc.T("Model Viewer")));
            lines.Add("");
            AddUnexpected(res, lines);
            AddBoneChanges(res, lines);
            AddJoints(res, lines);
            lines.Add("");
            lines.Add(Loc.T("REBUILD REPORT"));
            AddReport(res, lines);
            ShowTextDialog(owner, Loc.T("OBJ → Model — Rebuilt Model"), lines.ToArray(), true);
        }

        /// <summary>Break a long message into fixed-width lines. The monospace dialog turns word
        /// wrap off so its tables line up, which would otherwise clip these sentences.</summary>
        private static string[] Wrap(string s, int width)
        {
            var outp = new List<string>();
            var line = new System.Text.StringBuilder();
            int lineW = 0;
            foreach (string word in s.Split(' '))
            {
                if (word.Length == 0) continue;
                // Japanese and Chinese have no spaces to break on, so a "word" can be a whole
                // sentence; it is split wherever the column budget runs out instead.
                string w = word;
                while (w.Length > 0)
                {
                    int ww = DisplayWidth(w);
                    int room = width - lineW - (lineW == 0 ? 0 : 1);
                    if (ww <= room)
                    {
                        if (lineW != 0) { line.Append(' '); lineW++; }
                        line.Append(w); lineW += ww;
                        break;
                    }
                    if ((ww <= width || ww == w.Length) && lineW != 0)
                    {
                        outp.Add(line.ToString()); line.Length = 0; lineW = 0;
                        continue;
                    }
                    if (ww == w.Length) { outp.Add(w); break; } // an over-long path stays whole, as before
                    if (lineW != 0) { outp.Add(line.ToString()); line.Length = 0; lineW = 0; }
                    int take = 0, used = 0;
                    while (take < w.Length && used + CharWidth(w[take]) <= width) { used += CharWidth(w[take]); take++; }
                    if (take == 0) take = 1;
                    outp.Add(w.Substring(0, take));
                    w = w.Substring(take);
                }
            }
            if (line.Length != 0) outp.Add(line.ToString());
            if (outp.Count == 0) outp.Add("");
            return outp.ToArray();
        }

        /// <summary>Columns a character takes in the monospace report: CJK and full-width
        /// forms render two cells wide.</summary>
        private static int CharWidth(char c)
        {
            return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
                   (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
                   (c >= 0xFFE0 && c <= 0xFFE6) ? 2 : 1;
        }

        private static int DisplayWidth(string s)
        {
            int w = 0;
            foreach (char c in s) w += CharWidth(c);
            return w;
        }

        /// <summary>Import refusal. A ceiling refusal (too many vertices/normals/prims/parts for the
        /// file format's u8 slot indices) fills res.Report with the same budget table a success
        /// produces, so the user can see WHICH part blew WHICH limit — that only reads in the
        /// monospace dialog, so route it there and keep the MessageBox for plain failures.
        ///
        /// `rebuild` is the full-rebuild attempt, when one was made. Its refusal is reported too:
        /// having silently tried a second path and said nothing about why it also failed is how a
        /// user ends up believing the file is unfixable.</summary>
        private static void ShowImportFailure(IWin32Window owner, IlmObjConverter.ImportResult res, IlmObjConverter.ImportResult rebuild)
        {
            string err = res != null ? res.Error : Loc.T("unknown error");
            string alsoTried = (rebuild != null && !string.IsNullOrEmpty(rebuild.Error))
                ? "\n\n" + Loc.T("Rebuilding the model from scratch was tried as well, and refused too:") + "\n\n" + rebuild.Error
                : null;
            if (res == null || res.Report.Count == 0)
            {
                MessageBox.Show(owner, Loc.F("Rebuild failed:\n\n{0}", err) + alsoTried,
                    Loc.T("OBJ → Model"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            var lines = new List<string>();
            lines.Add(Loc.T("REBUILD REFUSED — THE MODEL DOES NOT FIT"));
            lines.Add("");
            lines.Add(err);
            lines.Add("");
            lines.Add(Loc.T("Nothing was written."));
            lines.Add("");
            Para(lines, Loc.T("A body part addresses its vertices, normals and faces with single bytes, so " +
                              "the limits below are hard: no build of the game can load a part past them. " +
                              "Remove geometry from the part named above — decimate it in Blender, or move " +
                              "some of the detail onto a neighbouring part — and rebuild."), "", 80);
            lines.Add("");
            lines.Add(Loc.T("BUDGET AT THE POINT OF REFUSAL"));
            lines.AddRange(res.Report);
            if (alsoTried != null)
            {
                lines.Add("");
                lines.Add(Loc.T("A FULL REBUILD WAS TRIED TOO, AND REFUSED:"));
                lines.Add("");
                Para(lines, rebuild.Error, "  ", 78);
            }
            ShowTextDialog(owner, Loc.T("OBJ → Model — Refused"), lines.ToArray(), true);
        }

        /// <summary>Ask before committing a grown model: it is a different KIND of output (needs
        /// loose-file support, cannot go back on a disc), and reaching it by accident — one stray
        /// subdivide in Blender — must not be silent. The counts come from the converter's own
        /// per-part delta lines, which are the first GrownParts.Count entries of Report.</summary>
        private static bool ConfirmGrow(IWin32Window owner, IlmObjConverter.ImportResult res)
        {
            int n = res.GrownParts.Count;
            string deltas = string.Join("\n", res.Report.Take(Math.Min(n, 12))) +
                            (n > 12 ? "\n   " + Loc.F("… and {0} more (full table follows)", n - 12) : "");
            string msg = Loc.T("This OBJ ADDS geometry — the rebuilt model is larger than the original.") + "\n\n" +
                Loc.F("New geometry in {0} of {1} body part(s):", n, res.Parts) + "\n" +
                deltas + "\n\n" +
                Loc.F("A larger-than-original model loads only through this port's loose-file path with " +
                      "\"{0}\" switched on. If you did not mean to add geometry, " +
                      "answer No, undo the change in Blender and rebuild.",
                      Loc.T("Enable loose file support (required for load-folder mods)")) + "\n\n" +
                Loc.T("Rebuild as a larger-than-original model?");
            return MessageBox.Show(owner, msg, Loc.T("OBJ → Model"), MessageBoxButtons.YesNo,
                       MessageBoxIcon.Question) == DialogResult.Yes;
        }

        /// <summary>Success dialog for a grown model. The budget table is column-aligned and far
        /// too long for a MessageBox, and the install rules are stricter than the plain path's, so
        /// this goes in the scrollable monospace dialog.</summary>
        private static void ShowGrowReport(IWin32Window owner, IlmObjConverter.ImportResult res)
        {
            string name = Path.GetFileName(res.IlmPath);
            var lines = new List<string>();
            lines.Add(Loc.T("REBUILT AS A LARGER-THAN-ORIGINAL MODEL"));
            lines.Add("");
            lines.Add(Loc.F("Written:  {0}", res.IlmPath));
            lines.Add(Loc.F("Grew {0} of {1} body part(s), adding {2} new face(s).",
                            res.GrownParts.Count, res.Parts, res.Prims));
            lines.Add("");
            AddInstallSteps(lines, Loc.T("A model bigger than the one on the disc is read ONLY through the loose-file path."), name);
            if (res.Warnings.Count > 0)
            {
                lines.Add("");
                lines.Add(Loc.F("WARNINGS ({0})", res.Warnings.Count));
                foreach (var w in res.Warnings) lines.Add("  - " + w);
            }
            lines.Add("");
            lines.Add(Loc.T("BUDGET  (per part: used / limit, and the pool window it occupies)"));
            lines.AddRange(res.Report);
            ShowTextDialog(owner, Loc.T("OBJ → Model — Grown Model"), lines.ToArray(), true);
        }

        /// <summary>Composite the character's true in-game texture (each region through its
        /// own CLUT palette row) to a PNG beside the OBJ, and point every material in the
        /// MTL at it via map_Kd, so the mesh shows textured in Blender instead of flat
        /// white. One image serves every material because the composite already bakes each
        /// region's palette — which is also exactly what a single loose-PNG override does
        /// in game (hires_override.c registers one image across all rows). Returns a
        /// one-line note for the success dialog, or null when there is nothing to do.</summary>
        private static string TextureExportedObj(string ilm, string objPath, string mtlPath)
        {
            try
            {
                string tim = FindTimBeside(ilm);
                if (tim == null || !File.Exists(mtlPath)) return null;
                string pngPath = Path.Combine(Path.GetDirectoryName(objPath),
                    Path.GetFileNameWithoutExtension(objPath) + "_texture.png");
                string err;
                if (!ClutComposer.Compose(ilm, tim, pngPath, out err)) return null;

                string pngRef = Path.GetFileName(pngPath);
                var outLines = new List<string>();
                bool wroteForBlock = false;
                foreach (string line in File.ReadAllLines(mtlPath))
                {
                    string t = line.TrimStart();
                    if (t.StartsWith("newmtl ", StringComparison.Ordinal)) wroteForBlock = false;
                    // Drop any stale map_Kd so re-exporting over an old MTL stays clean.
                    if (t.StartsWith("map_Kd ", StringComparison.Ordinal)) continue;
                    outLines.Add(line);
                    // The composite is opaque; a d/Tr line above would make Blender show it
                    // see-through, so add map_Kd right after Kd where neither is affected.
                    if (!wroteForBlock && t.StartsWith("Kd ", StringComparison.Ordinal))
                    {
                        outLines.Add("map_Kd " + pngRef);
                        wroteForBlock = true;
                    }
                }
                File.WriteAllLines(mtlPath, outLines.ToArray());
                return Loc.F("Textured preview: {0} is beside the OBJ and wired into the " +
                             "MTL, so the mesh shows its real in-game texture in Blender. (Import " +
                             "ignores the MTL — this is display only.)", pngRef);
            }
            catch { return null; }  // a preview convenience must never fail the export
        }

        /// <summary>Find NAME.TIM beside NAME.ILM (either extension case).</summary>
        private static string FindTimBeside(string ilm)
        {
            string c = Path.ChangeExtension(ilm, ".TIM");
            if (File.Exists(c)) return c;
            c = Path.ChangeExtension(ilm, ".tim");
            return File.Exists(c) ? c : null;
        }

        /// <summary>Best-effort guess of the .ILM a file came from
        /// (NAME.obj / NAME_reference.png -> NAME.ILM beside it).</summary>
        private static string GuessIlmFor(string path)
        {
            string dir = Path.GetDirectoryName(path);
            string stem = Path.GetFileNameWithoutExtension(path);
            if (stem.EndsWith("_reference", StringComparison.OrdinalIgnoreCase))
                stem = stem.Substring(0, stem.Length - "_reference".Length);
            foreach (var ext in new[] { ".ILM", ".ilm" })
            {
                string c = Path.Combine(dir ?? ".", stem + ext);
                if (File.Exists(c)) return c;
            }
            return null;
        }

        public static void ShowTextDialog(IWin32Window owner, string title, string[] lines, bool monospace)
        {
            ShowTextDialog(owner, title, lines, monospace, null);
        }

        /// <summary>With `confirmText` the Close button becomes that action plus a Cancel, and the
        /// return value is whether the user took it. A rebuild has to be judged from the weld and
        /// bone report, which is far too long for a MessageBox, so the question is asked on the
        /// same page as the evidence rather than in a box that follows it.</summary>
        public static bool ShowTextDialog(IWin32Window owner, string title, string[] lines, bool monospace, string confirmText)
        {
            int w = monospace ? 720 : 540;
            int h = monospace ? 470 : 420;
            bool confirm = !string.IsNullOrEmpty(confirmText);
            bool accepted = false;
            // A column-aligned table only lines up in a fixed-pitch face. The font is declared
            // OUTSIDE the form's using so it is disposed after the form, never while a live
            // control still references it. A null resource in a using is a legal no-op.
            using (Font mono = monospace ? new Font(FontFamily.GenericMonospace, 8.25f) : null)
            using (var dlg = new Form())
            {
                dlg.Text            = title;
                dlg.ClientSize      = new Size(w, h);
                dlg.FormBorderStyle = FormBorderStyle.FixedDialog;
                dlg.StartPosition   = FormStartPosition.CenterParent;
                dlg.MaximizeBox     = false;
                dlg.MinimizeBox     = false;
                try { dlg.Icon = Properties.Resources.launchericon; } catch { }

                var box = new TextBox
                {
                    Multiline   = true,
                    ReadOnly    = true,
                    ScrollBars  = monospace ? ScrollBars.Both : ScrollBars.Vertical,
                    WordWrap    = !monospace,
                    BorderStyle = BorderStyle.FixedSingle,
                    BackColor   = SystemColors.Window,
                    Location    = new Point(12, 12),
                    Size        = new Size(w - 24, h - 64),
                    TabStop     = false,
                    Text        = string.Join("\r\n", lines)
                };
                box.Select(0, 0);
                dlg.Controls.Add(box);

                var close = new Button
                {
                    Text = Loc.T(confirm ? "Cancel" : "Close"),
                    Location = new Point(w - 96, h - 40),
                    Size = new Size(84, 28),
                    DialogResult = confirm ? DialogResult.Cancel : DialogResult.OK
                };
                dlg.Controls.Add(close);
                dlg.CancelButton = close;
                dlg.AcceptButton = close;
                if (confirm)
                {
                    var go = new Button
                    {
                        Text = confirmText,
                        Location = new Point(w - 216, h - 40),
                        Size = new Size(112, 28),
                        DialogResult = DialogResult.OK
                    };
                    dlg.Controls.Add(go);
                    // Cancel stays the default button: this dialog exists because the safe paths
                    // already refused, so Enter must not commit a rebuild the user has not read.
                }

                if (mono != null) box.Font = mono;
                accepted = dlg.ShowDialog(owner) == DialogResult.OK;
            }
            return accepted;
        }
    }
}
