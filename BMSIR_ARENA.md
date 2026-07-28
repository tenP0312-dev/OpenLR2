# BMS-IR Arena for OpenLR2 0.2.0

Status: controlled direct-link tester client for Issue
[`BMS-Mania/IR#656`](https://github.com/BMS-Mania/IR/issues/656). It has not
completed Windows real-client acceptance and is not listed as a general public
BMS-IR download.

This fork integrates the BMS-IR Arena protocol directly into OpenLR2. It does
not load or copy code from LR2ArenaEx. LR2ArenaEx was used only as a reference
for the general idea of integrating Arena behavior with an LR2-compatible
client.

## Setup

1. Download the ZIP matching the OpenLR2 installation architecture (`x64` or
   `x86`) and extract it.
2. Close OpenLR2. Back up the existing `OpenLR2_x64.exe` or
   `OpenLR2_x86.exe`, then copy the tester executable into the OpenLR2
   installation directory beside `LR2files`.
3. Configure OpenLR2's normal LR2IR connection for BMS-IR. Use the numeric
   BMS-IR player ID and the LR2 game-auth token from BMS-IR in the normal
   LR2IR fields.
4. Copy `BMSIR_ARENA.example.json` to
   `LR2files/Config/bmsir-arena.json`.
5. If OpenLR2 did not obtain the numeric IR ID during login, set `player_id`
   in that JSON. Do not put a password, token, or passmd5 in the Arena JSON.
6. Start the Arena-enabled OpenLR2 build. On music select or result, press
   `F9` to open the Arena control panel. `Tab` changes pages, arrow keys move or
   change a setting, and `Enter` activates it.

To restore ordinary OpenLR2, close the game and restore the backed-up
executable.

## Controls

- `F9`: open or close the Arena control panel on music select/result.
- `F10`: hide or restore the complete Arena overlay from any scene.
- `Insert`: context action outside the panel: rated entry, room READY, chart
  nomination, or option READY.
- `Delete`: delegate a room nomination to server random.
- `End`: vote to force-end the active chart. Disconnected players are treated
  as having voted; the chart ends after all remaining players vote.

The five control pages are `MAIN`, `PUBLIC ROOMS`, `ROOM SETUP`, `CHAT`, and
`MANUAL`. Text fields use OpenLR2's text-input window and are confirmed with
Enter.

## Implemented lifecycle

- WSS connection through Windows WinHTTP, with no extra networking DLL.
- Protocol v5 hello as `client_flavor=openlr2` and `ruleset_profile=lr2`.
- Existing LR2-compatible game credential reuse.
- Queue entry/cancel with the saved CPU and unrestricted-rating preferences.
- Server-managed CPU matches. CPU has no rating; when no human opponent is
  available it immediately matches, chooses the highest eligible locally owned
  Arena chart, scores a preselected AA-to-MAX value, and changes the human
  rating by +1/0/-1.
- Public/locked room creation and joining, room names, host transfer, kick,
  selector changes, participant/spectator switching, READY, room retention,
  disband, and room-code clipboard copy/paste.
- EX SCORE, LOWEST BP, and MAX COMBO room rules; forced gauge; official/free
  chart scope; all/host/rotating nomination; single, all-picks, and first-to
  series.
- Public-lobby and room/match chat, with passwords and chat text excluded from
  diagnostics.
- Server-delivered manual, so server-only rule changes do not require a new
  OpenLR2 executable.
- Persistent results, large rated delta display, and explicit close action.
- Phase/action text and all countdowns, with the final 10 seconds yellow and
  final 5 seconds red.
- Relative battle bars supplied by the server for EX SCORE, BP, and MAX COMBO.
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

## Current tester-preview limits

- Windows x64 and x86 compile/protocol-test CI passes. Windows 10/11
  real-client lifecycle and gameplay testing is still pending.
- CI outputs are unsigned tester previews, not general public BMS-IR releases.
- Ranked nomination remains server-random. Rooms can nominate the chart
  currently highlighted in music select or delegate to server random.
- Reconnecting a match that is already playing fails closed by forfeiting;
  live gameplay cannot safely be reconstructed.
- OpenLR2 exposes NORMAL, MIRROR, RANDOM, and S-RANDOM values differently from
  the Arena wire IDs. NORMAL/MIRROR/RANDOM/S-RANDOM are retained. R-RANDOM,
  SPIRAL, and OpenLR2-only scatter/converge options have no safe shared mapping
  and are clamped to NORMAL.
- The server must accept `openlr2` as an LR2-only Arena capability before this
  client can authenticate outside development.
- NORMAL RANDOM is available for 5K/7K and 10K/14K, where the server sends
  the LR2 seed equivalent of the canonical cross-client lane order. In DP,
  OpenLR2 restarts the synchronized sequence for 2P so both sides match the
  independently seeded oraja-side layouts. RANDOM is clamped to NORMAL for
  other key modes; MIRROR remains available.
- The reviewed build may be installed behind exact direct URLs and private
  server allowlists for acceptance testing. Public download-page listing,
  changelog publication, and announcements remain outside this preview.
