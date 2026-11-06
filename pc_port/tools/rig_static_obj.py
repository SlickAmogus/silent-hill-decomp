"""Rig a static T-pose humanoid OBJ into a skinned glTF for the character swap system.

Run inside Blender (4.x):

    blender --background --python tools/rig_static_obj.py -- IN.obj OUT.glb [key=value ...]

The OBJ must be Y-up, facing +Z (nose toward +Z), standing on y=0, arms out in a
T-pose with the character's left arm toward +X. That is the Mixamo frame, which is what
pc_skin_chara.c retargets from. A Mixamo-named skeleton (no prefix) is placed from
height slices of the mesh, weights are an inverse-distance falloff to the bone segments
with region gates (Blender's heat weighting fails on overlapping low-poly shells), the
texture is converted to PNG (the game does not decode JPEG), and the glb is exported
with the settings the Skinned_Character_Replacement_Guide asks for.

Landmarks are heuristics on proportions. Every one can be overridden on the command
line, in OBJ units: hips_y, knee_y, ankle_y, leg_x, shoulder_y, shoulder_x, wrist_x,
neck_y, head_y, center_z. Pass render=DIR to also write rest and bent-pose previews.
"""
import os
import sys
import math

import bpy
from mathutils import Vector, Quaternion


def parse_args():
    argv = sys.argv
    if "--" not in argv:
        raise SystemExit("usage: blender --background --python rig_static_obj.py -- IN.obj OUT.glb [key=value ...]")
    rest = argv[argv.index("--") + 1:]
    if len(rest) < 2:
        raise SystemExit("need IN.obj and OUT.glb")
    opts = {}
    for kv in rest[2:]:
        k, _, v = kv.partition("=")
        opts[k] = v
    return rest[0], rest[1], opts


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_obj(path):
    # Blender's importer turns the file's Y-up/+Z-front into Blender Z-up/-Y-front, so
    # glTF export (which does the inverse) gives the Mixamo frame back unchanged.
    bpy.ops.wm.obj_import(filepath=path, forward_axis="NEGATIVE_Z", up_axis="Y")
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        raise SystemExit("no mesh imported")
    if len(meshes) > 1:
        bpy.ops.object.select_all(action="DESELECT")
        for o in meshes:
            o.select_set(True)
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.join()
        meshes = [bpy.context.view_layer.objects.active]
    ob = meshes[0]
    ob.name = "Body"
    # Bake any import transform so vertex coordinates are the file's.
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return ob


def to_gltf(v):
    """Blender (x, y, z) -> Y-up (x, z, -y)."""
    return (v.x, v.z, -v.y)


def to_blender(p):
    """Y-up (x, y, z) -> Blender (x, -z, y)."""
    return Vector((p[0], -p[2], p[1]))


def landmarks(ob, opts):
    pts = [to_gltf(v.co) for v in ob.data.vertices]
    ys = [p[1] for p in pts]
    H = max(ys)
    hand_x = max(abs(p[0]) for p in pts)

    def band(lo, hi, pred=lambda p: True):
        return [p for p in pts if lo <= p[1] < hi and pred(p)]

    arm = [p for p in pts if abs(p[0]) > 0.5 * hand_x]
    shoulder_y = sorted(p[1] for p in arm)[len(arm) // 2] if arm else 0.72 * H
    below = band(shoulder_y - 0.10 * H, shoulder_y - 0.05 * H, lambda p: abs(p[0]) < 0.5 * hand_x)
    shoulder_x = (max(abs(p[0]) for p in below) if below else 0.1 * H) + 0.012 * H
    torso = band(0.55 * H, 0.70 * H, lambda p: abs(p[0]) < shoulder_x)
    center_z = sum(p[2] for p in torso) / len(torso) if torso else 0.0
    hips_y = 0.47 * H
    ankle_y = 0.045 * H
    knee_y = 0.5 * (hips_y + ankle_y)
    shin = band(0.10 * H, 0.20 * H, lambda p: abs(p[0]) > 0.1)
    leg_x = sum(abs(p[0]) for p in shin) / len(shin) if shin else 0.05 * H
    foot = band(0.0, 0.05 * H)
    toe_z = max(p[2] for p in foot) if foot else center_z + 0.1 * H
    wrist_x = hand_x - 0.07 * H
    elbow_x = 0.5 * (shoulder_x + wrist_x)
    neck_y = shoulder_y + 0.07 * H
    head_y = shoulder_y + 0.12 * H

    def mean_z(sel, fallback):
        return sum(p[2] for p in sel) / len(sel) if sel else fallback

    # Each joint sits on its own segment's axis. A joint on the torso's centre
    # line left the sleeve, which is modelled behind that line, hanging behind
    # its pivot once swung down, and the hand, modelled ahead of it, in front:
    # Harry's slight elbow bend became a hunch.
    sleeve = lambda lo, hi: [p for p in pts if lo < abs(p[0]) < hi and abs(p[1] - shoulder_y) < 0.08 * H]
    arm_z = mean_z(sleeve(shoulder_x + 0.03 * H, shoulder_x + 0.11 * H), center_z)
    elbow_z = mean_z(sleeve(elbow_x - 0.03 * H, elbow_x + 0.03 * H), arm_z)
    wrist_z = mean_z(sleeve(wrist_x - 0.03 * H, wrist_x + 0.01 * H), elbow_z)
    hand_z = mean_z([p for p in pts if abs(p[0]) > wrist_x], wrist_z)
    legs = lambda lo, hi: [p for p in pts if lo <= p[1] < hi and 0.2 * leg_x < abs(p[0]) < 3.0 * leg_x]
    upleg_z = mean_z(legs(hips_y - 0.09 * H, hips_y), center_z)
    knee_z = mean_z(legs(knee_y - 0.045 * H, knee_y + 0.045 * H), upleg_z)
    ankle_z = mean_z(legs(ankle_y - 0.012 * H, ankle_y + 0.035 * H), knee_z)
    head_z = mean_z([p for p in pts if p[1] > head_y], center_z)
    neck_z = mean_z([p for p in pts if neck_y - 0.015 * H < p[1] < head_y and abs(p[0]) < 0.6 * shoulder_x], center_z)
    lm = {
        "H": H,
        "hand_x": hand_x,
        "shoulder_y": shoulder_y,
        "shoulder_x": shoulder_x,
        "wrist_x": wrist_x,
        "neck_y": neck_y,
        "head_y": head_y,
        "hips_y": hips_y,
        "knee_y": knee_y,
        "ankle_y": ankle_y,
        "leg_x": leg_x,
        "center_z": center_z,
        "toe_z": toe_z,
        "arm_z": arm_z,
        "elbow_z": elbow_z,
        "wrist_z": wrist_z,
        "hand_z": hand_z,
        "upleg_z": upleg_z,
        "knee_z": knee_z,
        "ankle_z": ankle_z,
        "head_z": head_z,
        "neck_z": neck_z,
    }
    for k in list(lm):
        if k in opts:
            lm[k] = float(opts[k])
    return lm


def build_joints(lm):
    """Mixamo joint positions in the Y-up frame: name -> (parent, head)."""
    cz = lm["center_z"]
    H = lm["H"]
    sy, sx = lm["shoulder_y"], lm["shoulder_x"]
    wx = lm["wrist_x"]
    ex = 0.5 * (sx + wx)
    hy, ky, ay, lx = lm["hips_y"], lm["knee_y"], lm["ankle_y"], lm["leg_x"]
    az, ez, wz, hz = lm["arm_z"], lm["elbow_z"], lm["wrist_z"], lm["hand_z"]
    uz, kz, akz = lm["upleg_z"], lm["knee_z"], lm["ankle_z"]
    J = {}
    J["Hips"] = (None, (0, hy, uz))
    J["Spine"] = ("Hips", (0, hy + 0.07 * H, cz))
    J["Spine1"] = ("Spine", (0, hy + 0.14 * H, cz))
    J["Spine2"] = ("Spine1", (0, hy + 0.21 * H, cz))
    J["Neck"] = ("Spine2", (0, lm["neck_y"], lm["neck_z"]))
    J["Head"] = ("Neck", (0, lm["head_y"], lm["head_z"]))
    J["HeadTop_End"] = ("Head", (0, H, lm["head_z"]))
    for side, s in (("Left", 1.0), ("Right", -1.0)):
        J[side + "Shoulder"] = ("Spine2", (s * 0.3 * sx, sy + 0.01 * H, 0.5 * (cz + az)))
        J[side + "Arm"] = (side + "Shoulder", (s * sx, sy, az))
        J[side + "ForeArm"] = (side + "Arm", (s * ex, sy, ez))
        J[side + "Hand"] = (side + "ForeArm", (s * wx, sy, wz))
        J[side + "HandEnd"] = (side + "Hand", (s * lm["hand_x"], sy, hz))
        J[side + "UpLeg"] = ("Hips", (s * lx, hy, uz))
        J[side + "Leg"] = (side + "UpLeg", (s * lx, ky, kz))
        J[side + "Foot"] = (side + "Leg", (s * lx, ay, akz))
        J[side + "ToeBase"] = (side + "Foot", (s * lx, 0.3 * ay, akz + 0.6 * (lm["toe_z"] - akz)))
        J[side + "Toe_End"] = (side + "ToeBase", (s * lx, 0.1 * ay, lm["toe_z"]))
    return J


def build_armature(J):
    arm_data = bpy.data.armatures.new("Armature")
    arm = bpy.data.objects.new("Armature", arm_data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    children = {}
    for name, (parent, _) in J.items():
        children.setdefault(parent, []).append(name)
    bones = {}
    order = ["Hips"]
    i = 0
    while i < len(order):
        order.extend(children.get(order[i], []))
        i += 1
    for name in order:
        parent, head = J[name]
        eb = arm_data.edit_bones.new(name)
        eb.head = to_blender(head)
        kids = children.get(name, [])
        if kids:
            # Tail toward the first child; a leaf points on along its parent.
            eb.tail = to_blender(J[kids[0]][1])
        else:
            d = eb.head - to_blender(J[parent][1])
            eb.tail = eb.head + (d.normalized() * max(d.length * 0.5, 0.5) if d.length > 1e-6 else Vector((0, 0, 0.5)))
        if (eb.tail - eb.head).length < 1e-4:
            eb.tail = eb.head + Vector((0, 0, 0.5))
        if parent:
            eb.parent = bones[parent]
        bones[name] = eb
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm


def seg_dist(p, a, b):
    ab = b - a
    t = 0.0 if ab.length_squared < 1e-9 else max(0.0, min(1.0, (p - a).dot(ab) / ab.length_squared))
    return (p - (a + ab * t)).length


def weight_mesh(ob, arm, J, lm):
    """Inverse-distance^4 to the nearest three allowed bone segments, with region gates."""
    H = lm["H"]
    segs = {}
    for name, (parent, head) in J.items():
        if name.endswith("_End") or name.endswith("HandEnd"):
            continue
        kids = [n for n, (p, _) in J.items() if p == name]
        tail = J[kids[0]][1] if kids else head
        segs[name] = (Vector(head), Vector(tail))
    torso = ["Hips", "Spine", "Spine1", "Spine2", "Neck"]
    head_b = ["Neck", "Head"]
    arms = {s: [s + "Shoulder", s + "Arm", s + "ForeArm", s + "Hand"] for s in ("Left", "Right")}
    legs = {s: [s + "UpLeg", s + "Leg", s + "Foot", s + "ToeBase"] for s in ("Left", "Right")}
    groups = {name: ob.vertex_groups.new(name=name) for name in segs}
    sy, sx, wx = lm["shoulder_y"], lm["shoulder_x"], lm["wrist_x"]
    hy, ay = lm["hips_y"], lm["ankle_y"]
    for v in ob.data.vertices:
        p = Vector(to_gltf(v.co))
        side = "Left" if p.x >= 0 else "Right"
        allowed = list(torso)
        # The shoulder tops and a sailor collar reach above the neck joint:
        # only the narrow column above it is head, or the whole yoke would
        # turn with the head. The clavicles get no vertices: a T-pose cap
        # held by the clavicle stays up when the sleeve swings down, and the
        # seam between them stretched into a spike. The cap rides the chest
        # and blends into the arm across the sleeve top instead.
        if p.y > lm["head_y"] or (p.y > lm["neck_y"] - 0.015 * H and abs(p.x) < 0.5 * sx):
            allowed = list(head_b)
        # Past the cap the band widens to the sleeve's full thickness: a sleeve
        # top left to the chest there stayed up in the T-pose as a spike. The
        # narrow band near the body keeps the collar tips with the chest.
        if (abs(p.x) > 0.8 * sx and abs(p.y - sy) < 0.06 * H) or \
           (abs(p.x) > sx + 0.06 * H and abs(p.y - sy) < 0.10 * H):
            allowed = [n for n in arms[side] if not n.endswith("Shoulder")] + ["Spine2"]
        if abs(p.x) > wx + 0.005 * H:
            allowed = [side + "Hand"]
        if p.y < hy + 0.04 * H:
            allowed = legs[side] + ["Hips"]
        if p.y < ay * 0.7 and p.z > lm["center_z"] + 0.4 * (lm["toe_z"] - lm["center_z"]):
            allowed = [side + "ToeBase", side + "Foot"]
        cands = []
        for name in allowed:
            a, b = segs[name]
            d = max(seg_dist(p, a, b), 0.004 * H)
            cands.append((1.0 / d ** 6, name))
        cands.sort(reverse=True)
        cands = cands[:3]
        total = sum(w for w, _ in cands)
        weights = [(w / total, name) for w, name in cands]
        # Across the shoulder cap the chest share fades into the upper arm by
        # width, not by distance: a hard edge there left the cap standing in
        # the T-pose while the sleeve top swung down, a spike at each shoulder.
        cap_in, cap_out = 0.8 * sx, sx + 0.06 * H
        if cap_in <= abs(p.x) < cap_out and abs(p.y - sy) < 0.06 * H:
            t = (abs(p.x) - cap_in) / (cap_out - cap_in)
            t = t * t * (3.0 - 2.0 * t)
            weights = [(t, side + "Arm"), (1.0 - t, "Spine2")]
        for w, name in weights:
            if w > 0.02:
                groups[name].add([v.index], w, "REPLACE")
    mod = ob.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    ob.parent = arm


def fix_textures(ob, src_dir, out_dir):
    """Convert every image to PNG and point the materials at the copies.

    Viewer exports name textures by the format they claim rather than the bytes
    (a PNG called Binary_0.jpeg), and Blender then refuses to load them, so the
    file is copied under its real extension first."""
    for mat in ob.data.materials:
        if not mat or not mat.use_nodes:
            continue
        for node in mat.node_tree.nodes:
            if node.type != "TEX_IMAGE" or node.image is None:
                continue
            img = node.image
            base = os.path.basename(bpy.path.abspath(img.filepath) if img.filepath else img.name)
            src = os.path.join(src_dir, base)
            if not os.path.isfile(src):
                raise SystemExit("texture not found: " + src)
            with open(src, "rb") as f:
                head = f.read(8)
            ext = ".png" if head.startswith(b"\x89PNG") else (".jpg" if head[:2] == b"\xff\xd8" else os.path.splitext(base)[1])
            stem = os.path.splitext(base)[0]
            png = os.path.join(out_dir, stem + ".png")
            if ext == ".png":
                with open(src, "rb") as f, open(png, "wb") as g:
                    g.write(f.read())
            else:
                fixed = os.path.join(out_dir, stem + "_src" + ext)
                with open(src, "rb") as f, open(fixed, "wb") as g:
                    g.write(f.read())
                loaded = bpy.data.images.load(fixed, check_existing=False)
                # Loading is lazy; retargeting the path before the pixels are read
                # makes save() look for them at the new path and fail.
                loaded.pixels[0]
                loaded.filepath_raw = png
                loaded.file_format = "PNG"
                loaded.save()
            node.image = bpy.data.images.load(png, check_existing=False)
        for node in mat.node_tree.nodes:
            if node.type == "BSDF_PRINCIPLED":
                node.inputs["Metallic"].default_value = 0.0
        # Alpha cut-outs are decided by the game shader; keep the material opaque here.
        mat.blend_method = "OPAQUE"


def export_glb(ob, arm, path):
    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.export_scene.gltf(
        filepath=path,
        export_format="GLB",
        use_selection=True,
        export_apply=False,
        export_yup=True,
        export_texcoords=True,
        export_normals=True,
        export_tangents=False,
        export_materials="EXPORT",
        export_image_format="AUTO",
        export_skins=True,
        export_all_influences=False,
        export_def_bones=False,
        export_animations=False,
        export_morph=False,
        export_lights=False,
        export_cameras=False,
    )


def render_previews(ob, arm, lm, out_dir):
    """Workbench renders: rest T-pose, then arms down with a bent elbow and knee."""
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x = 900
    scene.render.resolution_y = 900
    scene.render.film_transparent = False
    cam_data = bpy.data.cameras.new("Cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = lm["H"] * 1.15
    cam = bpy.data.objects.new("Cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    mid = Vector((0, 0, lm["H"] * 0.5))

    def shoot(name, yaw_deg):
        yaw = math.radians(yaw_deg)
        cam.location = mid + Vector((math.sin(yaw) * 200, -math.cos(yaw) * 200, 0))
        cam.rotation_euler = (math.radians(90), 0, yaw)
        scene.render.filepath = os.path.join(out_dir, name + ".png")
        bpy.ops.render.render(write_still=True)

    shoot("rest_front", 0)
    shoot("rest_side", 90)

    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="POSE")
    pb = arm.pose.bones

    def rot(name, axis, deg):
        pb[name].rotation_mode = "QUATERNION"
        pb[name].rotation_quaternion = Quaternion(axis, math.radians(deg))

    # Arms down to the sides: the bone's local roll is unknown, so turn about the
    # world forward axis through the joint by using the armature's frame.
    for s, sign in (("Left", 1), ("Right", -1)):
        pb[s + "Arm"].rotation_mode = "QUATERNION"
        pb[s + "Arm"].rotation_quaternion = Quaternion((0, 0, 1), 0)
        bone = arm.data.bones[s + "Arm"]
        q = Quaternion(Vector((0, 1, 0)), math.radians(70 * sign))
        pb[s + "Arm"].rotation_quaternion = (bone.matrix_local.to_3x3().inverted() @ q.to_matrix() @ bone.matrix_local.to_3x3()).to_quaternion()
    bone = arm.data.bones["LeftForeArm"]
    q = Quaternion(Vector((0, 1, 0)), math.radians(60))
    pb["LeftForeArm"].rotation_mode = "QUATERNION"
    pb["LeftForeArm"].rotation_quaternion = (bone.matrix_local.to_3x3().inverted() @ q.to_matrix() @ bone.matrix_local.to_3x3()).to_quaternion()
    bone = arm.data.bones["RightLeg"]
    q = Quaternion(Vector((1, 0, 0)), math.radians(-50))
    pb["RightLeg"].rotation_mode = "QUATERNION"
    pb["RightLeg"].rotation_quaternion = (bone.matrix_local.to_3x3().inverted() @ q.to_matrix() @ bone.matrix_local.to_3x3()).to_quaternion()
    bone = arm.data.bones["RightUpLeg"]
    q = Quaternion(Vector((1, 0, 0)), math.radians(35))
    pb["RightUpLeg"].rotation_mode = "QUATERNION"
    pb["RightUpLeg"].rotation_quaternion = (bone.matrix_local.to_3x3().inverted() @ q.to_matrix() @ bone.matrix_local.to_3x3()).to_quaternion()
    bone = arm.data.bones["Head"]
    q = Quaternion(Vector((0, 0, 1)), math.radians(35))
    pb["Head"].rotation_mode = "QUATERNION"
    pb["Head"].rotation_quaternion = (bone.matrix_local.to_3x3().inverted() @ q.to_matrix() @ bone.matrix_local.to_3x3()).to_quaternion()
    bpy.ops.object.mode_set(mode="OBJECT")
    shoot("bent_front", 0)
    shoot("bent_side", 90)
    # Back to rest before export.
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="POSE")
    bpy.ops.pose.select_all(action="SELECT")
    bpy.ops.pose.transforms_clear()
    bpy.ops.object.mode_set(mode="OBJECT")


def main():
    in_path, out_path, opts = parse_args()
    clear_scene()
    ob = import_obj(os.path.abspath(in_path))
    lm = landmarks(ob, opts)
    print("[rig] landmarks: " + ", ".join("%s=%.2f" % (k, v) for k, v in sorted(lm.items())))
    J = build_joints(lm)
    arm = build_armature(J)
    weight_mesh(ob, arm, J, lm)
    out_dir = os.path.dirname(os.path.abspath(out_path))
    fix_textures(ob, os.path.dirname(os.path.abspath(in_path)), out_dir)
    if "render" in opts:
        render_previews(ob, arm, lm, opts["render"])
    export_glb(ob, arm, os.path.abspath(out_path))
    print("[rig] wrote " + out_path)


main()
