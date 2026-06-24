# NL_Drive_CS2

Набор из двух kernel-mode проектов для CS2 с безопасным запуском/остановкой,
pre-load диагностикой и автоматическим обновлением нужных offsets.

## Проекты

### F20Kit / F20Driver

`F20Driver` следит за `m_iNumRoundKills` в `cs2.exe` и при новом kill
инжектит F20 через `kbdclass!KeyboardClassServiceCallback`.

Ключевые вещи:

- `F20Kit\START.bat` собирает diagnostics до загрузки драйвера и оставляет окно
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

- `bin\run.bat` собирает pre-load diagnostics, обновляет offsets и грузит
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

Собрать драйверы:

```powershell
msbuild MyDriver2\F20Driver\F20Driver.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild "isvalveDS spoofer\IsValveDS_Driver\IsValveDS_Driver.vcxproj" /p:Configuration=Release /p:Platform=x64
msbuild "isvalveDS spoofer\IsValveDS_Console\IsValveDS_Console.vcxproj" /p:Configuration=Release /p:Platform=x64
msbuild kdmap_unmap\analyze_kbdclass\analyze_kbdclass.vcxproj /p:Configuration=Release /p:Platform=x64
```

Готовые локальные пакеты:

- `F20Kit\F20Kit.zip`
- `isvalveDS spoofer\IsValveDS_spoofer.zip`

## Диагностика

При запуске bat-файлы складывают диагностику в:

- `F20Kit\logs\diag_preload_*`
- `isvalveDS spoofer\bin\logs\diag_preload_*`

В zip и git эти runtime logs не добавляются.
