# Noctis

**English** · [Русский](#русский)

Turns the monitor off after N minutes of inactivity and wakes it on mouse movement or a key press — like the built-in Windows display timeout, but more reliable. It only looks at **your** input, so browsers, game launchers, open video tabs or drivers can't keep the screen awake. The PC keeps working: downloads, music and torrents are not interrupted.

Version 1.2.1 · Developer: **Sallador**

<img src="../docs/noctis.png" width="360">

## Turn the screen off at any moment

Noctis is more than a timer. Stepping away from your desk, listening to music in the dark, or just want the display off right now? Turn it off instantly, without waiting for the inactivity timeout:

- **Desktop button.** The installer can place a **Turn off screen** shortcut on your desktop and in the Start menu. Double-click it and the display goes dark.
- **In the app.** Use the **Turn off screen** button in the Noctis panel, or the same command in the tray icon menu.
- **Hotkey.** **Ctrl + Alt + M** works from any application.
- **Command line.** `Noctis.exe /off` — for scripts, Task Scheduler or your own shortcuts.

The screen wakes up as usual: move the mouse or press any key. Noctis switches the display off only after you release the mouse button or keys, and ignores accidental mouse jitter during the first 1.5 seconds, so the screen does not light up again by itself.

## Features

- **Inactivity timeout** — 1 / 3 / 5 / 10 / 15 / 30 minutes, or any value from 1 to 240 minutes.
- **Instant screen-off** — desktop button, app button, tray menu, Ctrl + Alt + M or `Noctis.exe /off` (see above).
- **Protection from accidental wake-up** — the screen turns off only after you release the keys or mouse button. Mouse jitter in the first 1.5 seconds turns it off again.
- **Games and video** — the screen is not turned off while a full-screen app is running. This can be disabled.
- **Protection from "alarm clocks"** — if a program, not you, wakes the screen, Noctis turns it off again.
- **Pause for 1 hour** — the tray icon dims.
- **Start with Windows** — quietly in the tray.
- **Windows 11 design** — Acrylic flyout above the tray like Quick Settings, Settings-style toggles, a live countdown with a progress bar. Follows the light/dark theme and accent colour. The monochrome tray icon matches the taskbar.
- **Light on resources** — about 2–3 MB of RAM. The window buffer is released while the flyout is closed.

## Install

Run `Noctis_Setup_1.2.1.exe` from [Releases](../../../releases). No admin rights needed. It installs to `%LOCALAPPDATA%\Programs\Noctis`. Uninstall from *Settings → Apps*.

Silent install:

```
Noctis_Setup_1.2.1.exe /S [/D=<folder>] [/nodesktop] [/noautostart] [/nolaunch]
uninstall.exe /uninstall /S
```

## Usage

The moon icon in the tray: **left click** opens settings, **right click** opens the menu.

Command line: `Noctis.exe /off` turns the screen off now; `Noctis.exe /autostart` starts silently.

## Build

Visual Studio 2022 (C++ workload) → run `build.bat`. It produces `bin\Noctis.exe` and `dist\Noctis_Setup_1.2.1.exe`.

Settings are stored in `HKCU\Software\Sallador\Noctis`.

---

<a id="русский"></a>

# Noctis

[English](#noctis) · **Русский**

Выключает монитор после N минут вашего бездействия и включает его при движении мыши или нажатии клавиши. Работает как встроенный таймер Windows, но надёжнее: смотрит только на **ваш** ввод, поэтому браузеры, игровые клиенты, открытые вкладки с видео и драйверы не мешают экрану погаснуть. Компьютер при этом продолжает работать: загрузки, музыка и торренты не прерываются.

Версия 1.2.1 · Разработчик: **Sallador**

## Выключение экрана в любой момент

Noctis — не только таймер. Отошли от компьютера, слушаете музыку в темноте или просто хотите погасить экран прямо сейчас? Выключите его мгновенно, не дожидаясь срабатывания таймера:

- **Кнопка на рабочем столе.** Установщик может разместить на рабочем столе и в меню «Пуск» ярлык **«Выключить экран»**. Двойной щелчок — и экран гаснет.
- **В приложении.** Кнопка **«Выключить экран»** в панели Noctis или одноимённый пункт в меню значка в трее.
- **Сочетание клавиш.** **Ctrl + Alt + M** работает из любого приложения.
- **Командная строка.** `Noctis.exe /off` — для скриптов, Планировщика заданий или собственных ярлыков.

Экран включается как обычно — достаточно пошевелить мышью или нажать любую клавишу. Noctis выключает экран только после того, как вы отпустите кнопку мыши или клавиши, и не реагирует на случайное дрожание мыши в первые полторы секунды, поэтому экран не загорается снова сам по себе.

## Возможности

- **Время бездействия** — 1 / 3 / 5 / 10 / 15 / 30 минут или любое значение от 1 до 240 минут.
- **Мгновенное выключение экрана** — кнопкой на рабочем столе, в приложении, через меню в трее, сочетанием Ctrl + Alt + M или командой `Noctis.exe /off` (см. выше).
- **Защита от случайного пробуждения** — экран гаснет только после того, как отпущены клавиши или кнопка мыши. Дрожание мыши в первые 1,5 секунды снова гасит экран.
- **Игры и видео** — пока открыто полноэкранное приложение, экран не выключается. Можно отключить.
- **Защита от «будильников»** — если экран включила программа, а не вы, Noctis снова его выключает.
- **Пауза на 1 час** — значок в трее становится бледным.
- **Автозапуск с Windows** — тихо, сразу в трей.
- **Дизайн Windows 11** — панель Acrylic над треем, как «Быстрые настройки», переключатели как в «Параметрах», живой обратный отсчёт с индикатором. Следует светлой/тёмной теме и акцентному цвету. Монохромный значок в трее подстраивается под панель задач.
- **Экономная** — около 2–3 МБ памяти. Буфер окна освобождается, пока панель закрыта.

## Установка

Запустите `Noctis_Setup_1.2.1.exe` из [Releases](../../../releases). Права администратора не нужны. Программа ставится в `%LOCALAPPDATA%\Programs\Noctis`. Удаление — «Параметры → Приложения».

Тихая установка:

```
Noctis_Setup_1.2.1.exe /S [/D=<папка>] [/nodesktop] [/noautostart] [/nolaunch]
uninstall.exe /uninstall /S
```

## Использование

Значок луны в трее: **левый клик** открывает настройки, **правый** — меню.

Командная строка: `Noctis.exe /off` — выключить экран сейчас; `Noctis.exe /autostart` — тихий запуск.

## Сборка

Visual Studio 2022 (компонент C++) → запустите `build.bat`. Получите `bin\Noctis.exe` и `dist\Noctis_Setup_1.2.1.exe`.

Настройки хранятся в `HKCU\Software\Sallador\Noctis`.
