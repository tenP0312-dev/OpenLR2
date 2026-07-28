# BMS-IR Arena for OpenLR2

Status: source-preview client for Issue
[`BMS-Mania/IR#636`](https://github.com/BMS-Mania/IR/issues/636). It has not
completed Windows real-client acceptance and is not a public BMS-IR release.

This fork integrates the BMS-IR Arena protocol directly into OpenLR2. It does
not load or copy code from LR2ArenaEx. LR2ArenaEx was used only as a reference
for the general idea of integrating Arena behavior with an LR2-compatible
client.

## Setup

1. Configure OpenLR2's normal LR2IR connection for BMS-IR. Use the numeric
   BMS-IR player ID and the LR2 game-auth token from BMS-IR in the normal
   LR2IR fields.
2. Copy `BMSIR_ARENA.example.json` to
   `LR2files/Config/bmsir-arena.json`.
3. If OpenLR2 did not obtain the numeric IR ID during login, set `player_id`
   in that JSON. Do not put a password, token, or passmd5 in the Arena JSON.
4. Start the Arena-enabled OpenLR2 build. On music select, press `Insert` to
   enter or leave rated matchmaking. During the option-lock phase, choose the
   lane option in OpenLR2 and press `Insert` again to READY.

The overlay shows connection, queue, match, and chart state. This first
source-preview automatically delegates its nomination slot to server random.

## Implemented lifecycle

- WSS connection through Windows WinHTTP, with no extra networking DLL.
- Protocol v3 hello as `client_flavor=openlr2` and `ruleset_profile=lr2`.
- Existing LR2-compatible game credential reuse.
- Queue entry/cancel with the saved CPU and unrestricted-rating preferences.
- Match reservation and server-random nomination.
- Exact local MD5 possession lookup in `song.db`.
- Explicit option READY, LR2 option normalization, and exact locked-option
  application to the Arena chart.
- Server-selected chart transition through OpenLR2's ordinary decide/load
  path.
- A load-complete barrier followed by the server's shared start epoch.
- One-second EX/processed-note/BP/max-combo reporting.
- Complete and hard-fail final reporting.
- LR2 lamp/gauge conversion to the Arena clear-type IDs.
- Option restoration after the Arena result or cancellation.

## Diagnostics

`bmsir-arena.log` is JSON Lines and rotates at 2 MiB, retaining five backups.
It records transport failures, protocol types/phases, match IDs, chart MD5s,
scene transitions, option/play mode, counters, and final state.

The diagnostic writer deliberately removes:

- passwords, passmd5 values, and tokens;
- build/plugin fingerprints;
- chat text.

For a failed Windows test, reproduce once and attach
`bmsir-arena.log` plus the newest numbered backup if present. Also include the
OpenLR2 build architecture, Windows version, scene shown on screen, and whether
ordinary LR2IR login succeeded.

## Current source-preview limits

- Windows 10/11 x64 and x86 builds still require CI and real-client testing.
- CI outputs are unsigned source previews, not public BMS-IR releases.
- Server-random nomination is implemented; in-client candidate-folder
  nomination and room management are not yet implemented.
- Reconnecting a match that is already playing fails closed by forfeiting;
  live gameplay cannot safely be reconstructed.
- OpenLR2 exposes NORMAL, MIRROR, RANDOM, and S-RANDOM values differently from
  the Arena wire IDs. NORMAL/MIRROR/RANDOM are retained and unsupported values
  are clamped to NORMAL.
- The server must accept `openlr2` as an LR2-only Arena capability before this
  client can authenticate outside development.
- NORMAL RANDOM is available for 5K/7K and 10K/14K, where the server sends
  the LR2 seed equivalent of the canonical cross-client lane order. RANDOM is
  clamped to NORMAL for other key modes; MIRROR remains available.
- Production allowlists, downloadable artifacts, rollout/restart, changelog,
  and announcements are outside Issue #636.
