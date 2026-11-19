# herbe for Windows (Win32 + GDI, MSVC)

Native port of herbe. No X server, no MSYS/Cygwin runtime: a single `herbe.exe`
linked against `user32`, `gdi32` and `shcore`.

## Build

```bat
win32\build.bat            :: MSVC, produces win32\herbe.exe
```
or `cmake -S win32 -B win32/build && cmake --build win32/build --config Release`.
`config.def.h` is copied to `config.h` on first build, edit that.

## Usage (same contract as upstream)

```powershell
herbe "Notification body" "second line"; if ($LASTEXITCODE -eq 0) { "action!" }
herbe --dismiss    # == pkill -SIGUSR1 herbe   (dismiss the notification on screen)
herbe --action     # == pkill -SIGUSR2 herbe   (accept it)
taskkill /IM herbe.exe   # == SIGTERM to every herbe: all dismiss (exit 2)
```

Exit codes: `0` action, `1` failure, `2` dismissed.

## What changed and why

| Upstream (POSIX/X11) | Windows | Note |
|---|---|---|
| `override_redirect` window | `WS_POPUP` + `WS_EX_TOPMOST/TOOLWINDOW/NOACTIVATE`, `MA_NOACTIVATE` | never steals focus, not in taskbar/Alt-Tab |
| Xft font pattern | `font_face` + `font_size` (pt), DPI aware (per-monitor v2) | sizes in config are 96-DPI pixels |
| screen size | primary monitor **work area** | does not cover the taskbar |
| `sem_open("/herbe")` | named mutex `Local\herbe` | `WAIT_ABANDONED` makes a crashed herbe release the queue; the `sem_unlink` hack in upstream's usage path is no longer needed |
| `SIGUSR1`/`SIGUSR2` | named auto-reset events, sent with `--dismiss`/`--action` | events are reset when a notification takes the queue, so a stale signal can't kill the next one |
| `SIGALRM` | `SetTimer` | timer starts when the notification reaches the head of the queue, like upstream |
| `SIGINT`/`SIGTERM` | `SetConsoleCtrlHandler` / `WM_CLOSE` | both dismiss |
| UTF-8 `argv` | `wmain` UTF-16 `argv` | wrap never splits a surrogate pair |

Behavioural fixes vs upstream: a line that fills the width exactly is no longer
cut one character early; clicks on a queued (not yet active) notification are
handled immediately instead of when it reaches the head of the queue.

It is a **console** subsystem program on purpose: `cmd` and PowerShell only wait
for console programs, and `herbe ... && action` depends on waiting. When started
without a parent console (Explorer, a hotkey) the fresh console is hidden.
