# Character face animation models

`gamedata/builtin/CHARA/<NAME>.glb` (shipped from `pc_port/assets/`) gives the
game's own human characters blinking, lip sync and breathing when
**Character Face Animation** is on. Each file holds only the character's own
chest, neck and head parts, in game units at the rest pose (ANM keyframe 0),
marked with an `SH_NATIVE` node, so the rest of the body stays the stock model.
A replacement in `gamedata/load/CHARA/<NAME>.glb` always wins over these.

To rebuild one (from `pc_port/`, with the disc extracted):

```
G="build/gamedata/Silent Hill (USA)_extracted_00000"
python tools/ilm_obj.py export "$G/CHARA/LISA.ILM" -o LISA.obj --anm "$G/ANIM/LS.ANM" --keyframe 0
python tools/chara_face/gen_rest.py LISA.ilmmeta.json LISA.obj LISA > rest_LISA.txt
python tools/chara_face/npc2glb.py LISA LISA.obj "$G/CHARA/LISA.TIM" rest_LISA.txt LISA.glb tools/chara_face/faces.json
```

The rest table `gen_rest.py` prints must match the one in `src/pc_skin_chara.c`
for that character. Monster Cybil (MSB) is exported with `--anm SBL.ANM`, as
her own MSB.ANM has no standing frame.

`faces.json` holds, per character and in the head bone's frame (+y up, +z out
of the face, OBJ units): `eyes` [[x, y, half width, half height], ...],
`mouth` [x, y, half width] and `open` (how far the mouth opens). To find
them, run npc2glb with `HEADLOCAL=1` and render the result face-on.

Characters with semi-transparent face parts (the ghost doctor, ghost Alessa,
the Incubator) are left stock: npc2glb refuses them.
