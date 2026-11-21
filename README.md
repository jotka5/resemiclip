# [ReSemiclip](https://github.com/rehlds/ReSemiclip)
[![GitHub license](https://img.shields.io/github/license/jotka5/ReSemiclip.svg?longCache=true&style=flat-square)](https://github.com/jotka5/ReSemiclip/blob/master/LICENSE)
[![GitHub issues](https://img.shields.io/github/issues/jotka5/ReSemiclip.svg?longCache=true&style=flat-square)](https://github.com/jotka5/ReSemiclip/issues)
[![GitHub forks](https://img.shields.io/github/forks/jotka5/ReSemiclip.svg?longCache=true&style=flat-square)](https://github.com/jotka5/ReSemiclip/network)
[![GitHub stars](https://img.shields.io/github/stars/jotka5/ReSemiclip.svg?longCache=true&style=flat-square)](https://github.com/jotka5/ReSemiclip/stargazers)

ReSemiclip is a Metamod plugin for **Counter-Strike 1.6** on **ReHLDS** that removes
collision between players and makes them semi-transparent when they are close to each
other. Teammates can walk through each other instead of pushing and blocking.

This repository is a fork of [rehlds/ReSemiclip](https://github.com/rehlds/ReSemiclip).
It keeps the original behaviour and adds the improvements listed in
[What's new in this fork](#whats-new-in-this-fork).

## Features

- Players no longer block each other while semiclip is active.
- Distance based transparency (`effects 1`) or a fixed transparency value (`effects 0`).
- Works for everyone, for one team, or for teammates only (`team`).
- Optional time window from the start of the round (`time`).
- Crouch handling: you can jump on a crouching teammate (`crouch`).
- Bullets pass through semi-transparent teammates (`penetfire`).
- Per-map configuration overrides (`addons/resemiclip/maps/<map>.ini` and
  `maps/prefix_<prefix>.ini`).
- **Invisibility set by other plugins is preserved** (`invisible`).
- **Follow entities (dance copies, hats) fade smoothly instead of disappearing**
  and no longer show their placeholder geometry.

## Requirements

- [ReHLDS](https://github.com/rehlds/ReHLDS)
- [ReGameDLL_CS](https://github.com/rehlds/ReGameDLL_CS)
- [ReAPI](https://github.com/rehlds/ReAPI)
- Metamod-R (or any Metamod build compatible with ReHLDS)

## Installation

### Linux

```
cstrike/addons/resemiclip/resemiclip_mm_i386.so
cstrike/addons/resemiclip/config.ini
```

Add to `cstrike/addons/metamod/plugins.ini`:

```
linux addons/resemiclip/resemiclip_mm_i386.so
```

### Windows

```
cstrike/addons/resemiclip/resemiclip_mm.dll
cstrike/addons/resemiclip/config.ini
```

Add to `cstrike/addons/metamod/plugins.ini`:

```
win32 addons/resemiclip/resemiclip_mm.dll
```

## Configuration

The configuration file is `addons/resemiclip/config.ini`. It is re-read on every map
change, so a server restart is not required when you edit it. Every value can also be
changed at runtime with the `semiclip_option` console command:

```
semiclip_option <option> <value>
semiclip_option transparency 150
```

| Option | Values | Default | Description |
| --- | --- | --- | --- |
| `semiclip` | `0` \| `1` | `1` | Enable or disable semiclip. |
| `team` | `0` \| `1` \| `2` \| `3` | `3` | Who semiclip applies to: `0` everyone, `1` Terrorists only, `2` Counter-Terrorists only, `3` teammates only. |
| `time` | `0`..`180` | `0` | How long semiclip works from the start of the round, in seconds. `0` means the whole round. |
| `crouch` | `0` \| `1` | `0` | Allows jumping onto a crouching player. |
| `effects` | `0` \| `1` | `0` | `1`: transparency depends on the distance between players. `0`: fixed transparency. |
| `distance` | `64`..`250` | `200` | Distance at which transparency and semiclip start to work. |
| `transparency` | `0`..`255` | `120` | Fixed transparency (`effects 0`). `0` disables transparency, semiclip still works. |
| `penetfire` | `0` \| `1` | `0` | Allow bullets to pass through semi-transparent teammates. |
| `invisible` | `0` \| `1` \| `2` | `1` | How to treat rendering set by other plugins. See below. |
| `debug` | `0` \| `1` | `0` | Log follow entity render states to the server console (diagnostics only). |

### `invisible`

Semiclip constantly rewrites `rendermode` / `renderamt` of the entities it sends to
clients. That used to break invisibility implemented by other plugins (dance menus,
admin invisibility, custom effects): an invisible player became semi-transparent
instead of staying invisible.

| Value | Behaviour |
| --- | --- |
| `0` | Legacy behaviour. Semiclip always overrides rendering. Invisible players become semi-transparent. |
| `1` | Default. Fully invisible players stay invisible. A player counts as invisible when the entity has `EF_NODRAW`, or `renderamt == 0` with a transparent render mode (`TransColor`, `TransTexture`, `TransAlpha`, `TransAdd`). `kRenderGlow` is intentionally not treated as invisibility, so glow effects keep working. As soon as the visibility is restored, the normal distance based transparency resumes. |
| `2` | Keep any custom render state. If another plugin sets a `rendermode` other than `kRenderNormal`, semiclip does not touch that entity at all. The most polite mode, but a plugin that leaves a stale render mode will disable semiclip for that player. |

In all modes the entity still receives `solid = SOLID_NOT`, so semiclip keeps working
for invisible players — only the rendering is left alone.

### `debug`

Diagnostics, does not change gameplay.

- `0` — silent (default).
- `1` — for every processed follow entity (dance copy, hat) a `[SEMICLIP-DBG]` line is
  printed to the server console: indices of host/follow/aiment/owner, computed distance
  and alpha, the values actually sent to the client (`net_rm`, `net_amt`), and the
  original `rendermode` / `renderamt` / `effects` of the entities involved. Logging is
  throttled to avoid flooding.

## Follow entities (dance copies, hats)

Plugins that create a "copy" of a player for animation purposes (for example a dance
menu) attach the copy with `MOVETYPE_FOLLOW` and `aiment` pointing to another entity
that carries the skeleton. Hats usually follow the copy.

GoldSrc draws a follow entity in the transparent pass **only if its `aiment` is also in
the transparent list** (see `R_DrawTEntitiesOnList` in the engine). If the copy is made
transparent while `aiment` stays opaque, the client skips it completely — the copy
vanishes instead of fading.

ReSemiclip handles this:

1. For non-player follow entities it uses `kRenderTransTexture` (real alpha blending).
   `kRenderTransAlpha` is an alpha-test mode for generic studio models and makes them
   pop out of existence.
2. It puts the `aiment` into the transparent list too, with `renderamt = 0`. The client
   finds the `aiment` by index and borrows its bones, then draws the copy with the copy's
   own alpha. The `aiment` itself is skipped in the drawing pass (`r_blend <= 0`), so its
   placeholder geometry is never visible.
3. The transmitted `renderamt` is clamped to `1..255`: the network field is a single
   byte, and values above `255` used to wrap to `0` (fully invisible).

As a result, the dance copy fades smoothly with distance, exactly like a normal player
model, and never disappears completely.

## Per-map configuration

Semiclip reads additional files on map start:

- `addons/resemiclip/maps/prefix_<prefix>.ini` — for example `prefix_de.ini` for every
  `de_*` map, applied first;
- `addons/resemiclip/maps/<mapname>.ini` — for example `maps_de_dust2.ini`, applied
  last and overrides the prefix file.

Both files use the same `key = value` format as `config.ini` and only need the options
you want to override.

## Building

### Linux

```bash
CC=gcc CXX=g++ cmake -B build
cmake --build build -j
```

The result is `build/resemiclip_mm_i386.so` (32-bit is required by `hlds_linux`).

### Windows

Open `msvc/resemiclip.sln` and build the `Release|Win32` configuration, or run:

```
msbuild msvc/resemiclip.sln /p:Configuration=Release /p:Platform=Win32
```

The result is `msvc/Release/resemiclip_mm.dll`.

## What's new in this fork

### 2.4.23 — `aimenthide`

- Added `invisible` option (`0` legacy, `1` keep fully invisible, `2` keep any custom
  render). Invisible players stay invisible; distance transparency resumes when the
  invisibility ends.
- Fixed follow entities (dance copies, hats) disappearing completely near another
  player: the entity's `aiment` is now mirrored into the transparent list.
- The `aiment` is sent with `renderamt = 0`, so the additive placeholder geometry of
  animation models is no longer visible.
- Non-player follow models use `kRenderTransTexture` instead of `kRenderTransAlpha`,
  which gives real alpha blending instead of alpha test.
- Transmitted `renderamt` is clamped to `1..255` to avoid byte wrap.
- Follow entities of dead players are ignored.
- Added `debug` option for follow entity render state logging.

## License

GPL-3.0, see [LICENSE](LICENSE). Based on
[rehlds/ReSemiclip](https://github.com/rehlds/ReSemiclip) by the ReHLDS Dev Team.
