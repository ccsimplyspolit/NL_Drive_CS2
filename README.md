<div align="center">

# NL_Drive_CS2

**Kernel-mode Counter-Strike 2 helper kits — kill-trigger yaw injector + `m_bIsValveDS` spoofer.**

[![Latest release](https://img.shields.io/github/v/release/ccsimplyspolit/NL_Drive_CS2?label=latest&color=2ea44f&logo=github)](https://github.com/ccsimplyspolit/NL_Drive_CS2/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/ccsimplyspolit/NL_Drive_CS2/total?color=2ea44f&logo=github)](https://github.com/ccsimplyspolit/NL_Drive_CS2/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D4?logo=windows)](https://www.microsoft.com/en-us/windows)
[![Build](https://img.shields.io/github/actions/workflow/status/ccsimplyspolit/NL_Drive_CS2/build-release.yml?label=CI&logo=githubactions)](https://github.com/ccsimplyspolit/NL_Drive_CS2/actions)
[![Docs](https://img.shields.io/badge/docs-RU%20%2F%20EN-35C46A)](docs/WIKI.md)
[![License](https://img.shields.io/badge/license-research-lightgrey)](#disclaimer)

![hero](docs/assets/readme-hero.png)

</div>

> [!IMPORTANT]
> **Always use the latest GitHub Release.** Old tags stay for diff/history only.
> Бери только последний релиз. Старые теги — для истории.

---

## ✨ Two kits in one repo

<table>
<tr>
<td width="50%" valign="top">

### 🎯 F20Kit
Kernel-mode round-kill detector. On every kill:
- holds **P** for 2.5 s (cooldown window)
- **300 ms before P-up** fires one tap from a **22-key** pool (Numpad 0–9 + F13–F24)
- **alternating yaw sign** (POS ↔ NEG every kill)
- **excludes the symmetric counterpart** (no `+M / −M` pairs)
- magnitudes uniformly cover **[−35° … +35°]**

The tap fires while P is still held so the cheat's MOUSE OVERRIDE yaw change lands inside the kill-action key window.

Injects via `kbdclass!KeyboardClassServiceCallback` resolved through Microsoft PDB symbols first, byte-pattern fallback second, and falls back to monitor-only if nothing is safe.

</td>
<td width="50%" valign="top">

### 🪪 IsValveDS spoofer
Kernel-mode `C_CSGameRules::m_bIsValveDS` flipper, driven from a user-mode console.
- shared-memory + named-event control plane (no IRP)
- re-resolves `cs2.exe` → `client.dll` → `dwGameRules` → field every iteration
- console writes a **full diagnostic log** next to the exe
- survives `cs2.exe` restart and map flip

</td>
</tr>
</table>

---

## 🚀 Quick Start

| Step | What to do |
| ---- | ---------- |
| **1** | Download the latest release: [📦 Releases](https://github.com/ccsimplyspolit/NL_Drive_CS2/releases/latest) |
| **2** | Extract `F20Kit.zip` and/or `IsValveDS_spoofer.zip` to a short path, e.g. `C:\NL_Drive_CS2\F20Kit` |
| **3** | Right-click → **Run as Administrator**: `START.bat` (F20Kit) or `bin\run.bat` (IsValveDS) |
| **4** | Read the post-load message — it gives you the **one-line CS2 console paste** and the NumLock reminder |

> [!NOTE]
> **No Visual C++ Redistributable required.** Kits ship the runtime DLLs app-local. If they get quarantined, run `install_vcredist.bat` once.

---

## ⌨️ F20Kit — yaw bind table (configure these in your cheat)

> The driver picks the key; your cheat's `MOUSE OVERRIDE` / yaw `Local view` bind list must map each key to the matching yaw value.

<table>
<tr><th>Pool</th><th colspan="11">Key → yaw (deg)</th></tr>
<tr>
<td>🟢 <b>POSITIVE</b></td>
<td>Num1<br>+1</td><td>Num2<br>+4</td><td>Num4<br>+8</td><td>Num6<br>+11</td><td>Num8<br>+15</td>
<td>F13<br>+18</td><td>F15<br>+21</td><td>F17<br>+25</td><td>F19<br>+28</td><td>F21<br>+32</td><td>F23<br>+35</td>
</tr>
<tr>
<td>🔴 <b>NEGATIVE</b></td>
<td>Num0<br>−1</td><td>Num3<br>−4</td><td>Num5<br>−8</td><td>Num7<br>−11</td><td>Num9<br>−15</td>
<td>F14<br>−18</td><td>F16<br>−21</td><td>F18<br>−25</td><td>F20<br>−28</td><td>F22<br>−32</td><td>F24<br>−35</td>
</tr>
</table>

Hold key: **P** (scan `0x19`), held for **2500 ms** on each accepted kill.

### One-line CS2 console paste (run once per session)

Paste this into the CS2 console so the game itself never reacts to the driver's keypresses — they exist only as triggers for the cheat:

```text
unbind p; unbind F13; unbind F14; unbind F15; unbind F16; unbind F17; unbind F18; unbind F19; unbind F20; unbind F21; unbind F22; unbind F23; unbind F24; unbind KP_INS; unbind KP_END; unbind KP_DOWNARROW; unbind KP_PGDN; unbind KP_LEFTARROW; unbind KP_5; unbind KP_RIGHTARROW; unbind KP_HOME; unbind KP_UPARROW; unbind KP_PGUP
```

> [!WARNING]
> **NumLock MUST be ON** (LED lit). Scan codes `0x47..0x52` only register as Numpad digits with NumLock on; otherwise they inject as nav-cluster (`Home/End/arrows/Ins/PgUp/PgDn`) and the cheat will see the wrong key.

`START.bat` prints both the line and the NumLock reminder after loading the driver, so you can just copy from the launcher window.

---

## 🛑 Stop / Cleanup

| Kit | Command |
| --- | --- |
| F20Kit | `STOP.bat` |
| IsValveDS | `bin\stop.bat` *or* `stop` in console |

Both kits do a **safe stop event → wait for done event → tracked `kdunmap --alreadyStopped`** sequence. If worker-exit is not confirmed, the scripts refuse blind unmap and ask for a reboot instead.

---

## 🛰️ Named-object map (kernel ↔ user)

| Kit | Kernel object | Win32 name | Purpose |
| --- | --- | --- | --- |
| F20Kit | `\BaseNamedObjects\F20DriverStop` | `Global\F20DriverStop` | request worker stop |
| F20Kit | `\BaseNamedObjects\F20DriverStopped` | `Global\F20DriverStopped` | cleanup finished |
| IsValveDS | `\BaseNamedObjects\IsValveDSState` | `Global\IsValveDSState` | shared memory |
| IsValveDS | `\BaseNamedObjects\IsValveDSStop` | `Global\IsValveDSStop` | request worker stop |
| IsValveDS | `\BaseNamedObjects\IsValveDSStopped` | `Global\IsValveDSStopped` | cleanup finished |

Kernel creates objects directly under `\BaseNamedObjects\<name>` (no extra `\Global\` path component). Win32 `OpenEvent("Global\<name>")` translates to the same path. This wire-up survives hardened Windows builds where the implicit `\BaseNamedObjects\Global` symlink might be missing.

---

## 🧰 VC++ runtime

The kits include four app-local DLLs next to `kdmap.exe` / `kdunmap.exe`:

| DLL | Purpose |
| --- | --- |
| `msvcp140.dll` | C++ standard library |
| `vcruntime140.dll` | base C runtime |
| `vcruntime140_1.dll` | C runtime extension |
| `concrt140.dll` | concurrency runtime |

If those DLLs are quarantined by AV or you prefer a system-wide install, run `install_vcredist.bat`. It offers two sources:

- Microsoft VC++ 2015-2022 x64 Redistributable (official permalink `aka.ms`).
- VisualCppRedist AIO from `abbodi1406/vcredist` GitHub release, launched with the documented `/y` CLI switch.

No third-party installer is bundled in the repository.

---

## 📁 Repository layout

```text
NL_Drive_CS2/
├─ src/
│  ├─ drivers/
│  │  ├─ F20Driver/          # kernel driver — kill trigger + yaw inject
│  │  └─ IsValveDS/          # kernel driver — m_bIsValveDS spoofer
│  ├─ apps/
│  │  └─ IsValveDSConsole/   # user-mode SHM console with file logger
│  └─ tools/
│     ├─ analyze_kbdclass/   # PDB-based kbdclass analyzer
│     ├─ kdmap/              # tracked mapper wrapper
│     └─ kdunmap/            # tracked unmapper wrapper
├─ kits/
│  ├─ F20Kit/                # F20 runtime kit (zip source)
│  └─ IsValveDS/             # IsValveDS runtime kit (zip source)
├─ scripts/
│  └─ build_release.ps1      # build everything + sync into kits + zip
├─ .github/workflows/
│  └─ build-release.yml      # CI build + release publishing
└─ docs/
   ├─ WIKI.md                # full RU / EN guide (mirrors GitHub Wiki)
   └─ assets/                # readme/wiki images
```

---

## 🔨 Build locally

Requirements:
- Visual Studio 2022 with C++ workload
- Windows SDK / WDK 10.0.26100.x (restored via NuGet)
- PowerShell 5+
- `TheCruZ/kdmapper` checked out next to this repository as `..\kdmapper`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_release.ps1
```

Outputs:
- `kits\F20Kit\F20Kit.zip`
- `kits\IsValveDS\IsValveDS_spoofer.zip`

---

## 🤖 GitHub Actions

`.github/workflows/build-release.yml` builds on Windows Server 2022:
- restores WDK / SDK NuGet packages,
- clones and builds `kdmapper` static library,
- builds all drivers / tools / consoles,
- packages both release zips,
- uploads workflow artifacts on every main/PR build,
- publishes release assets automatically on `v*` tag push or manual run.

Publish a release from git:

```powershell
git tag v1.2
git push origin v1.2
```

---

## 🩺 Diagnostics

Launchers collect a pre-load diagnostics bundle **before** mapping any driver:

- `F20Kit\logs\diag_preload_*`
- `IsValveDS\bin\logs\diag_preload_*`

The IsValveDS console additionally writes `IsValveDS_Console.log` next to the exe with timestamps for every WinAPI call, every command typed, and an unhandled-exception filter that captures SEH / CRT invalid-parameter / pure-call / `std::terminate`.

For bug reports send the latest launcher log, the matching `diag_preload_*` folder/zip, `IsValveDS_Console.log` if applicable, DebugView output, and the latest minidump if a BSOD occurred.

---

## ⚖️ Disclaimer

Research / educational project. The code documents Windows kernel ideas (kdmapper-style manual map without IRP, MmCopyVirtualMemory-based cross-process IO, SHM + named event control plane, SEH-wrapped PEB walks, etc.) using Counter-Strike 2 as a measurable target. Use of injection or game-modifying drivers against live multiplayer servers is forbidden by Valve. You are responsible for what you do with this code.
