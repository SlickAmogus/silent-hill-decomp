# RetroAchievements support

The Linux PC port can optionally load the official `rcheevos` runtime and connect to RetroAchievements without adding a mandatory build dependency.

## Current status

This is an initial, **softcore-only** integration. It provides:

- login with a RetroAchievements web API token
- PlayStation disc identification through `rcheevos`
- achievement and leaderboard processing against the port's 2 MB emulated PSX RAM
- unlock, completion, disconnect, and reconnect messages in `SilentHill.log`
- no password storage and no token storage in `config.cfg`

Hardcore mode is intentionally disabled. The PC port includes quick save/load, debug controls, alternate cameras, randomizer, modified gameplay options, and other features that must be audited before the client can claim hardcore compliance.

## Requirements

Install or build:

- a shared `rcheevos` library named `librcheevos.so` or `librcheevos.so.0`, built with hash support
- libcurl (`libcurl.so.4` on most Linux distributions)

The normal game build remains unchanged when these libraries are absent.

## Configuration

Get your web API key from your RetroAchievements account settings. Export the following variables before launching the game:

```bash
export SH_RA_USERNAME="your_username"
export SH_RA_TOKEN="your_web_api_key"
export SH_RA_DISC="$HOME/Games/SilentHillPC/gamedata/Silent Hill (USA).bin"
./SilentHillPC
```

For Steam or another launcher, place the variables in the launch command or wrapper script. Do not publish or commit your token.

The integration remains inactive unless all three variables are present. `SH_RA_DISC` must point to the exact disc image used by the game so `rcheevos` can calculate the supported RetroAchievements hash.

## Troubleshooting

Enable the game's debug log and inspect `SilentHill.log`. RetroAchievements messages begin with `[RA]`.

Common messages:

- `librcheevos and libcurl are required`: one of the runtime libraries could not be loaded
- `SH_RA_DISC does not point to a readable file`: the path is incorrect or inaccessible
- `Login failed`: the username or token is invalid
- `Game identification failed`: the disc dump does not match a hash registered with RetroAchievements, or the library lacks hash support

## Security

The integration reads credentials only from the process environment. It does not log the token, save it to disk, or accept an account password.

Environment variables can still be visible to processes running as the same operating-system user. A future launcher integration should store tokens through a desktop keyring rather than plain configuration files.

## Implementation notes

`pc_retroachievements_linux.cpp` is discovered automatically by the existing recursive PC-port source glob. It uses runtime symbol loading so the core executable does not gain mandatory `rcheevos` or libcurl link dependencies.

Achievement evaluation runs at approximately 30 Hz and reads addresses `0x000000` through `0x1FFFFF` directly from `g_PsxRam`, matching the port's emulated PlayStation RAM layout.
