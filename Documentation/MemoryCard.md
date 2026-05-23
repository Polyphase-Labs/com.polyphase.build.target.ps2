# PS2 Memory Card — Polyphase Build Target

`Runtime/PS2/System_PS2.cpp` implements `SYS_ReadSave` / `SYS_WriteSave` /
`SYS_DoesSaveExist` against libmc (PS2SDK's memory-card library), with a
graceful host-disk fallback when no card is present.

## What lives where

| Path | What goes there |
| --- | --- |
| `mc0:/BXDATA-POLY0001/icon.sys` | PS2 Browser metadata — title, background color, lights, icon filenames. Written once on first save. |
| `mc0:/BXDATA-POLY0001/<saveName>` | One file per `SYS_WriteSave(saveName, ...)` call. |
| `host:save/<saveName>` | Fallback when no MC is detected (PCSX2 `-elf` boot without an MC configured). |

The folder name `BXDATA-POLY0001` follows the PS2 Browser convention:

- `B` = save data folder (vs. `S` for "single icon" or `R` for "homebrew root")
- `XDATA-` = third-party / homebrew prefix (vs. region prefixes `ASLUS` /
  `ESLES` / `ISLPS` reserved for licensed games)
- `POLY0001` = 8-char product code (matches the build profile's default
  `discId`). Max folder length is 32 chars; the constraint is browser
  enforcement, not libmc.

If a project changes `discId` in their Build Profile, the folder name in
`System_PS2.cpp` (`kMcSaveDir = "/BXDATA-POLY0001"`) should match. v1
doesn't plumb `discId` to runtime, so it's hardcoded — update both places
together.

## libmc API discipline

Every libmc call except `mcInit` is **asynchronous**. The canonical pattern
is:

```cpp
// 1. Fire the async call
if (mcOpen(port, slot, path, mode) < 0) return -1;  // bad args / not init'd
// 2. Block until completion
int cmd = 0, result = 0;
mcSync(0, &cmd, &result);
// 3. `result` carries the actual return value (fd for mcOpen, byte count
//    for mcRead/Write, status code for mcMkDir, etc.)
return result;
```

If you skip `mcSync`, the next libmc call returns "BUSY". If you call
`mcSync` without first firing an async call, it blocks forever (no command
to sync). One-at-a-time is the rule.

The addon wraps this in sync helpers (`McOpenSync`, `McCloseSync`,
`McReadSync`, `McWriteSync`, `McSeekSync`, `McDeleteSync`) so the public
`SYS_*` API doesn't repeat the pattern. These wrappers are declared above
their first user inside the anonymous namespace in `System_PS2.cpp`.

## Path conventions

- Paths are **relative to the card root** — no `"mc0:"` prefix in libmc
  calls.
- Paths start with a forward slash, e.g. `"/BXDATA-POLY0001/savefile.dat"`.
- The leading slash is significant — without it, libmc treats the path as
  relative to the card's current directory (which is undefined at boot
  and varies by BIOS revision).
- POSIX-style mode flags from `<fcntl.h>` work: `O_RDONLY`, `O_WRONLY`,
  `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`. The libmc backend translates
  them to the card's native flags internally.

## Card probe (one-shot cache)

`McProbeCard` is called before every save/read attempt. It memoises the
verdict in an enum:

```cpp
enum class McProbeState : int { Untried = 0, Available = 1, Unavailable = 2 };
```

A Lua game can spam `SYS_DoesSaveExist` 60× per second polling for save
data; without caching, every poll would round-trip `mcGetInfo` through
SIF RPC. The cache only re-probes if the verdict was `Untried` — once
`Unavailable`, it stays unavailable for the session (i.e. hot-inserting a
card mid-game isn't supported in v1).

`mcGetInfo` returns three values via out-pointers:

| Field | Meaning |
| --- | --- |
| `type` | `MC_TYPE_PS2` for a Sony PS2 card; other values for PocketStation / unsupported. |
| `freeKb` | Approximate KB free. Used in the boot log only — not in any decision. |
| `formatted` | 1 if the card is formatted as a PS2 card; 0 if blank / unformatted. |

A card is "usable" iff `type == MC_TYPE_PS2 && formatted == 1`. Anything
else falls through to `host:save/`.

## icon.sys

PS2 Browser browses each save folder and reads `icon.sys` for the visual
metadata: title text, background gradient, lighting setup, and icon
filenames. Without `icon.sys`, the folder may be invisible in the browser
(behaviour depends on BIOS revision and emulator — PCSX2's MC editor shows
it but most real-HW BIOSes hide it).

`McWriteIconSysIfMissing` writes `icon.sys` on first save and skips the
write on subsequent saves (it's wasteful to re-write a 964-byte struct
on every save).

### `McIconSys` layout

The struct mirrors libmc's `mcIcon`. All fields are little-endian on PS2 EE:

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0x000 | 4 B | `head[4]` | Magic `"PS2D"` |
| 0x004 | 2 B | `type` | 0 |
| 0x006 | 2 B | `nlOffset` | Byte index in `title[]` where line 2 begins |
| 0x008 | 4 B | `unknown2` | 0 |
| 0x00C | 4 B | `trans` | Background transparency 0..0x80 |
| 0x010 | 64 B | `bgCol[16]` | 4 corners × RGBA, each 0..0x80 (NOT 0..0xFF — browser clamps) |
| 0x050 | 48 B | `lightDir[12]` | 3 lights × XYZW (W ignored) |
| 0x080 | 48 B | `lightCol[12]` | 3 lights × RGBA 0..1.0 |
| 0x0B0 | 16 B | `lightAmbient[4]` | Ambient RGBA |
| 0x0C0 | 68 B | `title[68]` | Packed ASCII (Shift-JIS single-byte range). Line break implicit at `nlOffset`. |
| 0x104 | 64 B | `view[64]` | Icon filename for list view |
| 0x144 | 64 B | `copy[64]` | Icon filename when copying |
| 0x184 | 64 B | `del[64]` | Icon filename when deleting |
| 0x1C4 | 512 B | `unknown3[512]` | Padding to 964 bytes |

`static_assert(sizeof(McIconSys) == 964)` enforces the layout.

### Polyphase's icon.sys content

- **Title**: "Polyphase" (line 1) + "Save Data" (line 2), `nlOffset = 16`.
- **Background**: dark blue gradient (R=0x10, G=0x20, B=0x50, A=0x80
  all four corners — flat fill).
- **Lights**: 3-point standard setup (key from upper-right, fill from
  left, rim from behind-right) + low ambient (0.2, 0.2, 0.3).
- **Icon file**: `icon.icn` for all three states (view/copy/del). The
  addon does NOT ship `icon.icn` in v1 — most browsers (PCSX2 MC editor,
  every real-HW BIOS tested) show a placeholder when the referenced
  `.icn` is missing. If a browser hides the folder, write a real `.icn`.

## Save flow

`SYS_WriteSave(saveName, stream)`:

```
McProbeCard ──► card available?
    yes ─► McEnsureSaveDir  (mcMkDir + icon.sys write if missing)
        ─► McOpenSync(path, O_CREAT | O_TRUNC | O_WRONLY)
        ─► McWriteSync(fd, stream.data, stream.size)
        ─► McCloseSync(fd)
    no  ─► HostFallbackPath → fopen + fwrite
```

`SYS_ReadSave(saveName, outStream)`:

```
McProbeCard ──► card available?
    yes ─► McOpenSync(path, O_RDONLY)
        ─► McSeekSync(fd, 0, SEEK_END) → size
        ─► McSeekSync(fd, 0, SEEK_SET)
        ─► McReadSync(fd, outStream.data, size)
        ─► McCloseSync(fd)
    no  ─► HostFallbackPath → fopen + fread
```

`mcMkDir` returns `-4` ("exists") when the folder is already there. This
addon treats any non-fatal `mcMkDir` result as success and lets the
subsequent `mcOpen` surface real failures (folder permissions, card full,
etc.).

## Boot dependencies

Memory-card I/O requires three pieces in place at boot:

1. `SifLoadModule("rom0:SIO2MAN", 0, nullptr)` — drives the SIO2 bus the
   card sits on. Same module input uses, so it's loaded first in
   `Main_PS2.cpp`.
2. `SifLoadModule("rom0:MCMAN", 0, nullptr)` — IOP-side card driver.
3. `SifLoadModule("rom0:MCSERV", 0, nullptr)` — IOP-side filesystem.
4. `mcInit(MC_TYPE_XMC)` — EE-side libmc handshake. `MC_TYPE_XMC` selects
   the newer xmcman/xmcserv ABI; it's backwards-compatible with classic
   `mcman`/`mcserv` so loading `rom0:MCMAN` works for both.

If any of those fails, the addon logs `[2b] libmc up …` failure and
`SYS_WriteSave` falls through to host-disk fallback for the remainder of
the session.

## Failure modes to know

| Symptom | Likely cause |
| --- | --- |
| `mcInit` returns negative | MCMAN/MCSERV failed to load — check earlier `[2b]` boot log |
| `mcOpen` returns -5 | Path doesn't exist (no `O_CREAT`) — caller should retry with `O_CREAT` |
| `mcOpen` returns -4 | Path exists when `O_EXCL`-style behaviour was intended — race |
| `McProbeCard` returns false despite a card being inserted | Card unformatted; format via PS2 BIOS or PCSX2 MC editor |
| Saves invisible in real-HW browser | `icon.sys` missing or malformed; check `McWriteIconSysIfMissing` log |
| Saves work in PCSX2 but not real HW | Folder name doesn't follow `B`/`S`/`R` prefix convention — browser hides nonconforming folders |

## Open work

- **Multi-slot support** — currently hardcoded to port 0, slot 0. The
  PS2 has two memory-card slots; some games store profile data in slot 1.
- **`mcChDir` / `mcChStat` for atomic updates** — write to temp file,
  rename. Avoids corruption on power-cut mid-write. Worth doing for
  user-facing saves but probably not needed for autosaves.
- **discId plumbing** — runtime currently can't read the build profile's
  `discId` to update `kMcSaveDir`. A `-DPOLYPHASE_PS2_DISC_ID=...` Makefile
  define would fix this.

## References

- [PS2SDK libmc source](https://github.com/ps2dev/ps2sdk/tree/master/ee/rpc/libmc)
- [PS2 Browser folder conventions](https://www.ps2-home.com/forum/viewtopic.php?t=3097) — community write-up on folder naming
- [`mcIcon` reference](https://github.com/ps2dev/ps2sdk/blob/master/ee/rpc/libmc/include/libmc.h)
