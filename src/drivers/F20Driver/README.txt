F20DRIVER
=========

Что делает:
  Kernel-mode драйвер. Цепляется к cs2.exe, читает m_iNumRoundKills.
  При +1 — инжектит F20 keydown через kbdclass!KeyboardClassServiceCallback,
  через 2.5 секунды — keyup, плюс один random Numpad 0-9 tap на 55 ms.
  Без оверлея, без постоянного юзермод-компонента.

Сборка (Visual Studio + WDK):
  1. Из корня репозитория:
       powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_release.ps1
  2. Либо собрать только проект:
       msbuild src\drivers\F20Driver\F20Driver.vcxproj /p:Configuration=Release /p:Platform=x64
  3. Output: src\drivers\F20Driver\x64\Release\F20Driver.sys

Загрузка:
  Используй kits\F20Kit\START.bat. Он делает safe unload предыдущего инстанса,
  обновляет CS2 offsets, запускает analyzer kbdclass и маппит драйвер
  через tracked kdmap.exe.

CS2 console:
  WRITE IN CONSOLE:
    unbind F20
  Для Numpad 0-9 должен быть включён NumLock.

Лог:
  Сообщения с префиксом [F20Drv] видны в DebugView (Sysinternals)
  при включённом флаге "Capture Kernel".

Обновление оффсетов:
  kits\F20Kit\update_cs2_offsets.ps1 скачивает свежие данные из a2x/cs2-dumper
  и пишет их в HKLM\SOFTWARE\F20Driver перед загрузкой. Если GitHub
  недоступен или sanity-check не прошёл, драйвер использует built-in fallback.

Если F20 не нажимается:
  Проверь лог analyzer/driver. analyze_kbdclass.exe сначала ищет
  kbdclass!KeyboardClassServiceCallback через Microsoft PDB symbols и пишет
  PDB-sourced RVA в registry. Pattern scan остался только offline fallback.
