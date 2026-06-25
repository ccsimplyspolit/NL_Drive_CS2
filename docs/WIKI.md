<div align="center">

# 📘 NL_Drive_CS2 — Wiki

[![Latest](https://img.shields.io/github/v/release/ccsimplyspolit/NL_Drive_CS2?label=latest&color=2ea44f&logo=github)](https://github.com/ccsimplyspolit/NL_Drive_CS2/releases/latest)
[![Build](https://img.shields.io/github/actions/workflow/status/ccsimplyspolit/NL_Drive_CS2/build-release.yml?label=CI&logo=githubactions)](https://github.com/ccsimplyspolit/NL_Drive_CS2/actions)
[![Lang](https://img.shields.io/badge/docs-RU%20%2F%20EN-35C46A)](#)
[![Repo](https://img.shields.io/badge/repo-ccsimplyspolit%2FNL__Drive__CS2-181717?logo=github)](https://github.com/ccsimplyspolit/NL_Drive_CS2)

</div>

---

<table>
<tr>
<td align="center" width="50%">

### 🇷🇺 Русский
[Полная инструкция ↓](#ru-полная-инструкция)

</td>
<td align="center" width="50%">

### 🇬🇧 English
[Full guide ↓](#en-full-guide)

</td>
</tr>
</table>

---

## [RU] Полная инструкция

### 📑 Содержание

1. [Что это и из чего состоит](#1-что-это-и-из-чего-состоит)
2. [Скачать готовые файлы](#2-скачать-готовые-файлы)
3. [Требования к системе](#3-требования-к-системе)
4. [F20Kit — первый запуск](#4-f20kit--первый-запуск)
5. [F20Kit — таблица yaw-биндов](#5-f20kit--таблица-yaw-биндов)
6. [F20Kit — остановка](#6-f20kit--остановка)
7. [F20Kit — диагностика](#7-f20kit--диагностика)
8. [IsValveDS — первый запуск](#8-isvalveds--первый-запуск)
9. [IsValveDS — остановка](#9-isvalveds--остановка)
10. [VC++ runtime](#10-vc-runtime)
11. [Сборка из исходников](#11-сборка-из-исходников)
12. [GitHub Actions / релизы](#12-github-actions--релизы)
13. [Что присылать при проблемах](#13-что-присылать-при-проблемах)
14. [Частые ошибки](#14-частые-ошибки)

---

### 1. Что это и из чего состоит

`NL_Drive_CS2` — это два production-ready kernel-mode kit'а для Counter-Strike 2:

| Kit | Назначение |
| --- | --- |
| 🎯 **F20Kit** | На каждый kill: P-down → ждём **2.2 сек** → tap одной из 22 клавиш (Numpad 0-9 + F13-F24) с yaw `[-35..+35]` → ждём 0.3 сек → P-up. Tap происходит **за 300 ms до отжатия P**, пока kill-action key ещё активен, поэтому MOUSE OVERRIDE в чите успевает применить yaw в момент когда P держится. Знак yaw чередуется (POS ↔ NEG), magnitude uniform-random из 10 (исключая симметричный к предыдущему). |
| 🪪 **IsValveDS** | Spoof `C_CSGameRules::m_bIsValveDS` из kernel-mode, управление через user-mode SHM-консоль. |

Раскладка по репозиторию:

```text
NL_Drive_CS2/
├─ src/
│  ├─ drivers/F20Driver/          ← F20 kernel driver
│  ├─ drivers/IsValveDS/          ← IsValveDS kernel driver
│  ├─ apps/IsValveDSConsole/      ← SHM-console с file-логом
│  └─ tools/
│     ├─ analyze_kbdclass/        ← PDB-based kbdclass analyzer
│     ├─ kdmap/                   ← tracked mapper (kdmapper_lib)
│     └─ kdunmap/                 ← tracked unmapper
├─ kits/
│  ├─ F20Kit/                     ← готовый kit (zip source)
│  └─ IsValveDS/                  ← готовый kit (zip source)
├─ scripts/build_release.ps1      ← сборка всех проектов + zip
└─ .github/workflows/             ← CI build + auto-release
```

---

### 2. Скачать готовые файлы

🔗 [github.com/ccsimplyspolit/NL_Drive_CS2/releases/latest](https://github.com/ccsimplyspolit/NL_Drive_CS2/releases/latest)

| Архив | Что внутри |
| --- | --- |
| `F20Kit.zip` | F20Driver.sys + START.bat/STOP.bat + analyzer + kdmap/kdunmap + PS1 скрипты + app-local VC++ runtime + install_vcredist.bat |
| `IsValveDS_spoofer.zip` | IsValveDS_Driver.sys + IsValveDS_Console.exe + run.bat/stop.bat + kdmap/kdunmap + PS1 скрипты + app-local VC++ runtime + install_vcredist.bat |

Распаковывай в короткий путь без кириллицы и спецсимволов, например:

```text
C:\NL_Drive_CS2\F20Kit
C:\NL_Drive_CS2\IsValveDS
```

> ⚠️ **Используй ТОЛЬКО latest release.** Старые теги (v1, v2, v9.0) оставлены для истории / diff и могут содержать regressed-логику.

---

### 3. Требования к системе

| Компонент | Требование | Авто-фикс? |
| --- | --- | --- |
| Windows | 10/11 x64 | — |
| Права | Admin (для kdmap) | START.bat сам elevate |
| Secure Boot | OFF | ❌ только BIOS/UEFI |
| HVCI / Memory Integrity | OFF | ✅ START.bat |
| VulnerableDriverBlocklist | `= 0` | ✅ START.bat |
| AntiCheat (vgc/EAC/BE/FACEIT) | не запущен | ✅ START.bat kill |
| `iqvw64e` lingering | не загружен | ✅ START.bat clean |

> 🛡️ **F20Driver больше не полагается на один byte-pattern.** `analyze_kbdclass.exe` достаёт PDB GUID/Age из `kbdclass.sys`, тянет символы с Microsoft Symbol Server и находит `kbdclass!KeyboardClassServiceCallback` точным RVA. Fallback на сигнатуры используется только если PDB недоступен. Если ничего не безопасно — драйвер уходит в `monitor-only mode` (без BSOD).

---

### 4. F20Kit — первый запуск

```text
1. Запусти CS2 (Fullscreen Windowed).
2. Включи NumLock (LED горит).
3. Запусти F20Kit\START.bat ОТ АДМИНИСТРАТОРА.
4. Дождись сообщения "Driver loaded".
5. Открой консоль CS2 (~) и вставь ОДНУ СТРОКУ из STEP 2 которую START.bat
   выводит после загрузки (unbind p + F13..F24 + KP_*). 23 клавиши.
6. Настрой yaw-бинды в чите по таблице ниже (раздел 5).
7. Играй.
```

Что делает `START.bat`:

- создаёт `logs\start_<timestamp>.log` + alias `START_LAST.log`;
- собирает full pre-load диагностику в `logs\diag_preload_*`;
- safe-stop предыдущий instance + tracked kdunmap;
- обновляет CS2 offsets через `update_cs2_offsets.ps1` (TLS 1.2/1.3, sanity check);
- запускает `analyze_kbdclass.exe` с PDB resolution;
- грузит `F20Driver.sys` через `kdmap.exe --key F20Driver --indPages`;
- выводит **готовую unbind-строку** + **напоминание про NumLock**.

> 💡 **Точный timing одного kill-цикла:**
> ```
>  T=0      ms : InjectScan(P, DOWN)           ← kill detected, P нажата
>  T=2200   ms : InjectScan(tap, DOWN)         ← yaw-tap начинается, P ещё держится
>  T=2255   ms : InjectScan(tap, UP)           ← yaw-tap завершён
>  T=2500   ms : InjectScan(P, UP)             ← P отпущена
> ```
> Если в течение этих 2500 ms случится ещё kill — игнорируется (cooldown).

---

### 5. F20Kit — таблица yaw-биндов

Драйвер выбирает кнопку, **чит привязывает её к yaw** через MOUSE OVERRIDE / "Local view". Эти значения **должны совпадать** с тем, что записано в чите:

| 🟢 POSITIVE | scan | yaw | | 🔴 NEGATIVE | scan | yaw |
| --- | --- | --- | --- | --- | --- | --- |
| Num1 | `0x4F` | **+1°** | | Num0 | `0x52` | **−1°** |
| Num2 | `0x50` | **+4°** | | Num3 | `0x51` | **−4°** |
| Num4 | `0x4B` | **+8°** | | Num5 | `0x4C` | **−8°** |
| Num6 | `0x4D` | **+11°** | | Num7 | `0x47` | **−11°** |
| Num8 | `0x48` | **+15°** | | Num9 | `0x49` | **−15°** |
| F13 | `0x64` | **+18°** | | F14 | `0x65` | **−18°** |
| F15 | `0x66` | **+21°** | | F16 | `0x67` | **−21°** |
| F17 | `0x68` | **+25°** | | F18 | `0x69` | **−25°** |
| F19 | `0x6A` | **+28°** | | F20 | `0x6B` | **−28°** |
| F21 | `0x6C` | **+32°** | | F22 | `0x6D` | **−32°** |
| F23 | `0x6E` | **+35°** | | F24 | `0x76` | **−35°** |

**Hold key:** P (scan `0x19`), 2500 ms.

**Логика выбора:**
1. Каждый kill — flip знака (POS ↔ NEG).
2. Внутри противоположного pool — uniform random из **10 кандидатов** (11 minus симметричный к предыдущему magnitude).
3. Никогда не `+M / -M` подряд — взгляд не сводится в 0°.

Пример: `+18 → -25 → +8 → -11 → +35 → -4 → ...`

#### Одна строка для CS2 console (вставить раз за сессию)

```text
unbind p; unbind F13; unbind F14; unbind F15; unbind F16; unbind F17; unbind F18; unbind F19; unbind F20; unbind F21; unbind F22; unbind F23; unbind F24; unbind KP_INS; unbind KP_END; unbind KP_DOWNARROW; unbind KP_PGDN; unbind KP_LEFTARROW; unbind KP_5; unbind KP_RIGHTARROW; unbind KP_HOME; unbind KP_UPARROW; unbind KP_PGUP
```

> ⚠️ **NumLock = ON** обязателен. Иначе скан-коды `0x47..0x52` придут в игру как nav-кластер (Home/End/стрелки/Ins/PgUp/PgDn), и cheat не увидит правильную клавишу.

---

### 6. F20Kit — остановка

```text
STOP.bat
```

Алгоритм:
1. сигналит `Global\F20DriverStop`;
2. ждёт `Global\F20DriverStopped` (worker confirms cleanup);
3. только при подтверждении — `kdunmap.exe --key F20Driver --alreadyStopped`.

Если worker не подтвердил выход — скрипт **не делает blind free** и предлагает reboot.

---

### 7. F20Kit — диагностика

Основные логи:

```text
F20Kit\START_LAST.log                ← заголовок последнего запуска
F20Kit\logs\start_*.log              ← timestamped per-run launcher log
F20Kit\logs\diag_preload_*           ← pre-load диагностический бандл
F20Kit\logs\diag_preload_*.zip       ← он же в zip
F20Kit\logs\stop_*                   ← диагностика после STOP.bat
```

**DebugView filter:** `*F20Drv*`

Ожидаемые строки в DebugView:

```text
[F20Drv] ======================================
[F20Drv]    F20Driver v10 (P hold + 22-key yaw pool spanning [-35..+35])
[F20Drv] ======================================
[F20Drv] OS: 10.0 build 26200 (Win11)
[F20Drv] Inject ENABLED: targets=N cb=...
[F20Drv] WARN: REMINDER 1/2: NumLock MUST be ON ...
[F20Drv] WARN: REMINDER 2/2: Paste this in CS2 console once ...
[F20Drv] Found cs2.exe PID=0x...
[F20Drv] KILL RK=2->3  P hold=2500ms  tap scan=0x67 sign=NEG magIdx=6 tap=55ms
[F20Drv] KILL RK=3->4  P hold=2500ms  tap scan=0x4F sign=POS magIdx=0 tap=55ms
[F20Drv] P up
```

> 🛠️ Если `KILL` в логе есть, но cheat не реагирует → проверь bind-table в чите. Если `KILL` нет → проблема в CS2 offsets или kill detection (`Cs2DwLocalPlayerController` устарел).

---

### 8. IsValveDS — первый запуск

```text
1. Запусти CS2 и зайди в матч (в главном меню GameRules NULL).
2. Запусти IsValveDS\bin\run.bat ОТ АДМИНИСТРАТОРА.
3. Дождись окна "IsValveDS Spoofer - console".
4. Используй команды (см. ниже).
```

Что делает `run.bat`:

- собирает pre-load диагностику в `bin\logs\diag_preload_*`;
- safe-unload предыдущий instance (если был);
- обновляет offsets через `update_isvalveds_offsets.ps1`;
- грузит `IsValveDS_Driver.sys` через `kdmap.exe`;
- запускает `IsValveDS_Console.exe` (auto-poll 3 sec).

**Команды консоли:**

| Команда | Действие |
| --- | --- |
| `r` / `s` | показать текущий snapshot |
| `0` / `1` | записать `m_bIsValveDS = 0/1` |
| `w 0` / `w 1` | то же |
| `stop` | попросить драйвер выгрузиться |
| `h` / `?` | help |
| `q` | закрыть консоль (драйвер остаётся в kernel) |

**Console пишет `IsValveDS_Console.log` рядом с exe** — там timestamped лог каждого WinAPI вызова, каждой команды, и unhandled exception filter (SEH / CRT invalid-param / pure-call / `std::terminate`). При баге **этот файл нужно прислать** — он почти всегда содержит точную причину.

---

### 9. IsValveDS — остановка

```text
bin\stop.bat
```
или в консоли:
```text
stop
```

Safe unload flow:
1. `unload_isvalveds.ps1` → `SetEvent("Global\IsValveDSStop")`;
2. driver снимает `PsSetCreateProcessNotifyRoutine`, закрывает SHM/events;
3. driver → `SetEvent("Global\IsValveDSStopped")`;
4. потом → `kdunmap.exe --key IsValveDS_Driver --alreadyStopped`.

---

### 10. VC++ runtime

Kits ship app-local DLLs рядом с `kdmap.exe` / `kdunmap.exe`:

| DLL | Размер | Назначение |
| --- | --- | --- |
| `msvcp140.dll` | ~550 KB | C++ standard library |
| `vcruntime140.dll` | ~125 KB | base C runtime |
| `vcruntime140_1.dll` | ~50 KB | C runtime extension |
| `concrt140.dll` | ~325 KB | concurrency runtime |

**На стоковой Win10/11 install kit работает БЕЗ установленного VC Redist.** IsValveDS_Console.exe + analyze_kbdclass.exe собраны `/MT` (static CRT), вообще без зависимостей на эти DLL.

Если DLL были quarantined AV или хочется системного install — запусти `install_vcredist.bat`. Он предложит:

| Mode | Источник |
| --- | --- |
| **M** (default) | Microsoft VC++ 2015-2022 x64 Redistributable, `https://aka.ms/vs/17/release/vc_redist.x64.exe` |
| **A** (AIO) | `abbodi1406/vcredist` GitHub release, `VisualCppRedist_AIO_x86_x64.exe`, CLI `/y` |
| **Q** | cancel |

В репозитории **не** хранится сторонний `.exe`.

---

### 11. Сборка из исходников

Требования:
- Visual Studio 2022 (C++ workload)
- Windows SDK / WDK 10.0.26100.x (NuGet packages)
- PowerShell 5+
- `TheCruZ/kdmapper` склонирован рядом с репозиторием как `..\kdmapper`

Одна команда из корня:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_release.ps1
```

Что делает:
- собирает F20Driver, analyze_kbdclass, kdmap, kdunmap, IsValveDS_Driver, IsValveDS_Console;
- копирует app-local VC++ runtime DLLs в kits;
- синхронизирует свежие binaries в `kits/F20Kit/` и `kits/IsValveDS/bin/`;
- пересобирает `F20Kit.zip` и `IsValveDS_spoofer.zip`;
- печатает SHA256 архивов.

---

### 12. GitHub Actions / релизы

Workflow `.github/workflows/build-release.yml` делает то же самое на runner:

- восстанавливает WDK/SDK NuGet packages;
- клонирует и собирает `TheCruZ/kdmapper` как static lib;
- собирает все 6 проектов;
- пересобирает zip-ы;
- публикует artifacts на каждый build;
- при push tag `v*` или manual run с `publish_release=true` — прикрепляет zip к GitHub Release.

Публикация релиза из git:

```powershell
git tag v1.2
git push origin v1.2
```

---

### 13. Что присылать при проблемах

**F20Kit:**
- `F20Kit\START_LAST.log` + `F20Kit\logs\start_<latest>.log`
- `F20Kit\logs\diag_preload_<latest>` (целиком, или zip)
- `F20Kit\logs\stop_<latest>` если падало на STOP
- DebugView log с фильтром `*F20Drv*`
- minidump из `C:\Windows\Minidump\` если был BSOD

**IsValveDS:**
- `IsValveDS\bin\logs\diag_preload_<latest>`
- `IsValveDS\bin\IsValveDS_Console.log` ⭐ **обязательно** — там детальный причину error содержит
- `IsValveDS\bin\run_LAST.log`
- DebugView log с фильтром `*IsVDS*`
- minidump если был BSOD

---

### 14. Частые ошибки

| Симптом | Причина | Фикс |
| --- | --- | --- |
| `STATUS_IMAGE_CERT_REVOKED / 0xC0000603` | VulnerableDriverBlocklist блокирует `iqvw64e` | `preflight.bat` → reboot |
| `Device\Nal already in use` | Lingering Intel vulnerable driver | reboot |
| `STATUS_ACCESS_DENIED / 0xC0000022` | AntiCheat (vgc/EAC/FACEIT) запущен | `START.bat` сам убьёт; FACEIT иногда запускается даже когда игра закрыта |
| `F20Drv: Inject DISABLED` | analyzer не нашёл safe RVA | Monitor-only mode — пришли `diag_collect.bat` zip разработчику |
| Numpad работает как стрелки | NumLock = OFF | Включи NumLock |
| `IsVDS: GameRules NULL` | Главное меню или сервер не создал `C_CSGameRules` | Зайди в матч |
| Console мгновенно exit | Старый build без CONIN$ reader | Качай latest release (CONIN$ + static CRT) |
| `Cannot open SHM` | Драйвер не загружен ИЛИ namespace race | Проверь `IsValveDS_Console.log` + DebugView |

---

<details>
<summary><b>📦 Layout одного kit'а после распаковки (F20Kit)</b></summary>

```text
F20Kit\
├─ F20Driver.sys             ← kernel driver (v10)
├─ START.bat                 ← elevated launcher
├─ STOP.bat                  ← safe stop + tracked unmap
├─ preflight.bat             ← read-only system check
├─ diag_collect.bat          ← post-mortem diagnostic collector
├─ analyze_kbdclass.exe      ← PDB-based callback resolver (/MT, no deps)
├─ kdmap.exe                 ← tracked mapper (/MD + app-local DLLs)
├─ kdunmap.exe               ← tracked unmapper
├─ install_vcredist.bat      ← optional VC Redist installer (M/A/Q)
├─ cleanup_f20_state.ps1
├─ unload_f20.ps1
├─ update_cs2_offsets.ps1
├─ msvcp140.dll              ← app-local VC++ runtime
├─ vcruntime140.dll
├─ vcruntime140_1.dll
├─ concrt140.dll
├─ README.txt
└─ ИНСТРУКЦИЯ.txt
```

</details>

---

## [EN] Full Guide

### 📑 Table of contents

1. [Overview](#1-overview)
2. [Download](#2-download)
3. [System requirements](#3-system-requirements)
4. [F20Kit — first run](#4-f20kit--first-run)
5. [F20Kit — yaw bind table](#5-f20kit--yaw-bind-table)
6. [F20Kit — stop](#6-f20kit--stop)
7. [F20Kit — diagnostics](#7-f20kit--diagnostics)
8. [IsValveDS — first run](#8-isvalveds--first-run)
9. [IsValveDS — stop](#9-isvalveds--stop)
10. [VC++ runtime](#10-vc-runtime-en)
11. [Build from source](#11-build-from-source)
12. [GitHub Actions / releases](#12-github-actions--releases-en)
13. [What to send when something breaks](#13-what-to-send-when-something-breaks)
14. [Common errors](#14-common-errors)

---

### 1. Overview

| Kit | Purpose |
| --- | --- |
| 🎯 **F20Kit** | Per kill: P-down → wait 2.2 s → tap one of 22 keys (Numpad 0-9 + F13-F24, yaw `[-35..+35]`) → wait 0.3 s → P-up. The tap fires **300 ms BEFORE P-up**, while the kill-action key is still held, so the cheat's MOUSE OVERRIDE applies the yaw change at the right frame. Sign flips every kill (POS ↔ NEG); magnitude is uniform-random out of 10 candidates (skips the symmetric counterpart of the previous tap, so view never sums back to 0). |
| 🪪 **IsValveDS** | Spoof `C_CSGameRules::m_bIsValveDS` from kernel mode, driven by a user-mode SHM console. |

Repository layout:

```text
NL_Drive_CS2/
├─ src/
│  ├─ drivers/F20Driver/
│  ├─ drivers/IsValveDS/
│  ├─ apps/IsValveDSConsole/
│  └─ tools/
│     ├─ analyze_kbdclass/
│     ├─ kdmap/
│     └─ kdunmap/
├─ kits/
│  ├─ F20Kit/
│  └─ IsValveDS/
├─ scripts/build_release.ps1
└─ .github/workflows/
```

---

### 2. Download

🔗 [github.com/ccsimplyspolit/NL_Drive_CS2/releases/latest](https://github.com/ccsimplyspolit/NL_Drive_CS2/releases/latest)

| Archive | Contents |
| --- | --- |
| `F20Kit.zip` | F20Driver.sys + START/STOP bats + analyzer + kdmap/kdunmap + PS1 scripts + app-local VC++ runtime + install_vcredist.bat |
| `IsValveDS_spoofer.zip` | IsValveDS_Driver.sys + IsValveDS_Console.exe + run/stop bats + kdmap/kdunmap + PS1 scripts + app-local VC++ runtime + install_vcredist.bat |

Extract to a short path without special characters, e.g. `C:\NL_Drive_CS2\F20Kit`.

> ⚠️ **Always use the latest release.** Old tags are kept for history/diff only.

---

### 3. System requirements

| Component | Requirement | Auto-fixed? |
| --- | --- | --- |
| Windows | 10/11 x64 | — |
| Privileges | Admin | Launcher self-elevates |
| Secure Boot | OFF | ❌ BIOS/UEFI only |
| HVCI / Memory Integrity | OFF | ✅ START.bat |
| VulnerableDriverBlocklist | `= 0` | ✅ START.bat |
| AntiCheat (vgc/EAC/BE/FACEIT) | not running | ✅ START.bat |
| `iqvw64e` lingering | not loaded | ✅ START.bat |

> 🛡️ F20Driver resolves `kbdclass!KeyboardClassServiceCallback` via Microsoft Symbol Server (PDB GUID/Age), falls back to byte-signature only as a safety net, and refuses to inject if neither path is safe (monitor-only mode, no BSOD).

---

### 4. F20Kit — first run

```text
1. Launch CS2 (Fullscreen Windowed).
2. Enable NumLock (LED lit).
3. Run F20Kit\START.bat AS ADMINISTRATOR.
4. Wait for "Driver loaded".
5. Open CS2 console (~) and paste the one-line unbind shown by START.bat.
6. Configure the yaw bind table in your cheat (see section 5).
7. Play.
```

What `START.bat` does:

- creates `logs\start_<timestamp>.log` + `START_LAST.log` alias;
- collects full pre-load diagnostics in `logs\diag_preload_*`;
- safe-stops previous instance + tracked kdunmap;
- updates CS2 offsets through `update_cs2_offsets.ps1` (TLS 1.2/1.3 enforced);
- runs `analyze_kbdclass.exe` (PDB resolution);
- maps `F20Driver.sys` via `kdmap.exe --key F20Driver --indPages`;
- prints the ready unbind line + NumLock reminder.

---

### 5. F20Kit — yaw bind table

The driver picks the key; your cheat's MOUSE OVERRIDE / "Local view" yaw binds **must mirror this table exactly**:

| 🟢 POSITIVE | scan | yaw | | 🔴 NEGATIVE | scan | yaw |
| --- | --- | --- | --- | --- | --- | --- |
| Num1 | `0x4F` | **+1°** | | Num0 | `0x52` | **−1°** |
| Num2 | `0x50` | **+4°** | | Num3 | `0x51` | **−4°** |
| Num4 | `0x4B` | **+8°** | | Num5 | `0x4C` | **−8°** |
| Num6 | `0x4D` | **+11°** | | Num7 | `0x47` | **−11°** |
| Num8 | `0x48` | **+15°** | | Num9 | `0x49` | **−15°** |
| F13 | `0x64` | **+18°** | | F14 | `0x65` | **−18°** |
| F15 | `0x66` | **+21°** | | F16 | `0x67` | **−21°** |
| F17 | `0x68` | **+25°** | | F18 | `0x69` | **−25°** |
| F19 | `0x6A` | **+28°** | | F20 | `0x6B` | **−28°** |
| F21 | `0x6C` | **+32°** | | F22 | `0x6D` | **−32°** |
| F23 | `0x6E` | **+35°** | | F24 | `0x76` | **−35°** |

Hold key: **P** (scan `0x19`), 2500 ms.

**Pick logic:**
1. Every kill flips the sign (POS ↔ NEG).
2. Magnitude is uniform random out of **10 candidates** from the opposite-sign pool (skipping the symmetric counterpart of the previous tap).
3. The view never sums back to 0 (no `+M / −M` pair).

#### One-line CS2 console paste (once per session)

```text
unbind p; unbind F13; unbind F14; unbind F15; unbind F16; unbind F17; unbind F18; unbind F19; unbind F20; unbind F21; unbind F22; unbind F23; unbind F24; unbind KP_INS; unbind KP_END; unbind KP_DOWNARROW; unbind KP_PGDN; unbind KP_LEFTARROW; unbind KP_5; unbind KP_RIGHTARROW; unbind KP_HOME; unbind KP_UPARROW; unbind KP_PGUP
```

> ⚠️ **NumLock = ON** is mandatory; otherwise scan codes `0x47..0x52` are reported as nav-cluster (`Home/End/arrows/Ins/PgUp/PgDn`) and your cheat will get the wrong key.

---

### 6. F20Kit — stop

```text
STOP.bat
```

Flow:
1. signals `Global\F20DriverStop`;
2. waits for `Global\F20DriverStopped` (worker confirms cleanup);
3. only on confirmation → `kdunmap.exe --key F20Driver --alreadyStopped`.

If worker-exit is not confirmed, the script refuses blind unmap and asks for a reboot.

---

### 7. F20Kit — diagnostics

Logs:

```text
F20Kit\START_LAST.log
F20Kit\logs\start_*.log
F20Kit\logs\diag_preload_*
F20Kit\logs\stop_*
```

**DebugView filter:** `*F20Drv*`

Expected log lines:

```text
[F20Drv] F20Driver v10 (P hold + 22-key yaw pool spanning [-35..+35])
[F20Drv] OS: 10.0 build 26200 (Win11)
[F20Drv] Inject ENABLED: targets=N cb=...
[F20Drv] WARN: REMINDER 1/2: NumLock MUST be ON ...
[F20Drv] WARN: REMINDER 2/2: Paste this in CS2 console once ...
[F20Drv] KILL RK=2->3  P hold=2500ms  tap scan=0x67 sign=NEG magIdx=6 tap=55ms
[F20Drv] P up
```

---

### 8. IsValveDS — first run

```text
1. Launch CS2 and join a match (in main menu GameRules is NULL).
2. Run IsValveDS\bin\run.bat AS ADMINISTRATOR.
3. Wait for "IsValveDS Spoofer - console" window.
4. Use the commands below.
```

Console commands:

| Cmd | Action |
| --- | --- |
| `r` / `s` | show current snapshot |
| `0` / `1` | write `m_bIsValveDS = 0/1` |
| `w 0` / `w 1` | same |
| `stop` | ask driver to release SHM/event |
| `h` / `?` | help |
| `q` | close console (driver stays loaded) |

**`IsValveDS_Console.log` is written next to the exe** — full timestamped log of every WinAPI call, every command, plus an unhandled-exception filter (SEH / CRT invalid-param / pure-call / `std::terminate`). Send this file with any bug report; it almost always pinpoints the root cause.

---

### 9. IsValveDS — stop

```text
bin\stop.bat
```
or `stop` in the console.

Unload flow:
1. `unload_isvalveds.ps1` → `SetEvent("Global\IsValveDSStop")`;
2. driver unregisters process-notify, closes SHM/events;
3. driver → `SetEvent("Global\IsValveDSStopped")`;
4. then → `kdunmap.exe --key IsValveDS_Driver --alreadyStopped`.

---

### 10. VC++ runtime (EN)

Kits ship four app-local DLLs next to `kdmap.exe` / `kdunmap.exe`:

| DLL | Size | Purpose |
| --- | --- | --- |
| `msvcp140.dll` | ~550 KB | C++ standard library |
| `vcruntime140.dll` | ~125 KB | base C runtime |
| `vcruntime140_1.dll` | ~50 KB | C runtime extension |
| `concrt140.dll` | ~325 KB | concurrency runtime |

The kits **work on a clean Windows 10/11 install without VC++ Redistributable**. `IsValveDS_Console.exe` and `analyze_kbdclass.exe` are linked with `/MT` and have zero CRT DLL dependencies.

If the DLLs get quarantined or you want a system-wide install, run `install_vcredist.bat`. Two sources:

| Mode | Source |
| --- | --- |
| **M** (default) | Microsoft VC++ 2015-2022 x64 Redistributable, `aka.ms/vs/17/release/vc_redist.x64.exe` |
| **A** (AIO) | `abbodi1406/vcredist` GitHub release, `/y` CLI |

No third-party installer is bundled in the repo.

---

### 11. Build from source

Requirements:
- Visual Studio 2022 (C++ workload)
- Windows SDK / WDK 10.0.26100.x (NuGet)
- PowerShell 5+
- `TheCruZ/kdmapper` checked out as `..\kdmapper`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_release.ps1
```

Builds all 6 projects, copies app-local runtime, syncs binaries into `kits/`, rebuilds both release zips and prints SHA256 checksums.

---

### 12. GitHub Actions / releases (EN)

`.github/workflows/build-release.yml`:
- restores WDK/SDK NuGet packages;
- builds `kdmapper` static library;
- builds all drivers / tools / consoles;
- packages both release zips;
- uploads artifacts on every build;
- attaches zips to a GitHub Release on `v*` tag push or manual run with `publish_release=true`.

```powershell
git tag v1.2
git push origin v1.2
```

---

### 13. What to send when something breaks

**F20Kit:**
- `F20Kit\START_LAST.log` + latest `F20Kit\logs\start_*.log`
- `F20Kit\logs\diag_preload_<latest>` (folder or zip)
- `F20Kit\logs\stop_*` if STOP failed
- DebugView log with `*F20Drv*` filter
- minidump from `C:\Windows\Minidump\` if BSOD

**IsValveDS:**
- `IsValveDS\bin\logs\diag_preload_<latest>`
- `IsValveDS\bin\IsValveDS_Console.log` ⭐ **mandatory** — contains exact failure reason
- `IsValveDS\bin\run_LAST.log`
- DebugView log with `*IsVDS*` filter
- minidump if BSOD

---

### 14. Common errors

| Symptom | Cause | Fix |
| --- | --- | --- |
| `STATUS_IMAGE_CERT_REVOKED / 0xC0000603` | VulnerableDriverBlocklist blocks `iqvw64e` | `preflight.bat` → reboot |
| `Device\Nal already in use` | Lingering Intel vulnerable driver | reboot |
| `STATUS_ACCESS_DENIED / 0xC0000022` | AntiCheat (vgc/EAC/FACEIT) running | `START.bat` kills them; FACEIT may run even with game closed |
| `Inject DISABLED` | analyzer found no safe RVA | Monitor-only mode — send `diag_collect.bat` zip |
| Numpad acts like arrows | NumLock OFF | Enable NumLock |
| `GameRules NULL` | Main menu or server not started | Join a match |
| Console exits immediately | Old build without CONIN$ reader | Download the latest release |
| `Cannot open SHM` | Driver not loaded OR namespace race | Check `IsValveDS_Console.log` + DebugView |

---

<div align="center">

📦 [**Latest release**](https://github.com/ccsimplyspolit/NL_Drive_CS2/releases/latest) · 📂 [Repository](https://github.com/ccsimplyspolit/NL_Drive_CS2) · 🐛 [Issues](https://github.com/ccsimplyspolit/NL_Drive_CS2/issues)

</div>
