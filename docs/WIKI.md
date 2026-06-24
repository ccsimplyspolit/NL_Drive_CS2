# NL_Drive_CS2 Wiki

> [RU] Полная инструкция на русском.
> [EN] Full English guide.

---

## [RU] Полная инструкция

### 1. Что лежит в репозитории

```text
NL_Drive_CS2/
  src/
    drivers/
      F20Driver/          исходники kernel driver для F20
      IsValveDS/          исходники kernel driver для IsValveDS
    apps/
      IsValveDSConsole/   консоль для IsValveDS shared memory
    tools/
      analyze_kbdclass/   PDB-based analyzer для kbdclass.sys
      kdmap/              tracked mapper
      kdunmap/            tracked unmapper
      common.h
  kits/
    F20Kit/               готовый kit для запуска F20Driver
    IsValveDS/            готовый kit для запуска IsValveDS
  scripts/
    build_release.ps1     сборка всех проектов + упаковка zip
  tools/
    kbdclass/             developer-only Winbindex/bulk helpers
```

### 2. Скачать готовые файлы

Открой последний GitHub Release:

```text
https://github.com/ccsimplyspolit/NL_Drive_CS2/releases
```

Скачай нужный архив:

- `F20Kit.zip` - F20 kill trigger kit.
- `IsValveDS_spoofer.zip` - IsValveDS kit.

Распаковывай архив в короткий путь без кириллицы и спецсимволов, например:

```text
C:\NL_Drive_CS2\F20Kit
C:\NL_Drive_CS2\IsValveDS
```

### 3. Общие требования перед запуском

- Windows 10/11 x64.
- Запуск bat-файлов от администратора.
- Secure Boot выключен в BIOS/UEFI.
- HVCI / Memory Integrity выключен.
- Microsoft Vulnerable Driver Blocklist выключен, если он блокирует `iqvw64e`.
- Антивирусы и anti-cheat процессы могут блокировать mapper. `preflight.bat` покажет типичные блокеры.

Важно: F20 больше не полагается на один byte-pattern для всех Windows builds.
`analyze_kbdclass.exe` читает debug directory текущего `kbdclass.sys`, получает
PDB GUID/Age и ищет `kbdclass!KeyboardClassServiceCallback` через Microsoft
Symbol Server. Если symbol/PDB недоступен и fallback не уверен, драйвер должен
уйти в monitor-only mode вместо BSOD.

### 4. F20Kit: первый запуск

1. Запусти CS2.
2. Включи NumLock, если нужны random Numpad 0-9 taps.
3. Открой `F20Kit`.
4. Запусти `preflight.bat` от администратора и исправь то, что он подсветит.
5. Запусти `START.bat` от администратора.
6. В консоли CS2 напиши:

```text
unbind F20
```

Что делает `START.bat`:

- создает timestamped log: `logs\start_*.log`;
- собирает pre-load diagnostics: `logs\diag_preload_*`;
- safe-stop предыдущий mapped instance;
- обновляет CS2 offsets через `update_cs2_offsets.ps1`;
- запускает `analyze_kbdclass.exe`;
- грузит `F20Driver.sys` через tracked `kdmap.exe`;
- оставляет окно открытым при ошибке.

Поведение F20Driver:

- читает `m_iNumRoundKills` в `cs2.exe`;
- при новом kill нажимает F20 на 2.5 секунды;
- дополнительно делает random Numpad 0-9 tap на 55 ms;
- не повторяет тот же Numpad index два раза подряд;
- если inject unsafe, работает в monitor-only mode.

### 5. F20Kit: остановка

Используй:

```text
STOP.bat
```

Что делает `STOP.bat`:

- сигналит `Global\F20DriverStop`;
- ждет `Global\F20DriverStopped`;
- только после подтвержденного cleanup запускает:

```text
kdunmap.exe --key F20Driver --alreadyStopped
```

Если worker-exit не подтвержден, скрипт не освобождает live allocation и предлагает reboot.

### 6. F20Kit: диагностика

Основные логи:

```text
F20Kit\START_LAST.log
F20Kit\logs\start_*.log
F20Kit\logs\diag_preload_*
F20Kit\logs\stop_*
```

DebugView filter:

```text
*F20Drv*
```

Ожидаемые строки:

```text
[F20Drv] F20Driver v8 (...)
[F20Drv] Registry HIT: RVA=... validated by PDB symbol
[F20Drv] KILL RK=2->3 F20 hold=2500ms Num scan=0x4F idx=1 tap=55ms
[F20Drv] F20 up
```

Если Numpad 0-9 не нажимается:

- проверь, что NumLock включен;
- найди строку `KILL ... Num scan=... tap=55ms`;
- если есть `WARN: Inject scan=... consumed=0`, пришли DebugView log и `diag_preload_*`;
- если `KILL` нет вообще, проблема в CS2 offsets или kill detection.

### 7. IsValveDS: первый запуск

1. Запусти CS2 и зайди в матч. В главном меню `GameRules` может быть `NULL`.
2. Открой `IsValveDS\bin`.
3. Запусти `preflight.bat` от администратора.
4. Запусти `run.bat` от администратора.

Что делает `run.bat`:

- собирает pre-load diagnostics в `bin\logs\diag_preload_*`;
- safe-unload предыдущий instance;
- обновляет `dwGameRules` и `m_bIsValveDS` через `update_isvalveds_offsets.ps1`;
- грузит `IsValveDS_Driver.sys` через tracked `kdmap.exe`;
- запускает `IsValveDS_Console.exe`.

Команды консоли:

```text
r        показать текущий snapshot
s        то же самое
0        записать 0
1        записать 1
w 0      записать 0
w 1      записать 1
stop     попросить драйвер выйти и освободить SHM/event
h        помощь
q        закрыть консоль, драйвер остается загружен
```

### 8. IsValveDS: остановка

Используй:

```text
stop.bat
```

или команду в консоли:

```text
stop
```

Safe unload flow:

- `unload_isvalveds.ps1` сигналит `Global\IsValveDSStop`;
- драйвер снимает process notify, закрывает SHM/events/handles;
- драйвер сигналит `Global\IsValveDSStopped`;
- только потом запускается:

```text
kdunmap.exe --key IsValveDS_Driver --alreadyStopped
```

### 9. VC++ runtime

В release-zip уже кладутся app-local DLL рядом с `kdmap.exe`/`kdunmap.exe`:

- `msvcp140.dll`;
- `vcruntime140.dll`;
- `vcruntime140_1.dll`;
- `concrt140.dll`.

Если эти DLL удалены или заблокированы антивирусом, `START.bat` / `run.bat`
спросят перед установкой VC++ runtime.

`install_vcredist.bat` предлагает два режима:

- официальный Microsoft VC++ 2015-2022 x64 Redistributable;
- VisualCppRedist AIO latest от `abbodi1406/vcredist`, скачивается с GitHub
  только по выбору пользователя и запускается с CLI `/y`.

В репозитории не хранится сторонний vcredist `.exe`.

### 10. Сборка из исходников

Требования:

- Visual Studio 2022.
- Windows SDK/WDK 10.0.26100.x.
- PowerShell 5+.
- NuGet WDK/SDK packages.
- `TheCruZ/kdmapper`, checkout рядом с репозиторием как `..\kdmapper`.

Одна команда из корня репозитория:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_release.ps1
```

Скрипт:

- собирает F20Driver;
- собирает `analyze_kbdclass`;
- собирает `kdmap`;
- собирает `kdunmap`;
- собирает IsValveDS driver;
- собирает IsValveDS console;
- копирует свежие binaries в `kits`;
- пересобирает `F20Kit.zip` и `IsValveDS_spoofer.zip`.

Если packages лежат не в `packages\`, можно передать MSBuild property
`RepoPackagesDir` при ручной сборке проекта.

### 11. GitHub Actions / релизы

Workflow `.github/workflows/build-release.yml` делает то же самое на GitHub:

- восстанавливает WDK/SDK NuGet packages;
- клонирует и собирает `TheCruZ/kdmapper` как static lib;
- собирает драйверы, tools и консоль;
- пересобирает `F20Kit.zip` и `IsValveDS_spoofer.zip`;
- публикует artifacts на обычных builds;
- прикрепляет zip к GitHub Release при push tag `v*` или manual run с
  `publish_release=true`.

Новый релиз через git:

```powershell
git tag v2
git push origin v2
```

### 12. Winbindex

Winbindex не нужен на runtime path. Он полезен только разработчику для offline
regression tests множества версий `kbdclass.sys`.

Developer helper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\kbdclass\bulk_extract_kbdclass.ps1
```

### 13. Что присылать при проблемах

Для F20:

- `F20Kit\START_LAST.log`;
- `F20Kit\logs\diag_preload_*`;
- `F20Kit\logs\stop_*`, если падало на stop;
- DebugView log с filter `*F20Drv*`;
- minidump из `C:\Windows\Minidump`, если был BSOD.

Для IsValveDS:

- `IsValveDS\bin\logs\diag_preload_*`;
- `IsValveDS\bin\logs\stop_*`;
- вывод `IsValveDS_Console.exe`;
- DebugView log с filter `*IsVDS*`;
- minidump, если был BSOD.

### 14. Частые ошибки

`STATUS_IMAGE_CERT_REVOKED / 0xC0000603`
VulnerableDriverBlocklist блокирует `iqvw64e`. Запусти `preflight.bat`, примени fix и reboot.

`Device\Nal already in use`
Завис старый Intel vulnerable driver/service. Сделай reboot или проверь stop logs.

`F20 не нажимается`
Проверь analyzer log. Если PDB/fallback не нашел безопасный callback, будет monitor-only mode.

`Numpad работает как стрелки/Home/End`
Включи NumLock.

`IsValveDS пишет GameRules NULL`
Ты в главном меню или сервер еще не создал `C_CSGameRules`. Зайди в матч.

---

## [EN] Full Guide

### 1. Repository layout

```text
NL_Drive_CS2/
  src/
    drivers/
      F20Driver/          F20 kernel driver source
      IsValveDS/          IsValveDS kernel driver source
    apps/
      IsValveDSConsole/   shared-memory console for IsValveDS
    tools/
      analyze_kbdclass/   PDB-based kbdclass analyzer
      kdmap/              tracked mapper
      kdunmap/            tracked unmapper
      common.h
  kits/
    F20Kit/               ready-to-run F20 runtime kit
    IsValveDS/            ready-to-run IsValveDS runtime kit
  scripts/
    build_release.ps1     build all projects and package release zips
  tools/
    kbdclass/             developer-only Winbindex/bulk helpers
```

### 2. Downloading prebuilt packages

Open the latest GitHub Release:

```text
https://github.com/ccsimplyspolit/NL_Drive_CS2/releases
```

Download one of:

- `F20Kit.zip` - F20 kill trigger kit.
- `IsValveDS_spoofer.zip` - IsValveDS kit.

Extract to a short path without special characters, for example:

```text
C:\NL_Drive_CS2\F20Kit
C:\NL_Drive_CS2\IsValveDS
```

### 3. Common requirements

- Windows 10/11 x64.
- Run batch files as Administrator.
- Secure Boot disabled in BIOS/UEFI.
- HVCI / Memory Integrity disabled.
- Microsoft Vulnerable Driver Blocklist disabled if it blocks `iqvw64e`.
- Antivirus or anti-cheat processes may block the mapper. `preflight.bat` reports common blockers.

F20 no longer depends on one byte-pattern for every Windows build.
`analyze_kbdclass.exe` reads the current `kbdclass.sys` debug directory,
extracts PDB GUID/Age, and resolves `kbdclass!KeyboardClassServiceCallback`
through Microsoft Symbol Server. If PDB resolution and fallback validation are
not safe, the driver should enter monitor-only mode instead of risking a BSOD.

### 4. F20Kit: first run

1. Start CS2.
2. Enable NumLock if you want random Numpad 0-9 taps.
3. Open `F20Kit`.
4. Run `preflight.bat` as Administrator and apply suggested fixes.
5. Run `START.bat` as Administrator.
6. In the CS2 console, type:

```text
unbind F20
```

`START.bat` does the following:

- creates `logs\start_*.log`;
- collects pre-load diagnostics in `logs\diag_preload_*`;
- safe-stops the previous mapped instance;
- updates CS2 offsets through `update_cs2_offsets.ps1`;
- runs `analyze_kbdclass.exe`;
- maps `F20Driver.sys` through tracked `kdmap.exe`;
- keeps the window open on failure.

F20Driver behavior:

- reads `m_iNumRoundKills` from `cs2.exe`;
- on a new kill, holds F20 for 2.5 seconds;
- also sends one random Numpad 0-9 tap for 55 ms;
- does not repeat the same Numpad index twice in a row;
- if injection is unsafe, runs in monitor-only mode.

### 5. F20Kit: stop/unload

Use:

```text
STOP.bat
```

`STOP.bat`:

- signals `Global\F20DriverStop`;
- waits for `Global\F20DriverStopped`;
- only after confirmed cleanup runs:

```text
kdunmap.exe --key F20Driver --alreadyStopped
```

If worker-exit is not confirmed, the script does not free a live allocation and
offers a reboot path instead.

### 6. F20Kit diagnostics

Main logs:

```text
F20Kit\START_LAST.log
F20Kit\logs\start_*.log
F20Kit\logs\diag_preload_*
F20Kit\logs\stop_*
```

DebugView filter:

```text
*F20Drv*
```

Expected lines:

```text
[F20Drv] F20Driver v8 (...)
[F20Drv] Registry HIT: RVA=... validated by PDB symbol
[F20Drv] KILL RK=2->3 F20 hold=2500ms Num scan=0x4F idx=1 tap=55ms
[F20Drv] F20 up
```

If Numpad 0-9 does not work:

- verify NumLock is enabled;
- look for `KILL ... Num scan=... tap=55ms`;
- if you see `WARN: Inject scan=... consumed=0`, send the DebugView log and `diag_preload_*`;
- if there is no `KILL` line, the issue is CS2 offsets or kill detection.

### 7. IsValveDS: first run

1. Start CS2 and enter a match. In the main menu, `GameRules` may be `NULL`.
2. Open `IsValveDS\bin`.
3. Run `preflight.bat` as Administrator.
4. Run `run.bat` as Administrator.

`run.bat`:

- collects pre-load diagnostics in `bin\logs\diag_preload_*`;
- safe-unloads the previous instance;
- updates `dwGameRules` and `m_bIsValveDS` through `update_isvalveds_offsets.ps1`;
- maps `IsValveDS_Driver.sys` through tracked `kdmap.exe`;
- starts `IsValveDS_Console.exe`.

Console commands:

```text
r        show current snapshot
s        same as r
0        write 0
1        write 1
w 0      write 0
w 1      write 1
stop     ask driver to release SHM/event
h        help
q        close console, driver remains loaded
```

### 8. IsValveDS: stop/unload

Use:

```text
stop.bat
```

or the console command:

```text
stop
```

Safe unload flow:

- `unload_isvalveds.ps1` signals `Global\IsValveDSStop`;
- the driver unregisters process notify and closes SHM/events/handles;
- the driver signals `Global\IsValveDSStopped`;
- only then the script runs:

```text
kdunmap.exe --key IsValveDS_Driver --alreadyStopped
```

### 9. VC++ Runtime

Release zip files already include app-local DLLs next to `kdmap.exe` /
`kdunmap.exe`:

- `msvcp140.dll`;
- `vcruntime140.dll`;
- `vcruntime140_1.dll`;
- `concrt140.dll`.

If those DLLs are deleted or quarantined, `START.bat` / `run.bat` will ask
before installing a VC++ runtime.

`install_vcredist.bat` offers two modes:

- official Microsoft VC++ 2015-2022 x64 Redistributable;
- latest VisualCppRedist AIO from `abbodi1406/vcredist`, downloaded from GitHub
  only after user selection and launched with CLI `/y`.

No third-party vcredist `.exe` is stored in this repository.

### 10. Building from source

Requirements:

- Visual Studio 2022.
- Windows SDK/WDK 10.0.26100.x.
- PowerShell 5+.
- NuGet WDK/SDK packages.
- `TheCruZ/kdmapper`, checked out next to this repository as `..\kdmapper`.

Run from repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_release.ps1
```

The script:

- builds F20Driver;
- builds `analyze_kbdclass`;
- builds `kdmap`;
- builds `kdunmap`;
- builds the IsValveDS driver;
- builds the IsValveDS console;
- copies fresh binaries into `kits`;
- rebuilds `F20Kit.zip` and `IsValveDS_spoofer.zip`.

If packages are not under `packages\`, pass the MSBuild property
`RepoPackagesDir` when building projects manually.

### 11. GitHub Actions / releases

`.github/workflows/build-release.yml` performs the same build on GitHub:

- restores WDK/SDK NuGet packages;
- clones and builds `TheCruZ/kdmapper` as a static library;
- builds the drivers, tools, and console;
- rebuilds `F20Kit.zip` and `IsValveDS_spoofer.zip`;
- uploads artifacts for normal builds;
- attaches zip files to a GitHub Release on `v*` tag pushes or manual runs with
  `publish_release=true`.

Publish a new release from git:

```powershell
git tag v2
git push origin v2
```

### 12. Winbindex

Winbindex is not required at runtime. It is only useful for developer-side
offline regression testing across many `kbdclass.sys` versions.

Developer helper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\kbdclass\bulk_extract_kbdclass.ps1
```

### 13. What to send when something breaks

For F20:

- `F20Kit\START_LAST.log`;
- `F20Kit\logs\diag_preload_*`;
- `F20Kit\logs\stop_*` if stop/unload failed;
- DebugView log with `*F20Drv*`;
- minidump from `C:\Windows\Minidump` if there was a BSOD.

For IsValveDS:

- `IsValveDS\bin\logs\diag_preload_*`;
- `IsValveDS\bin\logs\stop_*`;
- `IsValveDS_Console.exe` output;
- DebugView log with `*IsVDS*`;
- minidump if there was a BSOD.

### 14. Common errors

`STATUS_IMAGE_CERT_REVOKED / 0xC0000603`
`iqvw64e` is blocked by VulnerableDriverBlocklist. Run `preflight.bat`, apply the fix, and reboot.

`Device\Nal already in use`
An old Intel vulnerable driver/service is still present. Reboot or inspect stop logs.

`F20 does not press`
Check analyzer logs. If PDB/fallback could not resolve a safe callback, the driver will run in monitor-only mode.

`Numpad acts like arrows/Home/End`
Enable NumLock.

`IsValveDS shows GameRules NULL`
You are in the main menu or the server has not created `C_CSGameRules` yet. Enter a match.
