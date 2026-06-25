F20Kit - kernel-mode CS2 round-kill detector (headless)
========================================================

Что делает
----------
Драйвер мониторит m_iNumRoundKills в cs2.exe (kernel-mode чтение памяти
через MmCopyVirtualMemory). На КАЖДЫЙ кил делает три вещи:

  1. Жмёт P СРАЗУ после kill и держит РАНДОМНО в диапазоне 1500..3000 ms.
     Длительность держания выбирается uniform-random на каждый kill, чтобы
     pattern не был периодическим. Кили в течение этого окна игнорируются -
     это и есть cooldown.
  2. За 245..350 ms (тоже рандом на каждый kill) ДО отжатия P нажимает
     клавишу из 22-клавишного tap-pool. Сама клавиша держится 55 ms
     (down -> up). Таким образом yaw-tap всегда происходит пока P ещё
     удерживается - cheat применяет MOUSE OVERRIDE в нужный момент.
  3. По истечению рандомного hold-времени P отпускается.

Tap pool привязан к yaw-биндам [-35..+35] в чите (см. таблицу ниже).
Каждый kill меняет знак yaw (POS <-> NEG). Magnitude выбирается из 11
с исключением последних 3..8 magnitudes (random window каждый pick,
независимо от знака) - одна и та же величина не повторится в окне
3..8 kills. Никаких +18/-18, никаких +18 через 2 kill'а после +18.

Таблица yaw-биндов в чите (должна совпадать с этим mapping-ом!):

  POSITIVE                 NEGATIVE
  Num1 -> +1               Num0 ->  -1
  Num2 -> +4               Num3 ->  -4
  Num4 -> +8               Num5 ->  -8
  Num6 -> +11              Num7 -> -11
  Num8 -> +15              Num9 -> -15
  F13  -> +18              F14  -> -18
  F15  -> +21              F16  -> -21
  F17  -> +25              F18  -> -25
  F19  -> +28              F20  -> -28
  F21  -> +32              F22  -> -32
  F23  -> +35              F24  -> -35

ВАЖНО: NumLock должен быть включён ВРУЧНУЮ перед запуском!
Драйвер больше НЕ переключает NumLock сам (LED-индикатор и Win32k state
расходятся - получался дребезг). Если NumLock OFF, то скан-коды
0x47..0x52 будут восприниматься как nav-кластер (Home/End/стрелки/Ins/PgUp/PgDn).

ПЕРЕД ИГРОЙ: открой консоль CS2 (~) и вставь ЭТУ ОДНУ СТРОКУ, чтобы CS2
не реагировал на наши клавиши сам (мы их используем только как
триггер для чита через MOUSE OVERRIDE):

  unbind p; unbind F13; unbind F14; unbind F15; unbind F16; unbind F17; unbind F18; unbind F19; unbind F20; unbind F21; unbind F22; unbind F23; unbind F24; unbind KP_INS; unbind KP_END; unbind KP_DOWNARROW; unbind KP_PGDN; unbind KP_LEFTARROW; unbind KP_5; unbind KP_RIGHTARROW; unbind KP_HOME; unbind KP_UPARROW; unbind KP_PGUP

START.bat выводит это же сообщение после загрузки драйвера, чтобы было
удобно скопировать.

Никаких хоткеев, никакого оверлея, никакого SHM, никакого numpad-cycle.
Драйвер только СЛУШАЕТ убийства (через память) и только ЖМЁТ клавиши.

Запуск
------
  1. Запусти CS2 (Fullscreen Windowed для engine_no_focus_sleep 0)
  2. Двойной клик: START.bat
     - запросит права администратора
     - АВТО-ФИКС: VulnerableDriverBlocklist, HVCI, AC процессы, iqvw64e
     - если применил registry-фиксы -> авто-ребут через 15 сек
     - скачает свежие CS2 offsets с github:a2x/cs2-dumper
     - запустит нативный analyze_kbdclass.exe (без Python!)
     - загрузит драйвер через kdmap.exe (tracked для true unload)
  3. В CS2 консоли:   unbind F20
  4. NumLock = ON на клавиатуре перед игрой

Требования
----------
  - Windows 10/11 x64
  - Secure Boot OFF (отключается только через BIOS, START.bat предупредит)
  - Python НЕ нужен - analyze_kbdclass.exe нативный C++
  - VC++ runtime обычно НЕ нужно ставить вручную: DLL кладутся рядом с exe.
    Если DLL удалены/заблокированы, START.bat спросит и запустит
    install_vcredist.bat.

Файлы в папке
-------------
  F20Driver.sys        - драйвер
  kdmap.exe            - tracked load (kdmapper-lib + запись base/size/event
                         в HKLM\SOFTWARE\kdmap_tracker\F20Driver)
  kdunmap.exe          - TRUE unload: читает tracking-record и освобождает память
                         только после подтверждённого worker-exit
  analyze_kbdclass.exe - нативный C++ анализатор (без Python),
                         ищет KbdClassServiceCallback RVA, пишет в registry
  START.bat            - auto-fix всего + analyzer + kdmap (всё в одном)
  STOP.bat             - сначала soft-stop + worker-exit confirmation,
                         потом kdunmap --alreadyStopped; иначе reboot prompt
  cleanup_f20_state.ps1 - чистит наши runtime keys/services после STOP и
                         перед новым analyzer-запуском
  update_cs2_offsets.ps1 - скачивает output/offsets.json и client_dll.json
                         из a2x/cs2-dumper, пишет CS2 offsets в registry
  preflight.bat        - read-only диагностика (без авто-фиксов)
  unload_f20.ps1       - legacy PS1, soft signal Global\F20DriverStop
  install_vcredist.bat - установка VC++ runtime по запросу (Microsoft или AIO)
  diag_collect.bat     - сборщик диагностики (см. ниже)
  README.txt           - этот файл
  ИНСТРУКЦИЯ.txt       - подробная русская инструкция

Управление (новый pipeline)
----------------------------
  START.bat - сначала безопасно останавливает предыдущий инстанс,
              потом kdunmap --alreadyStopped, потом analyzer, потом
              kdmap --indPages с записью в HKLM\SOFTWARE\kdmap_tracker\F20Driver.
  STOP.bat  - safe stop -> confirmed worker exit -> kdunmap:
              - exit 0 -> "полная выгрузка завершена, ребут не нужен"
              - нет live stop event -> stale tracker удаляется, blind free нет
              - worker не подтвердил выход -> kdunmap не запускается, нужен ребут
              - после STOP чистятся analyzer/runtime значения
                HKLM\SOFTWARE\F20Driver,
                HKLM\SOFTWARE\kdmap_tracker\F20Driver (если безопасно),
                сервисы F20Driver/iqvw64e

  START.bat перед analyzer очищает только CallbackRva/KbdTimestamp/
  KbdImageSize/KbdSha256/Signature. CS2 offsets НЕ удаляются заранее:
  если GitHub недоступен, остаются старые рабочие значения. Если скачивание
  и sanity-check успешны, update_cs2_offsets.ps1 перезаписывает registry
  свежими offsets, потом analyzer заново пишет CallbackRva, timestamp,
  image size и sha256 как при первом запуске.
  HVCI/VulnerableDriverBlocklist назад не включаются: это системные политики,
  их откат потребует reboot и может снова заблокировать загрузку.

  Что значит "true unload":
  kdunmap.exe ЗАНОВО грузит iqvw64e.sys (тот же exploit что и kdmap), читает
  base+size из реестра и дёргает MmFreeIndependentPages только после того,
  как unload_f20.ps1 получил Global\F20DriverStopped. После этого страницы
  возвращаются в NonPagedPool.

Диагностика
-----------
  - DebugView (Sysinternals), Capture -> Capture Kernel
  - В выводе ищи строки "[F20Drv]"
  - При старте драйвер пишет:
       [F20Drv] OS: 10.0 build NNNNN (Win10|Win11)
       [F20Drv] Registry HIT: RVA=0xNNNN validated by sig [name]
    или
       [F20Drv] Registry RVA 0xNNNN prologue doesn't match any KCSC signature - REJECT
       [F20Drv] Driver will run in monitor-only mode (no inject, no BSOD)
  - На kill будет лог:
       [F20Drv] KILL RK=N->M  F20 hold=2500ms  Num scan=0xNN idx=K tap=55ms
    Если numpad не доходит до игры, ищи рядом WARN:
       [F20Drv] WARN: Inject scan=0xNN flags=0xX target=N unit=N consumed=0
  - Если анализатор не нашёл уникальный паттерн -> driver runs in
    monitor-only mode (нет инжекта, нет BSOD).

BSOD на Win10 (или другом неподдерживаемом билде)
-------------------------------------------------
В новых сборках драйвер ДОЛЖЕН сам отказаться от инжекта если
analyzer записал в реестр сомнительный RVA (см. лог "REJECT").
Если у тебя/друга всё равно БСОДит:

  1. На проблемной машине запусти diag_collect.bat (от админа).
  2. Пришли получившийся f20_diag_*.zip разработчику.
  3. В zip есть:
       - kbdclass.sys (для добавления sha256 -> known-good RVA)
       - analyze_output.txt (что нашёл анализатор)
       - osinfo.txt (версия Windows, билд)
       - registry_state.txt (что записал анализатор)
       - bugcheck_events.txt (последние BSOD события)
  4. Разработчик добавит запись в g_KnownKbdclass[] и соберёт fix-сборку.

kdmap / kdmapper-lib - параметры и шпаргалка
--------------------------------------------
kdmap.exe в этом kit'е - tracked loader на базе kdmapper-lib.
Поддерживает Win10 1607 -> Win11 25H2.

Параметры:

  --free
      Размапить память драйвера сразу после возврата из DriverEntry.
      *** НЕ ИСПОЛЬЗОВАТЬ для F20Driver! ***
      F20Driver создаёт worker thread который работает после DriverEntry.
      Если код размапится - worker fault'нет -> BSOD.
      --free подходит ТОЛЬКО для drivers где вся работа делается в DriverEntry.

  --indPages   *** ВКЛЮЧЁН ПО УМОЛЧАНИЮ в START.bat ***
      Использовать MmAllocateIndependentPagesEx вместо ExAllocatePool.
      Скрывает выделение из BigPoolTable (плюс OPSEC). Безопасно для F20Driver -
      lifetime allocation тот же (до ребута), поведение DriverEntry/worker не
      меняется. Если на каком-то билде с этим флагом будут проблемы - убери
      "--indPages" из строки kdmap в START.bat.

  --PassAllocationPtr
      Передаёт allocation pointer первым аргументом в DriverEntry.
      Полезно если драйверу нужно знать свой базовый адрес для self-patch.
      F20Driver не использует - не нужно.

  --copy-header
      Копирует PE-header в kernel-память. По умолчанию header НЕ копируется
      (защита от обнаружения через scan PE-headers в kernel pool).
      F20Driver - не нужно.

Вызов в START.bat (по умолчанию):
    kdmap.exe --key F20Driver --stopEvent "Global\F20DriverStop" --indPages F20Driver.sys

Если на каком-то билде Windows будут проблемы с --indPages, можно откатить
на стандартный путь:
    kdmap.exe --key F20Driver --stopEvent "Global\F20DriverStop" F20Driver.sys

Типичные ошибки kdmap/kdmapper-lib:

  "\\Device\\Nal already in use"
      Предыдущий instance kdmap висит, или Intel driver iqvw64e.sys уже
      загружен. Перезагрузка решает. Можно вручную выгрузить iqvw64e.sys.

  STATUS 0xC0000022 / 0xC000009A
      Антивирус / AntiCheat блокирует. Некоторые AC (FACEIT) запущены даже
      когда игра закрыта - проверь Task Manager.

  STATUS 0xC0000603 (STATUS_IMAGE_CERT_REVOKED)
      Сертификат iqvw64e.sys заблокирован Microsoft VulnerableDriverBlocklist.
      Чтобы загрузить: regedit
          HKLM\SYSTEM\CurrentControlSet\Control\CI\Config
          VulnerableDriverBlocklistEnable = DWORD:0
      Затем reboot.

Защиты в драйвере, чтобы не БСОДить
------------------------------------
  - Analyzer сначала ищет KeyboardClassServiceCallback через Microsoft PDB
    symbols. Такой RVA драйвер принимает как PDB-sourced после проверки
    TimeDateStamp + SizeOfImage + попадания в .text.
  - Для legacy/fallback RVA драйвер заново проверяет первые 64 байта
    против известных сигнатур пролога KeyboardClassServiceCallback.
  - Если PDB и fallback не сработали -> monitor-only mode.
  - Дополнительная защита: hash-keyed DB известных билдов (sha256 ->
    проверенный RVA). При совпадении hash используется DB-RVA, перекрывая
    то что нашёл анализатор.

Универсальность kbdclass
------------------------
  Анализатор сначала пробует PDB symbols с Microsoft Symbol Server, затем
  8 сигнатур KeyboardClassServiceCallback как offline fallback.
  Покрывает Win7 -> Win11 25H2. Драйвер использует RVA из
  HKLM\SOFTWARE\F20Driver если анализатор успешно отработал; проверяет
  TimeDateStamp + SizeOfImage и что RVA попадает в .text. Если что-то
  не сходится -> инжект ОТКЛЮЧЕН (без BSOD), только мониторинг.
