/* Teste de ponta a ponta do aviso de ISO diferente (run_room_test.py).
 *
 * Um "RetroArch" falso: carrega o kailleraclient.dll da pasta dele, entrega
 * ao DLL um id de ISO fixo (kailleraSetContentIdCallback, como o
 * kailleraContentIdForGame do fork) e opera as janelas do DLL sozinho: conecta
 * no primeiro servidor da lista (o EmuLinker local do n02.ini), cria a sala
 * (host) ou entra na primeira sala da lista (join), aperta Start quando a sala
 * tem FAKE_PLAYERS jogadores e responde as caixas. Tudo vai para fake.log.
 *
 *   FAKE_ROLE      host | join
 *   FAKE_ISO       "CRC:TAMANHO" | none (sem o callback: outro emulador) |
 *                  unreadable (o callback responde -1)
 *   FAKE_DELAY_MS  quanto o "hash" demora (o callback responde 0 ate la)
 *   FAKE_PLAYERS   host: quantos jogadores esperar antes do Start
 *   FAKE_SETTLE_MS host: espera depois disso (os ids chegarem)
 *   FAKE_ANSWERS   host: respostas a pergunta "iniciar mesmo assim?" em
 *                  ordem (nao,sim); depois de um "nao" aperta Start de novo
 *   FAKE_RUN_MS    tempo total; no fim grava o chat da sala no log e sai
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_CONNECT 1036
#define LV_ULIST    1029
#define IDC_CREATE  1006
#define LV_GLIST    1046
#define LV_GULIST   1054
#define RE_GCHAT    1052
#define BTN_START   1055

typedef struct {
	char *appName;
	char *gameList;
	int (__stdcall *gameCallback)(char *game, int player, int numplayers);
	void (__stdcall *chatReceivedCallback)(char *nick, char *text);
	void (__stdcall *clientDroppedCallback)(char *nick, int playernb);
	void (__stdcall *moreInfosCallback)(char *gamename);
	int (__stdcall *findOrBrowseGameCallback)(char *wantedGame);
} kailleraInfos;

static char g_log[MAX_PATH];
static CRITICAL_SECTION g_lock;
static HMODULE g_dll;
static char g_iso[64];
static DWORD g_delay, g_t0;
static volatile LONG g_game_started = 0;
static char g_answers[128];
static int g_answer_pos = 0;

static void LogLine(const char* fmt, ...) {
	char line[8192];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(line, sizeof(line) - 2, fmt, ap);
	va_end(ap);
	line[sizeof(line) - 2] = 0;
	EnterCriticalSection(&g_lock);
	FILE* f = fopen(g_log, "ab");
	if (f) {
		fprintf(f, "%6lu %s\n", GetTickCount() - g_t0, line);
		fclose(f);
	}
	LeaveCriticalSection(&g_lock);
}

static const char* Env(const char* name, const char* def) {
	static char buf[8][256];
	static int n = 0;
	char* b = buf[n++ % 8];
	DWORD r = GetEnvironmentVariableA(name, b, 256);
	return r > 0 && r < 256 ? b : def;
}

static int __stdcall ContentId(const char* game, int start, char* out, int cap) {
	static DWORD since = 0;
	if (start)
		since = GetTickCount();
	if (strcmp(g_iso, "unreadable") == 0) {
		LogLine("contentid %s start=%d -> -1", game, start);
		return -1;
	}
	if (GetTickCount() - since < g_delay) {
		if (start)
			LogLine("contentid %s start=1 -> 0 (hashing)", game);
		return 0;
	}
	strncpy(out, g_iso, cap - 1);
	out[cap - 1] = 0;
	LogLine("contentid %s start=%d -> 1 %s", game, start, out);
	return 1;
}

static int __stdcall GameCallback(char* game, int player, int numplayers) {
	LogLine("game start %s player %d/%d", game, player, numplayers);
	InterlockedExchange(&g_game_started, 1);
	return 0;
}
static void __stdcall ChatCallback(char* nick, char* text) {
	LogLine("game chat <%s> %s", nick, text);
}
static void __stdcall DropCallback(char* nick, int p) {
	LogLine("dropped %s %d", nick, p);
}

typedef struct { int ctrl; HWND found; } FindCtx;
static BOOL CALLBACK FindTop(HWND w, LPARAM lp) {
	FindCtx* c = (FindCtx*)lp;
	DWORD pid;
	GetWindowThreadProcessId(w, &pid);
	/* Control ids repeat across the DLL's dialogs: the server window is the
	   one with the room's chat too, the server list the one with its list. */
	int other = c->ctrl == BTN_CONNECT ? LV_ULIST : RE_GCHAT;
	if (pid == GetCurrentProcessId() && IsWindowVisible(w) && GetDlgItem(w, c->ctrl) != NULL
		&& GetDlgItem(w, other) != NULL && (c->ctrl == BTN_CONNECT || GetDlgItem(w, LV_GLIST) != NULL)) {
		c->found = w;
		return FALSE;
	}
	return TRUE;
}
static HWND FindDialogWith(int ctrl) {
	FindCtx c = { ctrl, NULL };
	EnumWindows(FindTop, (LPARAM)&c);
	return c.found;
}

static HWND WaitDialogWith(int ctrl, DWORD ms) {
	DWORD t = GetTickCount();
	while (GetTickCount() - t < ms) {
		HWND w = FindDialogWith(ctrl);
		if (w)
			return w;
		Sleep(100);
	}
	return NULL;
}

typedef struct { const char* cls; HWND found; char list[2048]; } ClsCtx;
static BOOL CALLBACK FindCls(HWND w, LPARAM lp) {
	ClsCtx* c = (ClsCtx*)lp;
	DWORD pid;
	char cls[64], title[128], item[256];
	GetWindowThreadProcessId(w, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(w))
		return TRUE;
	GetClassNameA(w, cls, sizeof(cls));
	GetWindowTextA(w, title, sizeof(title));
	_snprintf(item, sizeof(item) - 1, "[%s|%s] ", cls, title);
	item[sizeof(item) - 1] = 0;
	strncat(c->list, item, sizeof(c->list) - strlen(c->list) - 1);
	if (c->cls && strcmp(cls, c->cls) == 0) {
		c->found = w;
		return FALSE;
	}
	return TRUE;
}
static HWND FindProcessWindow(const char* cls) {
	ClsCtx c;
	memset(&c, 0, sizeof(c));
	c.cls = cls;
	EnumWindows(FindCls, (LPARAM)&c);
	return c.found;
}
static const char* ListWindows(void) {
	static ClsCtx c;
	memset(&c, 0, sizeof(c));
	EnumWindows(FindCls, (LPARAM)&c);
	return c.list;
}

static void SelectRow(HWND lv, int row) {
	LVITEMA it;
	memset(&it, 0, sizeof(it));
	it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
	it.state = LVIS_SELECTED | LVIS_FOCUSED;
	SendMessageA(lv, LVM_SETITEMSTATE, row, (LPARAM)&it);
}

static void DumpChat(const char* when) {
	HWND sdlg = FindDialogWith(IDC_CREATE);
	if (!sdlg)
		return;
	char text[16384];
	text[0] = 0;
	GetWindowTextA(GetDlgItem(sdlg, RE_GCHAT), text, sizeof(text));
	LogLine("chat da sala (%s):\n%s\n-- fim do chat", when, text);
}

/* The DLL's boxes: ours (the ISO popup) and any other - logged and answered. */
typedef struct { HWND found; } BoxCtx;
static BOOL CALLBACK FindBox(HWND w, LPARAM lp) {
	BoxCtx* c = (BoxCtx*)lp;
	DWORD pid;
	char cls[64];
	GetWindowThreadProcessId(w, &pid);
	if (pid != GetCurrentProcessId() || !IsWindowVisible(w))
		return TRUE;
	GetClassNameA(w, cls, sizeof(cls));
	if (strcmp(cls, "#32770") == 0 && GetDlgItem(w, 0xFFFF) != NULL && GetDlgItem(w, IDC_CREATE) == NULL
		&& GetDlgItem(w, BTN_CONNECT) == NULL) {
		c->found = w;
		return FALSE;
	}
	return TRUE;
}

static volatile LONG g_press_start_again = 0;

static DWORD WINAPI BoxThread(LPVOID unused) {
	for (;;) {
		BoxCtx c = { NULL };
		EnumWindows(FindBox, (LPARAM)&c);
		if (c.found) {
			wchar_t wtitle[256], wtext[4096];
			char title[512], text[8192];
			Sleep(300); // drawn
			GetWindowTextW(c.found, wtitle, 256);
			GetDlgItemTextW(c.found, 0xFFFF, wtext, 4096);
			WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, title, sizeof(title), NULL, NULL);
			WideCharToMultiByte(CP_UTF8, 0, wtext, -1, text, sizeof(text), NULL, NULL);
			bool yesno = GetDlgItem(c.found, IDYES) != NULL;
			LONG ex = GetWindowLongA(c.found, GWL_EXSTYLE);
			LogLine("popup [%s]%s%s\n%s\n-- fim da popup", title, yesno ? " (Sim/Nao)" : " (OK)",
				(ex & WS_EX_TOPMOST) ? " topmost" : "", text);
			int id = IDOK;
			if (yesno) {
				char answer[16] = "nao";
				const char* p = g_answers + g_answer_pos;
				int n = 0;
				while (*p && *p != ',' && n < 15)
					answer[n++] = *p++;
				if (n)
					answer[n] = 0;
				g_answer_pos = (int)(p - g_answers) + (*p == ',' ? 1 : 0);
				id = strcmp(answer, "sim") == 0 ? IDYES : IDNO;
				LogLine("resposta: %s", id == IDYES ? "Sim" : "Nao");
				if (id == IDNO && g_answers[g_answer_pos])
					InterlockedExchange(&g_press_start_again, 1);
			}
			Sleep(700);
			{
				LONG e1 = GetWindowLongW(c.found, GWL_EXSTYLE);
				BOOL r = SetWindowPos(c.found, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
				DWORD err = GetLastError();
				LONG e2 = GetWindowLongW(c.found, GWL_EXSTYLE);
				DWORD fgpid = 0;
				GetWindowThreadProcessId(GetForegroundWindow(), &fgpid);
				LogLine("popup topmost depois de 1 s: %s (ex=%08lX; SetWindowPos=%d err=%lu -> %s; foreground %s)",
					(e1 & WS_EX_TOPMOST) ? "sim" : "nao", (unsigned long)e1, r, err, (e2 & WS_EX_TOPMOST) ? "sim" : "nao",
					fgpid == GetCurrentProcessId() ? "este processo" : "outro processo");
			}
			HWND button = GetDlgItem(c.found, id);
			if (button == NULL && id == IDOK) { // a MB_OK box's button is IDCANCEL
				id = IDCANCEL;
				button = GetDlgItem(c.found, id);
			}
			PostMessageA(c.found, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)button);
			Sleep(500);
		}
		Sleep(150);
	}
	return 0;
}

static void PressStart(HWND sdlg) {
	LogLine("Start");
	PostMessageA(sdlg, WM_COMMAND, MAKEWPARAM(BTN_START, BN_CLICKED), (LPARAM)GetDlgItem(sdlg, BTN_START));
}

static DWORD WINAPI DriveThread(LPVOID unused) {
	bool host = strcmp(Env("FAKE_ROLE", "join"), "host") == 0;
	int players = atoi(Env("FAKE_PLAYERS", "2"));
	DWORD settle = (DWORD)atoi(Env("FAKE_SETTLE_MS", "3000"));

	HWND ss = WaitDialogWith(BTN_CONNECT, 20000);
	if (!ss) {
		LogLine("ERRO: sem a janela de servidores");
		return 0;
	}
	SelectRow(GetDlgItem(ss, LV_ULIST), 0);
	Sleep(300);
	LogLine("Connect");
	PostMessageA(ss, WM_COMMAND, MAKEWPARAM(BTN_CONNECT, BN_CLICKED), (LPARAM)GetDlgItem(ss, BTN_CONNECT));

	HWND sdlg = WaitDialogWith(IDC_CREATE, 20000);
	if (!sdlg) {
		LogLine("ERRO: sem a janela do servidor");
		return 0;
	}
	for (int i = 0; i < 100; i++) {
		char t[256];
		GetWindowTextA(sdlg, t, sizeof(t));
		if (strncmp(t, "Connected to", 12) == 0)
			break;
		Sleep(100);
	}
	Sleep(1500);
	if (host) {
		LogLine("Create");
		PostMessageA(sdlg, WM_COMMAND, MAKEWPARAM(IDC_CREATE, BN_CLICKED), (LPARAM)GetDlgItem(sdlg, IDC_CREATE));
		HWND menu = NULL;
		for (int i = 0; i < 50 && !menu; i++) {
			Sleep(100);
			menu = FindProcessWindow("#32768");
		}
		if (!menu) {
			LogLine("ERRO: o menu de jogos nao abriu - janelas: %s", ListWindows());
			return 0;
		}
		LogLine("menu aberto");
		/* The menu's modal loop reads the dialog thread's queue. */
		PostMessageA(menu, WM_KEYDOWN, VK_DOWN, 0);
		Sleep(300);
		PostMessageA(menu, WM_KEYDOWN, VK_RETURN, 0);
	} else {
		HWND glist = GetDlgItem(sdlg, LV_GLIST);
		int rows = 0;
		for (int i = 0; i < 200 && rows == 0; i++) {
			Sleep(100);
			rows = (int)SendMessageA(glist, LVM_GETITEMCOUNT, 0, 0);
		}
		if (rows == 0) {
			LogLine("ERRO: nenhuma sala para entrar");
			return 0;
		}
		Sleep(500);
		SelectRow(glist, 0);
		NMHDR nm;
		nm.hwndFrom = glist;
		nm.idFrom = LV_GLIST;
		nm.code = NM_DBLCLK;
		LogLine("Join");
		SendMessageA(sdlg, WM_NOTIFY, LV_GLIST, (LPARAM)&nm);
	}

	if (!host)
		return 0;
	HWND gulist = GetDlgItem(sdlg, LV_GULIST);
	for (int i = 0; i < 600; i++) {
		if ((int)SendMessageA(gulist, LVM_GETITEMCOUNT, 0, 0) >= players)
			break;
		Sleep(100);
	}
	LogLine("%d jogador(es) na sala", (int)SendMessageA(gulist, LVM_GETITEMCOUNT, 0, 0));
	Sleep(settle);
	PressStart(sdlg);
	for (;;) {
		Sleep(200);
		if (InterlockedExchange(&g_press_start_again, 0)) {
			Sleep(3000);
			PressStart(sdlg);
		}
	}
	return 0;
}

static DWORD WINAPI EndThread(LPVOID unused) {
	DWORD run = (DWORD)atoi(Env("FAKE_RUN_MS", "40000"));
	typedef int (__stdcall *endgame_t)(void);
	endgame_t endGame = (endgame_t)GetProcAddress(g_dll, "kailleraEndGame");
	DWORD t = GetTickCount();
	bool ended = false;
	while (GetTickCount() - t < run) {
		Sleep(200);
		if (g_game_started && !ended) {
			Sleep(3000);
			LogLine("kailleraEndGame");
			if (endGame)
				endGame();
			ended = true;
		}
	}
	DumpChat("fim");
	/* Leave the server like a player closing the window, so the next test
	   doesn't find this nick and its room still there. */
	HWND sdlg = FindDialogWith(IDC_CREATE);
	if (sdlg) {
		PostMessageA(sdlg, WM_CLOSE, 0, 0);
		Sleep(1500);
	}
	LogLine("exit");
	ExitProcess(0);
	return 0;
}

int main(void) {
	char dir[MAX_PATH], dllPath[MAX_PATH];
	InitializeCriticalSection(&g_lock);
	g_t0 = GetTickCount();
	GetModuleFileNameA(NULL, dir, MAX_PATH);
	*strrchr(dir, '\\') = 0;
	_snprintf(g_log, MAX_PATH, "%s\\fake.log", dir);
	_snprintf(dllPath, MAX_PATH, "%s\\kailleraclient.dll", dir);
	strncpy(g_iso, Env("FAKE_ISO", "none"), sizeof(g_iso) - 1);
	strncpy(g_answers, Env("FAKE_ANSWERS", "nao"), sizeof(g_answers) - 1);
	g_delay = (DWORD)atoi(Env("FAKE_DELAY_MS", "1500"));
	DeleteFileA(g_log);
	LogLine("start iso=%s role=%s", g_iso, Env("FAKE_ROLE", "join"));

	g_dll = LoadLibraryA(dllPath);
	if (!g_dll) {
		LogLine("ERRO: LoadLibrary %lu", GetLastError());
		return 1;
	}
	typedef int (__stdcall *init_t)(void);
	typedef int (__stdcall *setinfos_t)(kailleraInfos*);
	typedef int (__stdcall *dialog_t)(HWND);
	typedef void (__stdcall *setcb_t)(int (__stdcall*)(const char*, int, char*, int));
	init_t init = (init_t)GetProcAddress(g_dll, "kailleraInit");
	setinfos_t setInfos = (setinfos_t)GetProcAddress(g_dll, "kailleraSetInfos");
	dialog_t dialog = (dialog_t)GetProcAddress(g_dll, "kailleraSelectServerDialog");
	setcb_t setCb = (setcb_t)GetProcAddress(g_dll, "kailleraSetContentIdCallback");

	static char games[] = "PCSX ReARMed: Euro vs Copa America 2004.bin\0\0";
	static kailleraInfos infos;
	infos.appName = (char*)"RetroArch 1.16.0.K.FFW.TIERES.teste";
	infos.gameList = games;
	infos.gameCallback = GameCallback;
	infos.chatReceivedCallback = ChatCallback;
	infos.clientDroppedCallback = DropCallback;
	infos.moreInfosCallback = NULL;
	infos.findOrBrowseGameCallback = NULL;
	init();
	setInfos(&infos);
	if (strcmp(g_iso, "none") != 0) {
		if (setCb)
			setCb(ContentId);
		LogLine("kailleraSetContentIdCallback %s", setCb ? "registrado" : "AUSENTE no DLL");
	}

	CreateThread(NULL, 0, BoxThread, NULL, 0, NULL);
	CreateThread(NULL, 0, DriveThread, NULL, 0, NULL);
	CreateThread(NULL, 0, EndThread, NULL, 0, NULL);
	dialog(NULL);
	LogLine("dialog returned");
	Sleep(INFINITE);
	return 0;
}
