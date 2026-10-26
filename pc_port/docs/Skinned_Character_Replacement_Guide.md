# Skinned Character Replacements (glTF)

Replace a character with any rigged, skinned model, such as a remake model or a
Mixamo-rigged character, by dropping one `.glb` file into the game data. The game drives
your model from its own skeleton every frame:

- joints bend smoothly through your skin weights
- every material keeps its full-resolution texture
- surfaces hide each other per pixel

There is no cutting into parts, no fitting in Blender and no converter step.

This replaces the older rigid-part workflow in [Model_Modding_Guide.md](Model_Modding_Guide.md)
for whole-character swaps. Use that guide only when you want to edit the original PSX model.

**Supported characters:**

| Character | File |
| --- | --- |
| Harry | `gamedata/load/CHARA/HERO.glb` |
| Cybil | `gamedata/load/CHARA/SIBYL.glb` |

The steps below use Harry's `HERO`. For Cybil, use `SIBYL` in every file name. The game
spells her model file that way. The same file is used in her ending scene.

## Quick start

1. Get a rigged humanoid model (see [Getting a rig](#getting-a-rig)). Mixamo, Blender,
   XPS, Unreal, 3ds Max Biped and VRoid bone names all work.
2. In Blender, **File > Export > glTF 2.0**, format **glTF Binary (.glb)**, with the
   settings under [Exporting](#exporting).
3. Save it as `gamedata/load/CHARA/HERO.glb`.
4. Make sure `allow_loose_files = 1` is in `config.cfg`.
5. For a female character, also create `gamedata/load/CHARA/HERO.ini` containing
   `female=1` (see [Female characters](#female-characters)).
6. Start the game. Harry is now your model.

To go back to the original Harry, delete or rename `HERO.glb`. The replacement takes
priority over any `HERO.ILM` in the same folder, so you do not need to remove that.

## Extra Harry models in Play As

You can keep the original Harry and add other models as characters to switch to. Any
file in `gamedata/load/CHARA` whose name starts with `HERO` becomes its own entry in the
Play As list, named after what follows `HERO`:

| File | Play As name |
| --- | --- |
| `HERO_CJ.glb` | CJ |
| `HEROCJ.glb` | CJ |
| `HERO_Heather.glb` | HEATHER |
| `HERO.glb` | Harry (replaces him, as above) |

Pick one the same way as any other Play As character: the console command
`playas cj`, the Play As cheat, or `player_character = cj` in `config.cfg`. The model
plays on Harry's own animations, exactly like a `HERO.glb` replacement, and takes its
settings from an `.ini` with the same name, such as `HERO_CJ.ini`.

If the file for the selected character is deleted later, the game falls back to
`HERO.glb` if there is one, otherwise to the original Harry. Load mods installed through
the Mod Manager land in the same folder, so a mod that ships `HERO_CJ.glb` shows up too.
The list is read when the game starts.

## What the model needs

| Requirement | Details |
|---|---|
| Format | glTF 2.0 with **one skin**. Use `.glb`. A `.gltf` with its `.bin` and textures next to it also works. |
| Skeleton | Humanoid, with the bones named in a common convention (see [Bones the game uses](#bones-the-game-uses)). The `mixamorig:` prefix and Sketchfab `_01` style suffixes are fine. |
| Pose | Any bind pose: T-pose, A-pose or arms down. The game converts it. |
| Facing | glTF standard: Y up, facing +Z. This is what Blender exports by default. |
| Size | Any. The model is scaled so its **shoulders** sit at Harry's shoulder height, feet on the floor. |
| Weights | Up to 4 bones per vertex, as glTF allows. |
| Textures | **PNG**: base colour, plus an optional normal map (see [Lighting and normal maps](#lighting-and-normal-maps)). JPEG is not decoded. Embed them in the `.glb`. |
| Polygons | Up to about 260,000 vertices per material. Every vertex is skinned each frame, so a model of 20,000 to 50,000 triangles keeps the frame rate comfortable. |

### Bones the game uses

Your rig must have at least these joints. Fingers, toes, extra spine joints, twist
bones and anything else are fine: they follow the nearest bone listed here.

| Game bone | Mixamo joint(s) |
|---|---|
| Pelvis | `Hips` |
| Chest | `Spine`, `Spine1`, `Spine2` |
| Head | `Neck`, `Head` |
| Clavicles | `LeftShoulder`, `RightShoulder` |
| Upper arms | `LeftArm`, `RightArm` |
| Forearms | `LeftForeArm`, `RightForeArm` |
| Hands | `LeftHand`, `RightHand` (and all finger joints) |
| Thighs | `LeftUpLeg`, `RightUpLeg` |
| Shins | `LeftLeg`, `RightLeg` |
| Feet | `LeftFoot`, `RightFoot`, `LeftToeBase`, `RightToeBase` |

Left and right are the **character's own** left and right, as Mixamo names them.

**Other naming conventions** work without renaming. A bone is matched by its side
(`L`, `R`, `Left`, `Right` as a separate word, or a `Left`/`Right`/`l`/`r` prefix) and a
body-part word in its name:

| Game bone | Words matched |
|---|---|
| Pelvis | `pelvis`, `hips` |
| Chest | `spine`, `chest`, `torso`, `abdomen` |
| Head | `neck`, `head`, and face joints (`eye`, `jaw`, `face`) |
| Clavicles | `clavicle`, `collar`, `shoulder` |
| Upper arms | `upperarm`, `upper_arm`, `shldr`, or the first `arm` joint below the clavicle |
| Forearms | `forearm`, `lowerarm`, `elbow`, or an `arm` joint below another `arm` joint |
| Hands | `hand`, `wrist`, `palm`, and finger words (`finger`, `thumb`, `index`, ...) |
| Thighs | `thigh`, `upleg`, `upperleg`, or the first `leg` joint |
| Shins | `calf`, `shin`, `knee`, `lowerleg`, or the second `leg` joint |
| Feet | `foot`, `toe`, `ball`, `ankle`, or the third `leg` joint and below |

So `upper_arm.L` (Blender), `Arm.001.L` / `Arm.002.L` (XPS), `upperarm_l` (Unreal),
`Bip01 L UpperArm` (3ds Max) and `J_Bip_L_UpperArm` (VRoid) are all recognised. Joints
that match nothing, such as hair or skirt bones, follow their parent.

## Getting a rig

**Already rigged with Mixamo names** (many Sketchfab and game-rip models): use it as is.

**Not rigged:**
1. Export the model from Blender as FBX or OBJ.
2. Upload it to [Mixamo](https://www.mixamo.com), place the markers and let it auto-rig.
3. Download it as **FBX, T-pose, without skin animation**.
4. Import the FBX into Blender. Check that each piece moves with its bones by rotating a
   few bones in Pose mode, then undo.

**Rigged with other bone names:** usually works as is (see the second table above).
If the log says `no Hips joint` or `no joint drives game bone N`, rename the bones it
missed to the Mixamo names. In Blender, select the armature and rename the bones in the
Outliner. The vertex groups follow automatically.

## Exporting

**File > Export > glTF 2.0**:

- **Format:** glTF Binary (`.glb`)
- **Include:** Selected Objects. Select the armature and every mesh that belongs to the
  character, and nothing else: no lights, no cameras.
- **Data > Mesh:** Apply Modifiers **off**, UVs **on**, Normals **on**, Tangents **on**
  if the model has a normal map. Blender cannot
  export shape keys with Apply Modifiers on, and the Armature modifier does not need
  applying: the skin is exported from the armature either way. If you use another
  modifier, such as Mirror or Subdivision, apply it by hand before adding shape keys.
- **Data > Shape Keys:** **on** if you want blinking (see below)
- **Data > Armature / Skinning:** **on**
- **Animation:** **off**. The game animates the model.
- **Material > Images:** keep PNG. If your textures are DDS, TGA or JPEG, convert
  them to PNG in Blender first: Image Editor > Image > Save As > PNG.

Keep the armature unscaled if you can. Applying scale with **Ctrl+A > All Transforms**
on the armature and the meshes, before exporting, avoids surprises.

## How your model is fitted to Harry

The game never changes your mesh. It works out how to map your skeleton onto Harry's
when the model loads:

- **Arms and legs** bend at Harry's elbows, wrists, hips and knees, so the animation
  bends them in the right place and a held weapon lands in the hand. The upper and
  lower segments are stretched or squashed along their length to match Harry's.
- **Torso, head, clavicles and feet** keep your model's own shape and only turn about
  Harry's pivots, so the neck, waist and feet are not stretched.
- **Shoulders** keep your model's own width. Modern bodies are broader than Harry's,
  so the upper arm stays out at your shoulder and the forearm angles in to reach
  Harry's wrist.
- **Hips and knees** keep your model's own spacing side to side.

### Proportions

Harry's PSX skeleton is not modern anatomy. It has a short neck, a high waist pivot
and a stance with the feet apart. A model that looks right in Blender can look
slightly off in game:

- **Waist too wide or thick:** in Edit mode, select the waist and hip loops and scale
  them in X and Y. Leave the armature alone.
- **Legs look bowed:** Harry's own stance has the feet apart and the left leg slightly
  back, and your model copies it. Slimming the lower legs and turning the feet in
  slightly in Edit mode helps.
- **Arms sink into a bulky coat at the hips:** Harry's hands hang close to his
  thighs. Slim the coat's lower sides and pockets in Edit mode.
- **Arms too long or short:** do not resize the arm bones. The game already matches
  Harry's arm lengths. Reshape the sleeves instead.

Change the mesh, not the bones. Moving joints changes where your model bends, but
not where Harry's animation bends.

## Hair and other cutouts

Texels with alpha below 50% are not drawn, so hair cards, eyelashes and lace need
an **alpha channel in the PNG itself**. Some ripped models paint hair strands on
solid black and rely on a separate opacity map. Those show as black squares in
game until you add the alpha:

1. Open the hair texture in an image editor such as GIMP.
2. Add an alpha channel and make the black background transparent. In GIMP:
   **Colors > Color to Alpha**, colour black.
3. Save it as PNG, then load that PNG in the hair material in Blender. Connect its
   **Alpha** output to the Principled BSDF **Alpha** input, so the exporter keeps it.

The cut is hard-edged. Soft, blended alpha is not supported.

## Lighting and normal maps

The game lights the model with its own lights, the same ones that light Harry. By
default this runs per pixel. Set **Character Lighting** to Per Vertex in the F10 quick
menu (Graphics page 2), or `chara_per_pixel_light = 0` in the config, to light it per
vertex like the stock characters.

Per pixel, a **normal map** in the material adds surface detail such as folds, seams and
pores, most visibly under the flashlight. In Blender, connect the normal map image
(Color Space: Non-Color) through a **Normal Map** node into the Principled BSDF
**Normal** input, and the exporter writes it as the material's normal texture.

- Use OpenGL-style normal maps (green up), which is what Blender and glTF expect. A
  DirectX-style map, common from Unreal and some game rips, has its green channel
  inverted: bumps read as dents. Invert the green channel first.
- Export with **Tangents** on. Without tangents the game builds its own from the UVs,
  which works but can show seams where Blender's would not.
- The vertex normals still decide the overall shading. Check Shade Smooth and Auto
  Smooth before blaming the normal map.

Per pixel, the rest of the glTF material is used too:

- **Metallic and roughness** (the factors, and the metallic-roughness texture with
  roughness in green and metal in blue) give the model highlights from the game's lights,
  the flashlight beam and the hero light. Low roughness is glossy, high is matte. A
  material that sets neither is treated as a matte non-metal.
- **Occlusion** (red channel) darkens creases and folds in the ambient light.
- **Emissive** (texture times factor, and `KHR_materials_emissive_strength`) glows at
  its own brightness, even in the dark and in Per Vertex mode.

In Blender these are the Principled BSDF's Metallic, Roughness and Emission inputs. For
occlusion, use a glTF Material Output node group's Occlusion input.

**Hero Light** (F10 quick menu, Graphics page 2; `hero_light` 0 to 3 in the config,
default 2) adds a soft fill light from above the camera to characters lit per pixel. It
gives a model shape in flat daylight, where the game lights a character with one colour,
and lets its normal map show in dim rooms. Set it to Off for the game's own lighting
only.

**Per-Pixel Lighting** (same page; `per_pixel_lighting = 1`, off by default) does the
same per-pixel lighting for the stock world and the stock characters, and the hero light
then reaches the stock characters too.

## Blinking

Add a shape key that closes both eyes and name it `blink`. `eyeBlink`, `eyesClosed`, or
a left and right pair such as `eyeBlink_L`/`eyeBlink_R` also work. The game blinks it
every 2 to 6 seconds, and the eyes stay shut when the ragdoll takes over. Export with
**Shape Keys** on.

## Lip sync

Add a shape key that opens the jaw and name it `jawOpen` (`mouthOpen` and `viseme_aa`
also work). While a voice line plays, the mouth opens with the loudness of the line.

The game does not record who speaks a line, so the port carries its own list of the
speaker of each voice line, by the speaker's model name (`HERO`, `SIBYL`, `DARIA`, `LISA`,
`KAU` and so on). Only the speaker's mouth moves, whichever model replaces them. Lines
not on the list, mostly short grunts, fall back to the pitch of the voice: a male line
moves Harry's mouth and a female line moves Cybil's.

If a line moves the wrong mouth, correct it in `gamedata/load/XA/speakers.txt`, one line
per voice line:

```
# <voice line number> = <model name>, or - for nobody
118 = KAU
255 = -
```

The voice line number is in `SilentHill.log`: look for the `[XA] Play xaIdx=` line logged
when the line starts.

A rig with a jaw bone needs no sculpting. Turn the jaw open about 10 degrees in pose
mode and save the result as the shape key.

## Cybil's pistol

Cybil's original model has her pistol built into her hand parts. The game keeps drawing
those original parts beside a replacement, so she still holds her gun, along with the
original low-poly hand around it.

## Ragdoll on death

Add `ragdoll = 1` to `config.cfg`. When Harry dies, a skinned replacement goes limp
and falls, pushed along the last hit he took. The original model always plays its
normal death animation.

## Female characters

Harry's own cries, grunts and breathing are raised in pitch for female characters, the
same as when you play as Cybil or Lisa. The game cannot tell from the mesh, so put a
text file named after the model next to it, `gamedata/load/CHARA/HERO.ini`:

```
female=1
```

The amount is the `female_voice_pitch` setting in `config.cfg` (percent, default 140,
`0` turns it off). Cutscene dialogue is recorded audio and keeps Harry's voice.

## Brightness and fog

The model goes through the game's own lighting and fog, but a modern texture is a
clean, evenly lit colour map, while Harry's is darker and has its shading painted in.
A replacement can stand out against the scenery as a result. Two more `HERO.ini`
settings tune it per model:

```
brightness=80
fog=130
```

| Setting | Effect |
|---------|--------|
| `brightness` | Percent of the lit colour (default 100, 0 to 200). Lower it for bright textures. |
| `fog` | Percent of the game's fog on the model (default 100, 0 to 400). Raise it to fade the model into the fog sooner. |

## Flashlight

Harry's flashlight is clipped to the left side of his chest, and the beam starts
there. If your character carries theirs somewhere else, move the beam to match, and
tone it down on a light-coloured outfit that the beam washes out:

```
flashlight=60
flashlight_x=16
```

| Setting | Effect |
|---------|--------|
| `flashlight` | Percent of the beam that lights the model itself (default 100, 0 to 100). |
| `flashlight_x` | Moves the beam origin across the chest, in cm. Positive is toward the character's right; 16 mirrors Harry's spot onto the right side. |
| `flashlight_y` | Moves it up or down, in cm. Negative is up. |
| `flashlight_z` | Moves it forward or back, in cm. Positive is forward. |
| `gun_raise` | Moves a held weapon along the forearm, in mm. Positive is up toward the wrist. |
| `gun_out` | Moves it sideways, in mm. Positive is away from the leg. |
| `gun_twist` | Turns it about the forearm, in degrees, round the middle of the grip. Positive is clockwise seen from the shoulder. |

The game already moves weapons into the model's own closed fist (the log's
`right fist offset` line); these settings fine-tune that.

The game reads `HERO.ini` when it loads the model, so restart after changing it.

## Troubleshooting

`SilentHill.log` has a `[SKIN]` line for everything the loader decides:

| Log line | Meaning |
|---|---|
| `[SKIN] gamedata/load/CHARA/HERO.glb: N verts, M materials, ...` | Loaded. The lines after it list each bone's fitted joint. |
| `no Hips joint` | No bone is named `Hips` or `Pelvis`. Rename the pelvis bone to `Hips`. |
| `no joint drives game bone N` | One of the required joints in the table is missing. |
| `buffers missing or no skin` | Exported without the armature, or the `.bin` is missing next to a `.gltf`. |
| `texture ... did not decode (PNG only)` | Convert that texture to PNG. The material draws white until you do. |
| No `[SKIN]` line at all | `allow_loose_files` is off, or the file is not named after the model (`HERO.glb`). |

## Limitations

- Harry and Cybil only for now. Other characters need their skeleton tables added.
- The hand holding an item closes into a fist, with every finger bent the same way.
  This needs a finger chain of three joints under each hand: the index finger when it is
  named (`LeftHandIndex1` to `3`), otherwise a middle finger. It bends past that knuckle
  in the direction the finger already curls, so a dead straight finger gets no grip.
  The game does not pose individual fingers.
- Shape keys drive blinking and the mouth. A rig that blinks with eyelid bones instead
  does not blink in game.
- The replacement does not cast flashlight shadows yet.
- The ragdoll collides with the floor, not walls.

## Licences

Respect the model's licence. Many Sketchfab models are CC-BY and need the author
credited wherever you share the mod.
