# Task: Flashlight self-shadowing on Harry (glow no longer lights through his body)

Status: OPEN, low priority, maintainer's call. Not currently planned.

## Summary
The flashlight is a light at Harry's chest/hand, so it lights Harry's own body
and the surfaces behind him as if he doesn't block it. The glow reads as
"shining through his midsection." This is essentially faithful to the original
(a light at Harry's position lights Harry too), which is why it happens in every
flashlight mode and with PGXP on or off.

## Investigation (2026-09-04)
Confirmed it is NOT the classic PSX glow sprites. In Modern mode
(`flashlight_mode=2`) the `func_800414E0` lens flare and `func_8003E740` flame
billboard are skipped entirely -- bounded `[GLOWDBG]` probes fired zero times
while the glow was on screen. So the on-screen glow is the per-pixel flashlight
shader lighting Harry's own geometry. In classic modes it's the authentic PSX
glow overlays drawn over him.

A frame capture in map6_s03 (Modern mode, flashlight on) showed the glow with no
`[GLOWDBG]` output, confirming the shader-lighting explanation.

## The actual fix (if we ever want it)
Make the flashlight **self-shadow Harry** so his front blocks the light from
reaching his back and the geometry behind him. The machinery already exists:
flashlight shadow mapping (`flmode 3` / `flashlight_shadows`, see the
`project_flashlight_shadow_mapping` memory). Extending the shadow pass to include
Harry as a caster into his own light is the clean route for Modern mode. Classic
modes would need sprite occlusion instead.

## Do NOT
- Do NOT reintroduce the `SZ_KIND_FLARE` per-pixel occlusion that was prototyped
  and reverted on 2026-09-04. It only affected classic-mode lens-flare sprites
  and did nothing in Modern mode (the mode users actually run), while adding risk
  to the depth pipeline.
- Any change must stay optional / faithful-by-default.
