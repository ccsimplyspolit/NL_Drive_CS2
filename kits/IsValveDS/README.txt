==================================================
 IsValveDS Spoofer
==================================================

Что это
-------
Подмена поля CCSGameRules::m_bIsValveDS в cs2.exe.
Две части:

  1. IsValveDS_Driver.sys  -  kernel-mode драйвер (kdmapper-style manual map).
     Поднимает named shared memory section + named event, в worker-треде
     постоянно опрашивает cs2.exe и при необходимости пишет в его память
     через MmCopyVirtualMemory.

  2. IsValveDS_Console.exe -  user-mode консоль.
     Открывает SHM и event по имени, читает текущее значение, пишет своё.

Архитектура (важно) - F20Kit-style, без IRP
--------------------------------------------
Драйвер НЕ создаёт IoCreateDevice / IoCreateSymbolicLink / IRP-dispatch.
Причина: kdmapper передаёт временный DRIVER_OBJECT; после выхода из
DriverEntry он невалиден, а IoCreateDevice сохраняет на него back-pointer,
по которому потом приходит IOCTL -> BSOD. Поэтому общение идёт через:

  Global\IsValveDSState   shared memory  (читают и драйвер, и консоль)
  Global\IsValveDSStop    named event    (консоль/PS1 -> драйвер: stop)
  Global\IsValveDSStopped named event    (драйвер -> PS1: worker cleanup done)

Все обращения к cs2.exe из драйвера - через MmCopyVirtualMemory,
без KeStackAttachProcess. PASSIVE_LEVEL, SEH-обёртки, проверки
PsGetProcessExitStatus + ProcessExitCallback на каждой итерации.
Если SHM/stop/done control-plane не создался, DriverEntry abort'ится и worker
не стартует.

Live-resync
-----------
Каждую итерацию worker-тред:
  1. Перечитывает указатель GameRules из client.dll!dwGameRules
     (он становится NULL в главном меню и repopulates в матче,
      может смениться при смене карты/сервера).
  2. Перечитывает текущий байт m_bIsValveDS.
  3. Если есть pending write_request_id - перепроверяет указатель ЕЩЁ раз,
     сравнивает с desired, и пишет ТОЛЬКО если значение реально отличается.
     После записи делает readback.
  4. cs2.exe может перезапуститься: ProcessExitCallback ставит флаг,
     при следующем заходе worker заново ищет процесс и DLL.

Структура папки
---------------
  IsValveDS_Driver/        - исходники драйвера (vcxproj, main.cpp, shared.h)
  IsValveDS_Console/       - исходники консоли  (vcxproj, main.cpp, shared.h)
  bin/                     - всё для запуска
      IsValveDS_Driver.sys
      IsValveDS_Driver.pdb
      IsValveDS_Console.exe    <- консоль (auto-poll каждые 3 сек)
      kdmap.exe                <- tracked-load маппер (kdmapper-lib + запись
                                   base/size в HKLM\SOFTWARE\kdmap_tracker)
      kdunmap.exe              <- TRUE unload: re-exploits iqvw64e и реально
                                   освобождает kernel pool без ребута
      run.bat                  - pre-load diag + offsets update + kdunmap
                                  прошлого + kdmap новый + запуск консоли
      stop.bat                 - kdunmap (без ребута если успех)
      unload_isvalveds.ps1     - soft signal Global\IsValveDSStop + wait for
                                  Global\IsValveDSStopped
      update_isvalveds_offsets.ps1
                               - скачивает dwGameRules + m_bIsValveDS и пишет
                                 их в HKLM\SOFTWARE\IsValveDS перед загрузкой

Оффсеты
-------
run.bat перед загрузкой драйвера запускает:

  bin\update_isvalveds_offsets.ps1

Скрипт скачивает свежие данные из a2x/cs2-dumper:

  https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.json
  https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.json

и пишет в HKLM\SOFTWARE\IsValveDS:

  Cs2DwGameRules      REG_QWORD
  Cs2M_bIsValveDS     REG_DWORD
  Cs2OffsetsSource    REG_SZ
  Cs2OffsetsFetchedAtUtc REG_SZ

Драйвер читает registry в DriverEntry. Если GitHub недоступен, registry пустой
или sanity-check не прошёл, используются встроенные fallback-значения:

  client.dll!dwGameRules        = 0x2341158
  CCSGameRules::m_bIsValveDS    = 0xA4

Ручная правка констант в IsValveDS_Driver/main.cpp больше не нужна для обычных
обновлений CS2.

Как запускать
-------------
1. Запусти cs2.exe.
2. Из bin/ запусти run.bat (он сам поднимется до админа):
       - убивает старую консоль (если осталась)
       - сигналит Global\IsValveDSStop (worker прошлого драйвера освобождает SHM)
       - собирает pre-load diagnostics в bin\logs\diag_preload_*
       - скачивает свежие offsets из a2x/cs2-dumper
       - маппит свежий IsValveDS_Driver.sys через kdmap.exe (с tracking)
       - запускает IsValveDS_Console.exe
3. Консоль раз в 3 сек печатает строку вида:
       [12:34:56] poll   value=0 @ 0x00007FF...  (tick=120 pid=12345)
       [12:34:59] poll * value=1 @ 0x00007FF...  <-- "*" = значение изменилось
   Команды (в любой момент):
       r / s    - снимок прямо сейчас
       0 / 1    - записать 0 или 1
       w <0|1>  - то же
       stop     - попросить драйвер отдать SHM/event (полная очистка ресурсов)
       h        - помощь
       q        - выход (драйвер остаётся в памяти ядра)

Mapper - параметры
------------------
run.bat использует только kdmap.exe с tracking-record в registry.
Старый one-shot kdmapper для этого проекта не используется: он не знает про
safe-stop и не может сделать подтверждённый kdunmap --alreadyStopped.

  --free
      *** НЕ ИСПОЛЬЗОВАТЬ для IsValveDS_Driver! ***
      Размапит код сразу после DriverEntry, но наш worker-thread продолжает
      работать -> fault -> BSOD. Подходит только для one-shot drivers.

  --indPages   *** ВКЛЮЧЁН ПО УМОЛЧАНИЮ в run.bat ***
      MmAllocateIndependentPagesEx вместо ExAllocatePool. Скрывает выделение
      из BigPoolTable (плюс OPSEC). Безопасно для нашего worker-thread
      драйвера. Если на каком-то билде проблема - убери "--indPages" из run.bat.

  --PassAllocationPtr, --copy-header
      Не нужны для IsValveDS_Driver.

Вызов в run.bat (по умолчанию):
    kdmap.exe --key IsValveDS_Driver --stopEvent "Global\IsValveDSStop" --indPages IsValveDS_Driver.sys

Ошибки mapper:
  "\\Device\\Nal already in use"  - предыдущий instance/Intel driver висит -> reboot
  0xC0000022 / 0xC000009A         - блокирует AC/AV (FACEIT даже когда игра закрыта)
  0xC0000603                      - сертификат iqvw64e в VulnerableDriverBlocklist;
                                    HKLM\SYSTEM\CurrentControlSet\Control\CI\Config
                                    VulnerableDriverBlocklistEnable = 0 + reboot

Полная "выгрузка"
-----------------
run.bat грузит драйвер через tracked kdmap, поэтому stop.bat может сначала
остановить worker, а потом через kdunmap освободить kernel allocation без
ребута. Если tracking отсутствует или kdunmap не смог освободить allocation,
безопасный fallback - reboot.

  stop.bat / `stop` в консоли / unload_isvalveds.ps1
    -> SetEvent("Global\IsValveDSStop")
    -> ждёт Global\IsValveDSStopped (или legacy disappearance stop-event)
    -> worker-тред просыпается, выходит из главного цикла, делает:
         - PsSetCreateProcessNotifyRoutine(..., TRUE)  (снять callback)
         - MmUnmapViewInSystemSpace + ZwClose section
         - ObDereferenceObject + ZwClose events
    -> kdunmap.exe --key IsValveDS_Driver --alreadyStopped
    -> Global\IsValveDSState / Stop / Stopped исчезают
    -> следующий run.bat спокойно создаст их заново для свежего .sys

Если БСОДнет - смотри
---------------------
- IsValveDS_Driver.pdb рядом с .sys (символы для WinDbg)
- бамп-фрейм !analyze -v в WinDbg покажет адрес;
  по PDB можно увидеть, где именно
- лог драйвера: DebugView (sysinternals), фильтр "IsVDS"

Сборка
------
Консоль (любая VS2022 с C++ toolset):
    msbuild "IsValveDS_Console\IsValveDS_Console.vcxproj" /p:Configuration=Release /p:Platform=x64

Драйвер (нужен установленный WDK):
    msbuild "IsValveDS_Driver\IsValveDS_Driver.vcxproj" /p:Configuration=Release /p:Platform=x64

Результаты:
    IsValveDS_Console\x64\Release\IsValveDS_Console.exe
    IsValveDS_Driver\x64\Release\IsValveDS_Driver.sys

Протокол SHM (вкратце)
----------------------
struct ISVALVEDS_STATE {
    uint32 magic;                  // ISVALVEDS_MAGIC когда current_* валидны
    int32  current_value;          // 0/1 или -1
    uint32 current_error;          // VDS_ERR_*
    uint64 current_address;        // абс. адрес m_bIsValveDS
    uint64 last_poll_systime;
    uint32 driver_tick;            // монотонно растёт
    uint32 cs2_pid;
    uint64 client_base;

    uint32 desired_value;          // <- пишет user
    uint32 write_request_id;       // <- пишет user (инкремент = "запиши")

    uint32 write_handled_id;       // <- пишет драйвер (== request_id когда сделано)
    uint32 write_error;            // VDS_ERR_* для последней записи
    int32  write_result_value;     // readback после записи
    uint64 write_handled_systime;
};

Драйвер всегда сбрасывает magic в 0, обновляет поля, ставит memory barrier,
выставляет magic = ISVALVEDS_MAGIC. Консоль читает с retry если magic порван.
