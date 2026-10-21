/* "retroarch.exe" falso para o teste do atualizador do pacote (run_test.py).
 *
 * Faz o que o RetroArch TIERES faz com o DLL ao abrir o Kaillera: tem uma
 * janela de classe "RetroArch", carrega o kailleraclient.dll da pasta dele,
 * chama kailleraInit() e, se FAKE_DIALOG=1, kailleraSelectServerDialog() numa
 * thread (como o kailleraThread do fork), que descarrega o DLL quando o
 * dialogo volta (CloseKaillera). O item "Kaillera Netplay" do menu
 * (WM_COMMAND 40028) carrega o DLL de novo, como o LoadKaillera() do fork -
 * menos com FAKE_NO_MENU=1 (a barra de menus desligada). Um WM_CLOSE fecha o
 * programa, como no RetroArch; sem ele, o programa sai sozinho depois de
 * FAKE_TIMER_MS (padrao 5000).
 *
 * Uma thread responde sozinha as caixas de mensagem do DLL (Sim/OK, ou Nao com
 * FAKE_ANSWER=nao) e, com FAKE_CANCEL=1, clica em Cancelar na janela de
 * progresso. Fecha tambem a caixa "Atualizacao" de outro processo (o rundll32
 * do DLL). Tudo vai para teste.log, ao lado do exe:
 *
 *     start <versao> pid=<pid> args=<argumentos>
 *     msgbox <titulo> | <texto>
 *     foreign msgbox <titulo> | <texto>   (de outro processo)
 *     progress <texto>          (janela de progresso vista)
 *     dialog returned / kaillera unloaded
 *     menu kaillera / menu kaillera ignored (...)
 *     wm_close / exit
 *
 * Compilado com /DFAKE_VERSION="v1" (ou v2...) e /DFAKE_TIERES para ter a
 * marca do RetroArch TIERES ("FFW.TIERES") no exe.
 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef FAKE_VERSION
#define FAKE_VERSION "v?"
#endif

#ifdef FAKE_TIERES
/* the same marker the real exe carries in its version string */
volatile const char fake_marker[] = "RetroArch 1.16.0.K.FFW.TIERES.9";
#endif

static wchar_t g_log[MAX_PATH];
/* Like the fork's win32_check_window: a close is dropped while Kaillera is
 * loaded ("Close Kaillera window first."), i.e. from kailleraInit() until the
 * Kaillera thread has unloaded the DLL after the dialog returned. */
static volatile LONG g_kaillera_open = 0;
static CRITICAL_SECTION g_log_lock;
static volatile LONG g_stop_clicker = 0;

static void LogLine(const wchar_t* fmt, ...) {
	wchar_t line[4096];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf(line, 4095, fmt, ap);
	va_end(ap);
	line[4095] = 0;
	char utf8[8192];
	int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8) - 2, NULL, NULL);
	if (n <= 0)
		return;
	utf8[n - 1] = '\n';
	EnterCriticalSection(&g_log_lock);
	HANDLE f = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		DWORD w;
		WriteFile(f, utf8, n, &w, NULL);
		CloseHandle(f);
	}
	LeaveCriticalSection(&g_log_lock);
}

static void Flatten(wchar_t* s) {
	for (; *s; s++)
		if (*s == L'\r' || *s == L'\n')
			*s = L' ';
}

/* FAKE_SHOT_DIR: saves <name>.bmp of the window, to look at
 * the layout of the DLL's boxes without a person in front of the screen. */
static void Screenshot(HWND hwnd, const wchar_t* name) {
	char dirA[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("FAKE_SHOT_DIR", dirA, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return;
	RECT rc;
	GetWindowRect(hwnd, &rc);
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	HDC screen = GetDC(NULL);
	HDC mem = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
	HGDIOBJ old = SelectObject(mem, bmp);
	/* what is on the screen (the DLL's windows are topmost) - PrintWindow
	   catches half-repainted controls */
	BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
	SelectObject(mem, old);

	BITMAPINFOHEADER bi;
	memset(&bi, 0, sizeof(bi));
	bi.biSize = sizeof(bi);
	bi.biWidth = w;
	bi.biHeight = h;
	bi.biPlanes = 1;
	bi.biBitCount = 24;
	bi.biCompression = BI_RGB;
	int stride = ((w * 3 + 3) & ~3);
	bi.biSizeImage = stride * h;
	char* pixels = (char*)malloc(bi.biSizeImage);
	GetDIBits(screen, bmp, 0, h, pixels, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

	wchar_t path[MAX_PATH];
	_snwprintf(path, MAX_PATH, L"%S\\%s.bmp", dirA, name);
	path[MAX_PATH - 1] = 0;
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		BITMAPFILEHEADER bf;
		memset(&bf, 0, sizeof(bf));
		bf.bfType = 0x4D42;
		bf.bfOffBits = sizeof(bf) + sizeof(bi);
		bf.bfSize = bf.bfOffBits + bi.biSizeImage;
		DWORD wr;
		WriteFile(f, &bf, sizeof(bf), &wr, NULL);
		WriteFile(f, &bi, sizeof(bi), &wr, NULL);
		WriteFile(f, pixels, bi.biSizeImage, &wr, NULL);
		CloseHandle(f);
	}
	free(pixels);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(NULL, screen);
}

static BOOL CALLBACK ClickerEnum(HWND hwnd, LPARAM lp) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (!IsWindowVisible(hwnd))
		return TRUE;
	wchar_t cls[64], title[256], text[2048];
	GetClassNameW(hwnd, cls, 64);
	GetWindowTextW(hwnd, title, 256);
	if (pid != GetCurrentProcessId()) {
		/* the DLL's reopener (rundll32) telling the player to open Kaillera */
		if (wcscmp(cls, L"#32770") == 0 && wcsncmp(title, L"Atualiza", 8) == 0) {
			text[0] = 0;
			GetDlgItemTextW(hwnd, 0xFFFF, text, 2048);
			Flatten(text);
			LogLine(L"foreign msgbox %s | %s", title, text);
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
			Sleep(300);
		}
		return TRUE;
	}
	if (wcscmp(cls, L"#32770") == 0) {
		text[0] = 0;
		GetDlgItemTextW(hwnd, 0xFFFF, text, 2048);
		Flatten(text);
		LogLine(L"msgbox %s | %s", title, text);
		{
			static int shots = 0;
			wchar_t name[32];
			_snwprintf(name, 32, L"msgbox_%lu_%d", GetCurrentProcessId(), ++shots);
			Sleep(150); /* drawn */
			Screenshot(hwnd, name);
		}
		const char* answer = getenv("FAKE_ANSWER");
		if (GetDlgItem(hwnd, IDYES) != NULL) {
			int id = (answer && strcmp(answer, "nao") == 0) ? IDNO : IDYES;
			PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(hwnd, id));
		} else {
			/* MB_OK: its button isn't IDOK on every Windows - closing works */
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
		}
		Sleep(300); /* let it close before the next scan */
	} else if (wcscmp(cls, L"N02AtualizacaoRetroArch") == 0) {
		static int seen = 0, cancelled = 0;
		HWND child = GetDlgItem(hwnd, 101);
		text[0] = 0;
		if (child)
			GetWindowTextW(child, text, 2048);
		Flatten(text);
		if (!seen || wcsstr(text, L"Baixando") != NULL) {
			if (seen < 400)
				LogLine(L"progress %s | %s", title, text);
			if (seen == 15)
				Screenshot(hwnd, L"progresso");
			seen++;
		}
		const char* cancel = getenv("FAKE_CANCEL");
		if (cancel && strcmp(cancel, "1") == 0 && !cancelled && wcsstr(text, L"Baixando") != NULL) {
			cancelled = 1;
			LogLine(L"clicking Cancelar");
			PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
		}
	}
	return TRUE;
}

static DWORD WINAPI ClickerThread(LPVOID p) {
	(void)p;
	while (!g_stop_clicker) {
		EnumWindows(ClickerEnum, 0);
		Sleep(100);
	}
	return 0;
}

typedef int (WINAPI* InitFn)(void);
typedef void (WINAPI* DialogFn)(void*);
static DialogFn g_dialog;
static HMODULE g_dll;

/* the "Kaillera Netplay" item of the fork's window menu (ID_KAILLERA) */
#define FAKE_MENU_KAILLERA 40028

static DWORD WINAPI DialogThread(LPVOID p) {
	(void)p;
	g_dialog(NULL);
	LogLine(L"dialog returned");
	/* CloseKaillera: kailleraShutdown, then the flag (FAKE_KAILLERA_MS: slower) */
	const char* ms = getenv("FAKE_KAILLERA_MS");
	Sleep(ms ? (DWORD)atoi(ms) : 300);
	InterlockedExchange(&g_kaillera_open, 0);
	FreeLibrary(g_dll);
	g_dll = NULL;
	LogLine(L"kaillera unloaded");
	return 0;
}

/* LoadKaillera() + InitialiseKaillera(): the DLL from disk, kailleraInit,
   and the dialog on its own thread. */
static int OpenKaillera(void) {
	g_dll = LoadLibraryW(L"kailleraclient.dll");
	if (g_dll == NULL) {
		LogLine(L"LoadLibrary failed %lu", GetLastError());
		return 0;
	}
	InitFn init = (InitFn)GetProcAddress(g_dll, "kailleraInit");
	g_dialog = (DialogFn)GetProcAddress(g_dll, "kailleraSelectServerDialog");
	InterlockedExchange(&g_kaillera_open, 1);
	init();
	LogLine(L"kailleraInit returned");

	const char* dialog = getenv("FAKE_DIALOG");
	if (dialog && strcmp(dialog, "1") == 0) {
		/* like the fork's kailleraThread: the main thread keeps pumping */
		CloseHandle(CreateThread(NULL, 0, DialogThread, NULL, 0, NULL));
	} else {
		InterlockedExchange(&g_kaillera_open, 0);
	}
	return 1;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_COMMAND:
		if (LOWORD(wp) == FAKE_MENU_KAILLERA) {
			const char* noMenu = getenv("FAKE_NO_MENU");
			if (noMenu && strcmp(noMenu, "1") == 0)
				LogLine(L"menu kaillera ignored (no menubar)");
			else if (g_kaillera_open || g_dll != NULL)
				LogLine(L"menu kaillera ignored (kaillera open)");
			else {
				LogLine(L"menu kaillera");
				OpenKaillera();
			}
			return 0;
		}
		break;
	case WM_CLOSE:
		if (g_kaillera_open) {
			LogLine(L"close refused (kaillera open)");
			return 0;
		}
		LogLine(L"wm_close");
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_TIMER:
		LogLine(L"no wm_close (timer)");
		DestroyWindow(hwnd);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int show) {
	(void)prev;
	(void)show;
	InitializeCriticalSection(&g_log_lock);
	SetProcessDPIAware(); /* like RetroArch (media/rarch.manifest, dpiAware) */
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	wcscpy(g_log, exe);
	wchar_t* slash = wcsrchr(g_log, L'\\');
	wcscpy(slash + 1, L"teste.log");

	wchar_t cwd[MAX_PATH];
	GetCurrentDirectoryW(MAX_PATH, cwd);
	LogLine(L"start %S pid=%lu args=[%s]", FAKE_VERSION, GetCurrentProcessId(), cmdLine);
	LogLine(L"cwd %s", cwd);
#ifdef FAKE_TIERES
	if (fake_marker[0] == 0)
		return 1;
#endif

	WNDCLASSW wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.lpszClassName = L"RetroArch";
	RegisterClassW(&wc);
	HWND hwnd = CreateWindowExW(0, L"RetroArch", L"RetroArch falso", WS_OVERLAPPEDWINDOW,
		0, 0, 200, 100, NULL, NULL, inst, NULL);

	HANDLE clicker = CreateThread(NULL, 0, ClickerThread, NULL, 0, NULL);

	if (!OpenKaillera())
		return 2;

	const char* timer = getenv("FAKE_TIMER_MS");
	SetTimer(hwnd, 1, timer ? (UINT)atoi(timer) : 5000, NULL);
	MSG m;
	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}
	g_stop_clicker = 1;
	WaitForSingleObject(clicker, 2000);
	LogLine(L"exit");
	return 0;
}
