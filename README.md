# NL_Drive_CS2

Набор из двух kernel-mode проектов для CS2 с безопасным запуском/остановкой,
pre-load диагностикой и автоматическим обновлением нужных offsets.

## Wiki / Instructions

- [RU] Полная инструкция: [docs/WIKI.md#ru-полная-инструкция](docs/WIKI.md#ru-полная-инструкция)
- [EN] Full guide: [docs/WIKI.md#en-full-guide](docs/WIKI.md#en-full-guide)

## Структура

```text
NL_Drive_CS2/
  src/
    drivers/
      F20Driver/          # kernel driver для F20 inject
      IsValveDS/          # kernel driver для m_bIsValveDS
    apps/
      IsValveDSConsole/   # user console для IsValveDS shared memory
    tools/
      analyze_kbdclass/   # PDB-based kbdclass analyzer
      kdmap/              # tracked mapper
      kdunmap/            # tracked unmapper
      common.h
  kits/
    F20Kit/               # готовый runtime kit для F20
    IsValveDS/            # готовый runtime kit для IsValveDS
  tools/
    kbdclass/             # developer-only bulk tests/helpers
  scripts/
    build_release.ps1     # build + sync binaries + package zip
```

## Проекты

### F20Kit / F20Driver

`F20Driver` следит за `m_iNumRoundKills` в `cs2.exe` и при новом kill
инжектит F20 через `kbdclass!KeyboardClassServiceCallback`.

Ключевые вещи:

- `kits\F20Kit\START.bat` собирает diagnostics до загрузки драйвера и оставляет окно
  открытым при ошибке.
- `analyze_kbdclass.exe` сначала ищет `KeyboardClassServiceCallback` через
  Microsoft PDB symbols. Pattern scan оставлен только fallback.
- `update_cs2_offsets.ps1` скачивает свежие offsets из
  `a2x/cs2-dumper` и пишет их в `HKLM\SOFTWARE\F20Driver`.
- `STOP.bat` и повторный `START.bat` сначала ждут
  `Global\F20DriverStopped`, потом вызывают
  `kdunmap.exe --key F20Driver --alreadyStopped`.
- В CS2 console нужно написать: `unbind F20`.

### IsValveDS spoofer

`IsValveDS_Driver` читает/пишет `C_CSGameRules::m_bIsValveDS` из ядра.
`IsValveDS_Console.exe` общается с драйвером через named section/events.

Ключевые вещи:

- `kits\IsValveDS\bin\run.bat` собирает pre-load diagnostics, обновляет offsets и грузит
  драйвер через tracked `kdmap.exe`.
- `update_isvalveds_offsets.ps1` скачивает `dwGameRules` и `m_bIsValveDS`
  из `a2x/cs2-dumper` в `HKLM\SOFTWARE\IsValveDS`.
- `stop.bat`/`unload_isvalveds.ps1` сигналят `Global\IsValveDSStop`,
  ждут `Global\IsValveDSStopped`, затем запускают
  `kdunmap.exe --key IsValveDS_Driver --alreadyStopped`.
- Старый one-shot mapper не нужен для штатного запуска.

## Winbindex

Winbindex полезен как developer-only источник разных версий `kbdclass.sys` для
offline regression tests и расширения fallback hash/signature базы. Runtime
зависимости от Winbindex нет: правильный путь для F20 - PDB lookup через
Microsoft Symbol Server по debug directory конкретного `kbdclass.sys`.

## Сборка

Требования:

- Visual Studio 2022
- Windows Driver Kit / SDK 10.0.26100.x
- PowerShell 5+
- Для запуска: админ-права, Secure Boot off, совместимые CI/HVCI настройки

Одна команда собирает всё, синхронизирует бинарники в kits и пересобирает zip:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_release.ps1
```

Проекты используют NuGet WDK/SDK packages. Скрипт поддерживает текущий локальный
fallback `MyDriver2\packages\`, но для чистого clone лучше восстановить packages
в `packages\` или передать MSBuild property `RepoPackagesDir`.

Готовые локальные пакеты:

- `kits\F20Kit\F20Kit.zip`
- `kits\IsValveDS\IsValveDS_spoofer.zip`

## Диагностика

При запуске bat-файлы складывают диагностику в:

- `kits\F20Kit\logs\diag_preload_*`
- `kits\IsValveDS\bin\logs\diag_preload_*`

В zip и git эти runtime logs не добавляются.
