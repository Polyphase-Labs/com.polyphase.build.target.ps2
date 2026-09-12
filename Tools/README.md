# PS2 hardware iteration scripts

Thin wrappers around `ps2client` for testing on real hardware. PowerShell
(`.ps1`) and POSIX (`.sh`) versions of each are equivalent.

All three locate the project by walking up from this folder
(`Tools/` -> package -> `Packages/` -> project root), so they work from any
working directory and need no configuration beyond the console's IP.

| Script | Does |
|---|---|
| `ps2run` | Launches the staged ELF on the console |
| `ps2reload` | Hands the running game to another ELF, remotely |
| `ps2link` | Hands it back to ps2link specifically |
| `ps2log` | Tails the runtime log |

## Console IP

Pass it, or set it once:

```powershell
$env:PS2_IP = '192.168.1.50'     # PowerShell
```
```sh
export PS2_IP=192.168.1.50       # sh
```

With neither, `ps2client`'s own default is used.

## ps2run

```powershell
.\ps2run.ps1                     # or: ./ps2run.sh
.\ps2run.ps1 -Ip 192.168.1.50
```

**Why a script rather than calling ps2client directly:** `ps2client` serves
`host:` from *its own working directory*. Launch it from the wrong folder and
the game boots fine but every asset load fails — a failure that looks like a
renderer bug and is not one. The script always `cd`s to `Build/PS2` first.

It also warns when the staged ELF is more than ten minutes old. Testing a stale
binary has cost this project several debugging cycles.

## ps2reload

Requires the **Remote Reload** Target Option to be checked — it is compiled out
by default, so an unchecked build will simply ignore the marker file.

```powershell
.\ps2reload.ps1                      # relaunch the freshly built ELF
.\ps2reload.ps1 -Target ps2link      # back to ps2link
.\ps2reload.ps1 -Target menu         # FreeMcBoot OSD menu
.\ps2reload.ps1 -Target osdsys       # bare BIOS browser (last resort)
.\ps2reload.ps1 -Target mc0:/APPS/FOO.ELF
```
```sh
./ps2reload.sh            # game (default)
./ps2reload.sh osdsys
```

Writes the target path into `reload.cmd` in `Build/PS2`. The running game polls
that file once a second; when it holds anything other than `none` it writes
`none` back, stops the audio mixer and `LoadExecPS2`s the target.

### Why the marker is never deleted

Under ps2link, a `host:` open of a file that **is not there** creates a
*directory* with that name. Once that happens the PC-side write fails with
"access denied" and the console can no longer read the marker either, so the
feature silently disables itself and nothing you do appears to work.

So the file is kept permanently present: `ps2run` creates it holding `none`
before launching, and the console disarms by writing `none` back rather than
deleting. `ps2reload` and `ps2link` also clear a stray `reload.cmd` directory if
they find one. An empty or unreadable marker is ignored outright — it used to
mean "reboot to the BIOS", which is a bad thing to guess at mid-session.

No purpose-built "reboot ELF" is involved — `reload.cmd` is a *text file holding
a path*, and there is no reboot syscall in ps2sdk to call instead (`SifIopReboot`
restarts the IOP only).

`osdsys` is the most reliable escape hatch: every other target is a `host:` or
`mc0:` path that depends on ps2link's IOP modules still being alive to serve it,
whereas `rom0:OSDSYS` is always present.

## ps2link

```powershell
.\ps2link.ps1                                     # or: ./ps2link.sh
.\ps2link.ps1 -Path mass:/PS2LINK/ps2link.elf
```

Shorthand for `ps2reload -Target ps2link`. Defaults to
`mc0:/ps2link_1.2/PS2LINK.ELF`; override per-call, or set `$env:PS2LINK_ELF` /
`PS2LINK_ELF` once.

**You usually do not need this.** `ps2reload` with no arguments relaunches the
freshly built ELF directly — one step instead of two. Reach for `ps2link` when
you want ps2link itself back: to run different homebrew, or because a `host:`
target failed to load and you want the loader back in charge.

### Known limitation: only `rom0:OSDSYS` actually lands

Tested on hardware: `osdsys` works. `menu` and `ps2link` pass the pre-flight open
(so the path and case are right, and the ELF is readable) but the console then
black-screens.

The likely cause is the IOP, not the path. By the time the game hands over, the
IOP is carrying our audsrv, SIO2MAN/PADMAN, MCMAN/MCSERV and ps2link's own
modules, and an incoming launcher expects to bring up its own. `rom0:OSDSYS`
survives that because it needs no IOP modules at all. Making the others work
would mean a clean handover - `SifIopReset` plus re-init - which risks breaking
the one target that does work, so it is not done.

With FreeMcBoot installed, `rom0:OSDSYS` lands in the FMCB menu anyway, from
which ps2link can be launched by hand. Use `osdsys`.

**`mc0:` paths are case-sensitive.** The folder can be lowercase while the ELF
is uppercase — `mc0:/ps2link_1.2/PS2LINK.ELF` is a real example that cost a few
round trips. If a path looks right but the console reports it cannot be opened,
check the case before anything else; the failure log lists the parent directory
so you can read the exact spelling off it.

If it does not come back, the path is the first thing to check — it varies by
setup (`mc0:`, `mass:`, different folder). `ps2reload -Target osdsys` is the
escape hatch that depends on nothing.

## ps2log

```powershell
.\ps2log.ps1                # follow everything
.\ps2log.ps1 -Profile       # only the per-second frame profile lines
.\ps2log.ps1 -Packaged      # read the packaged run's log instead
```
```sh
./ps2log.sh
./ps2log.sh profile
./ps2log.sh packaged
```

`-Profile` filters to the `[PS2] VU1 …` / `[PS2] EE …` lines, which is what you
want while watching a timing change land.
