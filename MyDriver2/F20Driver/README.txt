F20DRIVER
=========

Что делает:
  Kernel-mode драйвер. Цепляется к cs2.exe, читает m_iNumRoundKills.
  При +1 — инжектит F20 keydown через kbdclass!KeyboardClassServiceCallback,
  через 2.5 секунды — keyup, плюс один random Numpad 0-9 tap на 55 ms.
  Без оверлея, без постоянного юзермод-компонента.

Сборка (Visual Studio + WDK):
  1. Открыть MyDriver23\MyDriver2\MyDriver2.slnx в VS
  2. Add Existing Project → выбрать F20Driver\F20Driver.vcxproj
  3. Restore NuGet packages (используются те же что у MyDriver2)
  4. Build Release|x64 → выйдет F20Driver.sys в F20Driver\x64\Release\

Загрузка:
  Используй F20Kit\START.bat. Он делает safe unload предыдущего инстанса,
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
  F20Kit\update_cs2_offsets.ps1 скачивает свежие данные из a2x/cs2-dumper
  и пишет их в HKLM\SOFTWARE\F20Driver перед загрузкой. Если GitHub
  недоступен или sanity-check не прошёл, драйвер использует built-in fallback.

Если F20 не нажимается:
  Проверь лог analyzer/driver. analyze_kbdclass.exe сначала ищет
  kbdclass!KeyboardClassServiceCallback через Microsoft PDB symbols и пишет
  PDB-sourced RVA в registry. Pattern scan остался только offline fallback.
