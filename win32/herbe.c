/* herbe for Windows (Win32 + GDI, MSVC).
 *
 * Port of dudik/herbe. Same contract as upstream:
 *   herbe "line" "line" ...      shows a notification, blocks until it closes
 *   exit 0 = action (ACTION_BUTTON / `herbe --action`)
 *   exit 1 = failure
 *   exit 2 = dismissed (DISMISS_BUTTON / timeout / `herbe --dismiss` / WM_CLOSE / Ctrl+C)
 *
 * POSIX -> Win32 mapping (see LowLevel-Katas N4-notifications docs):
 *   XOpenDisplay/XCreateWindow(override_redirect) -> CreateWindowExW(WS_POPUP, TOPMOST|TOOLWINDOW|NOACTIVATE)
 *   Xft font + XftTextExtentsUtf8                 -> CreateFontW + GetTextExtentPoint32W (UTF-16)
 *   sem_open("/herbe") + sem_wait                 -> named mutex "Local\herbe" (WAIT_ABANDONED = crash-safe)
 *   SIGUSR1 / SIGUSR2 (pkill)                     -> named auto-reset events "Local\herbe-dismiss|action"
 *   SIGTERM / SIGINT                              -> WM_CLOSE (taskkill) / SetConsoleCtrlHandler
 *   alarm(duration) + SIGALRM                     -> SetTimer + WM_TIMER
 *   XNextEvent loop                               -> MsgWaitForMultipleObjects + PeekMessage pump
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

#define HERBE_BTN_LEFT 1
#define HERBE_BTN_MIDDLE 2
#define HERBE_BTN_RIGHT 3

#include "config.h"

#define EXIT_ACTION 0
#define EXIT_FAIL 1
#define EXIT_DISMISS 2

#define WM_HERBE_EXPIRE (WM_APP + 1) /* wParam = button, like expire(sig) */

static const wchar_t *MUTEX_NAME = L"Local\\herbe";
static const wchar_t *EV_DISMISS_NAME = L"Local\\herbe-dismiss";
static const wchar_t *EV_ACTION_NAME = L"Local\\herbe-action";

static HWND window;
static volatile LONG pending_ctrl; /* Ctrl+C arrived before the window existed */
static int exit_code = EXIT_DISMISS;
static int done;

static HFONT font;
static int text_height;
static wchar_t **lines;
static int num_of_lines;
static COLORREF col_bg, col_border, col_font;
static int s_padding, s_line_spacing, s_border;

static void die(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(EXIT_FAIL);
}

static COLORREF parse_color(const char *hex)
{
	unsigned r = 0, g = 0, b = 0;
	if (!hex || hex[0] != '#' || sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b) != 3)
		die("invalid color '%s' (expected #rrggbb)", hex ? hex : "(null)");
	return RGB(r, g, b);
}

static int text_width(HDC dc, const wchar_t *s, int n)
{
	SIZE sz = {0};
	if (n > 0)
		GetTextExtentPoint32W(dc, s, n, &sz);
	return sz.cx;
}

/* Port of upstream get_max_len(): how many UTF-16 units of `string` fit on
 * one line. Consumes an embedded '\n' (turned into a space, as upstream). */
static int get_max_len(HDC dc, wchar_t *string, int max_text_width, int max_advance)
{
	int len = (int)wcslen(string);
	int eol = len;
	int w = text_width(dc, string, eol);

	if (w > max_text_width) {
		/* largest prefix whose width is <= max. Upstream stops at >= and
		 * then steps back, which drops one char when a line fills the box
		 * exactly (very common with monospace fonts). */
		eol = max_text_width / (max_advance ? max_advance : 1);
		if (eol > len)
			eol = len;
		while (eol > 0 && text_width(dc, string, eol) > max_text_width)
			eol--;
		while (eol < len && text_width(dc, string, eol + 1) <= max_text_width)
			eol++;
		/* never split a surrogate pair (UTF-8 upstream could split a code point) */
		if (eol > 0 && eol < len && IS_HIGH_SURROGATE(string[eol - 1]))
			eol--;
		if (eol <= 0)
			eol = (len > 1 && IS_HIGH_SURROGATE(string[0])) ? 2 : (len ? 1 : 0);
	}

	for (int i = 0; i < eol; i++)
		if (string[i] == L'\n') {
			string[i] = L' ';
			return ++i;
		}

	if (eol == len)
		return eol; /* the rest fits */

	int temp = eol;
	while (string[eol] != L' ' && eol)
		--eol;

	return eol == 0 ? temp : ++eol;
}

static void expire(int button)
{
	if (button == DISMISS_BUTTON) {
		exit_code = EXIT_DISMISS;
		done = 1;
	} else if (button == ACTION_BUTTON) {
		exit_code = EXIT_ACTION;
		done = 1;
	}
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
	(void)type; /* CTRL_C / BREAK / CLOSE / LOGOFF / SHUTDOWN: SIGINT/SIGTERM -> dismiss */
	if (window)
		PostMessageW(window, WM_HERBE_EXPIRE, DISMISS_BUTTON, 0);
	else
		InterlockedExchange(&pending_ctrl, 1);
	return TRUE;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT rc;
		GetClientRect(hwnd, &rc);
		HBRUSH border = CreateSolidBrush(col_border);
		HBRUSH bg = CreateSolidBrush(col_bg);
		FillRect(dc, &rc, border);
		InflateRect(&rc, -s_border, -s_border);
		FillRect(dc, &rc, bg);
		DeleteObject(border);
		DeleteObject(bg);

		HGDIOBJ old = SelectObject(dc, font);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, col_font);
		SetTextAlign(dc, TA_TOP | TA_LEFT);
		for (int i = 0; i < num_of_lines; i++)
			TextOutW(dc, s_border + s_padding,
				 s_border + s_padding + i * (text_height + s_line_spacing),
				 lines[i], (int)wcslen(lines[i]));
		SelectObject(dc, old);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE; /* override_redirect: never steal focus */
	case WM_LBUTTONDOWN:
		expire(HERBE_BTN_LEFT);
		return 0;
	case WM_MBUTTONDOWN:
		expire(HERBE_BTN_MIDDLE);
		return 0;
	case WM_RBUTTONDOWN:
		expire(HERBE_BTN_RIGHT);
		return 0;
	case WM_TIMER: /* SIGALRM */
	case WM_CLOSE: /* taskkill without /F == SIGTERM */
		expire(DISMISS_BUTTON);
		return 0;
	case WM_HERBE_EXPIRE:
		expire((int)wp);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Pump the queue while waiting for kernel objects. Returns index of the
 * signalled object, or -1 when `done` was set by a window message. */
static int wait_pumping(DWORD n, const HANDLE *objs)
{
	for (;;) {
		MSG m;
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
		if (done)
			return -1;
		DWORD r = MsgWaitForMultipleObjects(n, objs, FALSE, INFINITE, QS_ALLINPUT);
		if (r < WAIT_OBJECT_0 + n)
			return (int)(r - WAIT_OBJECT_0);
		if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + n)
			return (int)(r - WAIT_ABANDONED_0); /* previous herbe crashed: we own it now */
		if (r == WAIT_FAILED)
			die("MsgWaitForMultipleObjects failed: %lu", GetLastError());
	}
}

static int signal_running(const wchar_t *name)
{
	HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
	if (!ev)
		return EXIT_FAIL; /* nothing on screen: like pkill matching no process */
	SetEvent(ev);
	CloseHandle(ev);
	return 0;
}

static void hide_own_console(void)
{
	/* Console subsystem keeps `herbe ... && action` semantics in cmd/PowerShell
	 * (they wait for console apps, not for GUI ones). If we were launched from
	 * Explorer/a hotkey we got a fresh console: hide it. */
	DWORD procs[2];
	if (GetConsoleProcessList(procs, 2) == 1)
		ShowWindow(GetConsoleWindow(), SW_HIDE);
}

int wmain(int argc, wchar_t *argv[])
{
	if (argc == 1)
		die("Usage: herbe body\n       herbe --dismiss | --action");

	if (argc == 2 && !wcscmp(argv[1], L"--dismiss"))
		return signal_running(EV_DISMISS_NAME);
	if (argc == 2 && !wcscmp(argv[1], L"--action"))
		return signal_running(EV_ACTION_NAME);

	hide_own_console();
	SetConsoleCtrlHandler(ctrl_handler, TRUE);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	col_bg = parse_color(background_color);
	col_border = parse_color(border_color);
	col_font = parse_color(font_color);

	HMONITOR mon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = {.cbSize = sizeof mi};
	GetMonitorInfoW(mon, &mi);
	UINT dpi_x = 96, dpi_y = 96;
	if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)))
		dpi_x = 96;
#define S(v) MulDiv((int)(v), (int)dpi_x, 96)
	int s_width = S(width);
	s_padding = S(padding);
	s_line_spacing = S(line_spacing);
	s_border = S(border_size);

	font = CreateFontW(-MulDiv(font_size, (int)dpi_x, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			   FF_DONTCARE, font_face);
	if (!font)
		die("Cannot create font");

	HDC mdc = CreateCompatibleDC(NULL);
	SelectObject(mdc, font);
	TEXTMETRICW tm;
	GetTextMetricsW(mdc, &tm);
	text_height = tm.tmHeight;

	int max_text_width = s_width - 2 * s_padding;
	int lines_size = 5;
	lines = malloc(lines_size * sizeof *lines);
	if (!lines)
		die("malloc failed");

	for (int i = 1; i < argc; i++) {
		wchar_t *p = argv[i];
		for (int eol = get_max_len(mdc, p, max_text_width, tm.tmMaxCharWidth); eol;
		     p += eol, num_of_lines++, eol = get_max_len(mdc, p, max_text_width, tm.tmMaxCharWidth)) {
			if (lines_size <= num_of_lines) {
				lines = realloc(lines, (lines_size += 5) * sizeof *lines);
				if (!lines)
					die("realloc failed");
			}
			lines[num_of_lines] = malloc((eol + 1) * sizeof(wchar_t));
			if (!lines[num_of_lines])
				die("malloc failed");
			wmemcpy(lines[num_of_lines], p, eol);
			lines[num_of_lines][eol] = 0;
		}
	}
	DeleteDC(mdc);

	/* upstream: height = (n-1)*spacing + n*text_height + 2*padding; X11 borders
	 * live outside the window, here we paint them inside the client area. */
	int height = (num_of_lines > 0 ? num_of_lines - 1 : 0) * s_line_spacing +
		     num_of_lines * text_height + 2 * s_padding;
	int win_w = s_width + 2 * s_border, win_h = height + 2 * s_border;
	RECT wa = mi.rcWork; /* work area: do not draw over the taskbar */
	int x = wa.left + S(pos_x), y = wa.top + S(pos_y);
	if (corner == TOP_RIGHT || corner == BOTTOM_RIGHT)
		x = wa.right - win_w - S(pos_x);
	if (corner == BOTTOM_LEFT || corner == BOTTOM_RIGHT)
		y = wa.bottom - win_h - S(pos_y);

	HINSTANCE inst = GetModuleHandleW(NULL);
	WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = wndproc, .hInstance = inst,
			  .hCursor = LoadCursorW(NULL, IDC_ARROW), .lpszClassName = L"herbe"};
	RegisterClassExW(&wc);
	window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"herbe", L"herbe",
				 WS_POPUP, x, y, win_w, win_h, NULL, NULL, inst, NULL);
	if (!window)
		die("Cannot create window: %lu", GetLastError());
	ShowWindow(window, SW_SHOWNOACTIVATE);
	if (InterlockedExchange(&pending_ctrl, 0))
		PostMessageW(window, WM_HERBE_EXPIRE, DISMISS_BUTTON, 0);

	/* Queue: one notification counts down at a time (upstream sem_wait). */
	HANDLE mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
	if (!mutex)
		die("CreateMutex failed: %lu", GetLastError());
	int owned = wait_pumping(1, &mutex) == 0;

	if (owned && !done) {
		HANDLE ev[2] = {CreateEventW(NULL, FALSE, FALSE, EV_DISMISS_NAME),
				CreateEventW(NULL, FALSE, FALSE, EV_ACTION_NAME)};
		/* a --dismiss sent while nobody was showing must not kill us */
		ResetEvent(ev[0]);
		ResetEvent(ev[1]);
		if (duration != 0)
			SetTimer(window, 1, duration * 1000, NULL);
		int r = wait_pumping(2, ev);
		if (r == 0)
			expire(DISMISS_BUTTON);
		else if (r == 1)
			expire(ACTION_BUTTON);
		CloseHandle(ev[0]);
		CloseHandle(ev[1]);
	}
	if (owned)
		ReleaseMutex(mutex);
	CloseHandle(mutex);

	for (int i = 0; i < num_of_lines; i++)
		free(lines[i]);
	free(lines);
	DestroyWindow(window);
	DeleteObject(font);

	return exit_code;
}
