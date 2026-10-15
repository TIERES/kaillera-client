#include "n02_update.h"
#include "n02_stream.h" // N02_STREAM_DEFAULT_HOST/PORT - same community server, different path
#include "n02_version.h"
#include "n02_wecamp.h" // N02_WECAMP_HOST/PORT - the site over HTTPS (package update)

#include <windows.h>
#include "k_socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN64
#include <winhttp.h>
#include <bcrypt.h>
#include <commctrl.h>
#pragma comment(lib, "bcrypt.lib")
#endif

// DLL instance handle from kailleraclient.cpp
extern HINSTANCE hx;

// A DLL always knows its own architecture at compile time - no runtime
// detection needed, and no risk of ever requesting the wrong build.
#ifdef _WIN64
#define N02_UPDATE_ARCH "x64"
#else
#define N02_UPDATE_ARCH "x86"
#endif

// A kailleraclient.dll build is a few hundred KB; refuse anything wildly
// larger than that as a sanity check against a misbehaving/compromised
// response rather than trust the server-reported size unconditionally.
#define N02_UPDATE_MAX_SIZE (16 * 1024 * 1024)

///////////////////////////////////////////////////////////////////////////////
// CRC32 (standard IEEE 802.3 / zlib polynomial - matches Python's
// zlib.crc32(), which is what app/updates.py uses server-side) - just an
// integrity check against a truncated/corrupted download before this code
// commits to swapping the running DLL out for it, not a security boundary.
///////////////////////////////////////////////////////////////////////////////

static unsigned long Crc32(const unsigned char* data, int len) {
	static unsigned long table[256];
	static bool tableInit = false;
	if (!tableInit) {
		for (unsigned long i = 0; i < 256; i++) {
			unsigned long c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		tableInit = true;
	}
	unsigned long crc = 0xFFFFFFFFUL;
	for (int n = 0; n < len; n++)
		crc = table[(crc ^ (unsigned char)data[n]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFUL;
}

///////////////////////////////////////////////////////////////////////////////
// Blocking HTTP GET helpers - raw sockets, no TLS. Style mirrors
// n02_watch.cpp's HttpGetBody(), including the same DNS-caching and
// bounded-connect() fixes (a stalled connect() here would freeze
// kailleraInit() itself, before any UI even shows).
///////////////////////////////////////////////////////////////////////////////

static char g_update_resolved_host[256] = { 0 };
static struct in_addr g_update_resolved_addr;
static bool g_update_resolved_valid = false;

static SOCKET ConnectWithTimeout(const char* host, int port, int timeoutMs) {
	if (host == NULL || host[0] == 0 || port <= 0)
		return INVALID_SOCKET;

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s == INVALID_SOCKET)
		return INVALID_SOCKET;

	sockaddr_in server;
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons((u_short)port);

	if (host[0] >= '0' && host[0] <= '9') {
		server.sin_addr.s_addr = inet_addr(host);
	} else {
		if (!g_update_resolved_valid || strcmp(g_update_resolved_host, host) != 0) {
			struct hostent* he = gethostbyname(host);
			if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
				closesocket(s);
				return INVALID_SOCKET;
			}
			g_update_resolved_addr = *(struct in_addr*)he->h_addr_list[0];
			strncpy(g_update_resolved_host, host, sizeof(g_update_resolved_host) - 1);
			g_update_resolved_host[sizeof(g_update_resolved_host) - 1] = 0;
			g_update_resolved_valid = true;
		}
		server.sin_addr = g_update_resolved_addr;
	}

	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

	u_long nonBlocking = 1;
	ioctlsocket(s, FIONBIO, &nonBlocking);

	bool connected = (connect(s, (struct sockaddr*)&server, sizeof(server)) == 0);
	if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
		fd_set wfds, efds;
		FD_ZERO(&wfds); FD_SET(s, &wfds);
		FD_ZERO(&efds); FD_SET(s, &efds);
		struct timeval tv;
		tv.tv_sec = timeoutMs / 1000;
		tv.tv_usec = (timeoutMs % 1000) * 1000;
		connected = (select(0, NULL, &wfds, &efds, &tv) > 0) && !FD_ISSET(s, &efds);
	}

	u_long blocking = 0;
	ioctlsocket(s, FIONBIO, &blocking);

	if (!connected) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

static bool SendGetRequest(SOCKET s, const char* host, const char* path) {
	char header[512];
	int headerLen = _snprintf(header, sizeof(header),
		"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
	if (headerLen < 0 || headerLen >= (int)sizeof(header))
		return false;
	return send(s, header, headerLen, 0) == headerLen;
}

// One-shot small response (the "version\tsize\tcrc32" line) - mirrors
// n02_watch.cpp's HttpGetBody. Returns body length, or -1 on any
// network/non-2xx error.
static int HttpGetSmall(const char* host, int port, const char* path, char* outBuf, int outCap) {
	SOCKET s = ConnectWithTimeout(host, port, 3000);
	if (s == INVALID_SOCKET) return -1;
	if (!SendGetRequest(s, host, path)) { closesocket(s); return -1; }

	char recvBuf[4096];
	int total = 0;
	while (total < (int)sizeof(recvBuf)) {
		int r = recv(s, recvBuf + total, sizeof(recvBuf) - total, 0);
		if (r <= 0) break;
		total += r;
	}
	closesocket(s);

	if (total < 12 || strncmp(recvBuf, "HTTP/1.", 7) != 0 || recvBuf[9] != '2')
		return -1;

	char* bodyStart = NULL;
	for (int i = 0; i + 3 < total; i++) {
		if (recvBuf[i] == '\r' && recvBuf[i + 1] == '\n' && recvBuf[i + 2] == '\r' && recvBuf[i + 3] == '\n') {
			bodyStart = recvBuf + i + 4;
			break;
		}
	}
	if (bodyStart == NULL) return -1;

	int bodyLen = total - (int)(bodyStart - recvBuf);
	if (bodyLen < 0) bodyLen = 0;
	if (bodyLen > outCap) bodyLen = outCap;
	if (bodyLen > 0) memcpy(outBuf, bodyStart, bodyLen);
	return bodyLen;
}

// Full download (the DLL itself) - loops recv() until the peer closes,
// since the payload doesn't fit in one TCP read like the small responses
// above do. outCap must already be sized to the expected download (the
// caller gets that from /updates/latest before calling this).
static int HttpGetToBuffer(const char* host, int port, const char* path, char* outBuf, int outCap) {
	SOCKET s = ConnectWithTimeout(host, port, 5000);
	if (s == INVALID_SOCKET) return -1;
	if (!SendGetRequest(s, host, path)) { closesocket(s); return -1; }

	char headBuf[8192];
	int headTotal = 0;
	char* bodyStart = NULL;
	while (headTotal < (int)sizeof(headBuf)) {
		int r = recv(s, headBuf + headTotal, sizeof(headBuf) - headTotal, 0);
		if (r <= 0) { closesocket(s); return -1; }
		headTotal += r;
		for (int i = 0; i + 3 < headTotal; i++) {
			if (headBuf[i] == '\r' && headBuf[i + 1] == '\n' && headBuf[i + 2] == '\r' && headBuf[i + 3] == '\n') {
				bodyStart = headBuf + i + 4;
				break;
			}
		}
		if (bodyStart != NULL) break;
	}
	if (bodyStart == NULL || headTotal < 12 || strncmp(headBuf, "HTTP/1.", 7) != 0 || headBuf[9] != '2') {
		closesocket(s);
		return -1;
	}

	int total = 0;
	int bodyInHead = headTotal - (int)(bodyStart - headBuf);
	if (bodyInHead > 0) {
		if (bodyInHead > outCap) bodyInHead = outCap;
		memcpy(outBuf, bodyStart, bodyInHead);
		total = bodyInHead;
	}
	while (total < outCap) {
		int r = recv(s, outBuf + total, outCap - total, 0);
		if (r <= 0) break;
		total += r;
	}
	closesocket(s);
	return total;
}

///////////////////////////////////////////////////////////////////////////////
// Swap: renames a staged kailleraclient.dll.new over the current DLL. Works
// even while this DLL is loaded and running - the Windows loader opens DLL
// files with share-delete/rename permission, so the currently-executing
// code keeps running unaffected from the renamed-away file, and whichever
// process loads "kailleraclient.dll" next picks up the new one. No reboot,
// no helper process needed - just a message telling the user to relaunch.
///////////////////////////////////////////////////////////////////////////////

static bool SwapInPendingUpdate(const char* dllPath) {
	char newPath[MAX_PATH];
	char oldPath[MAX_PATH];
	_snprintf(newPath, sizeof(newPath), "%s.new", dllPath);
	_snprintf(oldPath, sizeof(oldPath), "%s.old", dllPath);
	newPath[sizeof(newPath) - 1] = 0;
	oldPath[sizeof(oldPath) - 1] = 0;

	if (GetFileAttributesA(newPath) == INVALID_FILE_ATTRIBUTES)
		return false; // nothing staged

	DeleteFileA(oldPath); // best-effort - clear the way for the rename below

	if (!MoveFileExA(dllPath, oldPath, MOVEFILE_REPLACE_EXISTING))
		return false; // locked more strictly than usual this time - retry next launch

	if (!MoveFileExA(newPath, dllPath, MOVEFILE_REPLACE_EXISTING)) {
		// Try to put the previously-working DLL back rather than leave the
		// slot empty.
		MoveFileExA(oldPath, dllPath, MOVEFILE_REPLACE_EXISTING);
		return false;
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Version comparison. Release tags look like "v.TIERES.0.18" (older ones
// "v0.11"), and a CI build of a commit past its last tag carries git
// describe's "-<commits>-g<hash>" suffix, e.g. "v.TIERES.0.18-1-g15dd983" -
// see n02_version.h.
///////////////////////////////////////////////////////////////////////////////

#define N02_VERSION_MAX_PARTS 8

struct ParsedVersion {
	int parts[N02_VERSION_MAX_PARTS]; // the all-digit dot-separated components, e.g. {0, 18}
	int count;
	// git describe's "-<N>-g<hash>" -> N (newer than the plain tag); any
	// other suffix (e.g. "-beta") -> -1, a pre-release of that tag; 0 for
	// the plain tag itself.
	int suffixRank;
};

// False for anything that isn't a "v..." tag-based version - "dev", or a
// bare commit hash from a tagless `git describe --always`, whose hex digits
// would otherwise parse as nonsense version numbers.
static bool ParseVersion(const char* s, ParsedVersion* out) {
	memset(out, 0, sizeof(*out));
	if (s == NULL || (s[0] != 'v' && s[0] != 'V'))
		return false;

	const char* p = s + 1;
	const char* dash = strchr(p, '-');
	const char* baseEnd = (dash != NULL) ? dash : p + strlen(p);

	while (p < baseEnd) {
		const char* tokEnd = p;
		while (tokEnd < baseEnd && *tokEnd != '.')
			tokEnd++;
		bool numeric = (tokEnd > p);
		for (const char* c = p; c < tokEnd; c++) {
			if (*c < '0' || *c > '9') {
				numeric = false;
				break;
			}
		}
		if (numeric && out->count < N02_VERSION_MAX_PARTS)
			out->parts[out->count++] = atoi(p); // stops at the '.'/'-' ending the token
		p = (tokEnd < baseEnd) ? tokEnd + 1 : baseEnd;
	}
	if (out->count == 0)
		return false;

	if (dash != NULL) {
		const char* q = dash + 1;
		const char* digitsEnd = q;
		while (*digitsEnd >= '0' && *digitsEnd <= '9')
			digitsEnd++;
		if (digitsEnd > q && digitsEnd[0] == '-' && digitsEnd[1] == 'g')
			out->suffixRank = atoi(q);
		else
			out->suffixRank = -1;
	}
	return true;
}

// <0, 0 or >0, like strcmp. Missing trailing parts count as 0 (0.18 == 0.18.0).
static int CompareVersions(const ParsedVersion* a, const ParsedVersion* b) {
	int n = (a->count > b->count) ? a->count : b->count;
	for (int i = 0; i < n; i++) {
		int x = (i < a->count) ? a->parts[i] : 0;
		int y = (i < b->count) ? b->parts[i] : 0;
		if (x != y)
			return (x < y) ? -1 : 1;
	}
	if (a->suffixRank != b->suffixRank)
		return (a->suffixRank < b->suffixRank) ? -1 : 1;
	return 0;
}

// Version strings as the prompts and the package check compare them: <0 when
// remote is older than local, 0 when it's the same, >0 when it's newer. When
// either doesn't parse, any difference counts as newer (the plain check the
// DLL-only update always did).
static int CompareVersionStrings(const char* remote, const char* local) {
	ParsedVersion r, l;
	if (ParseVersion(remote, &r) && ParseVersion(local, &l))
		return CompareVersions(&r, &l);
	return strcmp(remote, local) == 0 ? 0 : 1;
}

///////////////////////////////////////////////////////////////////////////////
// Test server override - the n02.ini keys the account client already uses
// for a local wg-camp (n02_wecamp.cpp: [SC] WECAMP_HOST / WECAMP_PORT /
// WECAMP_HTTP=1, see tools/wecamp_test), read straight from the n02.ini next
// to this DLL: nSettings isn't initialized yet when kailleraInit() runs.
// With it set, both update paths talk to that server only - never to
// production.
///////////////////////////////////////////////////////////////////////////////

struct UpdateServer {
	wchar_t host[128];
	int port;
	bool secure;
};

static bool ReadServerOverride(const wchar_t* dir, UpdateServer* s) {
	wchar_t ini[MAX_PATH];
	_snwprintf(ini, MAX_PATH, L"%s\\n02.ini", dir);
	ini[MAX_PATH - 1] = 0;
	GetPrivateProfileStringW(L"SC", L"WECAMP_HOST", L"", s->host, 128, ini);
	if (s->host[0] == 0)
		return false;
	s->port = (int)GetPrivateProfileIntW(L"SC", L"WECAMP_PORT", N02_WECAMP_PORT, ini);
	s->secure = GetPrivateProfileIntW(L"SC", L"WECAMP_HTTP", 0, ini) == 0;
	return true;
}

// Directory part of a path (no trailing backslash).
static void DirectoryOf(const wchar_t* path, wchar_t* dir, int cap) {
	_snwprintf(dir, cap, L"%s", path);
	dir[cap - 1] = 0;
	wchar_t* slash = wcsrchr(dir, L'\\');
	if (slash != NULL)
		*slash = 0;
	else
		dir[0] = 0;
}

#ifdef _WIN64

///////////////////////////////////////////////////////////////////////////////
// Full-package update - the TIERES RetroArch (x64) only. The DLL running
// inside the TIERES retroarch.exe updates the whole light package - the exe,
// this DLL, the core - instead of just itself, then restarts RetroArch:
//
//  1. GET /updates/pacote/x64.txt (wg-camp's app/updates.py): the version of
//     the package's DLL, plus sha256/size/path of every file in it.
//  2. Hash the local copies. The ones that differ get offered: as a new
//     version, or - the package's DLL is this same version - as the rest of
//     an update that only reached the DLL (e.g. the DLL-only updater of an
//     older DLL swapped this one in).
//  3. Download each one to "<file>.new" by its sha256
//     (/updates/pacote/arquivos/<sha256>), checking size and sha256, behind
//     a progress window.
//  4. Swap them all in: "<file>" -> "<file>.old", "<file>.new" -> "<file>".
//     Windows lets a running exe or a loaded DLL be renamed (not
//     overwritten) - the same trick SwapInPendingUpdate() uses. The .old
//     files are deleted by the next run, once nothing has them open.
//  5. Restart: rundll32 runs kailleraUpdateRelaunchW from the new DLL, which
//     closes RetroArch with a WM_CLOSE - its normal exit, which writes the
//     memory card and the config like closing the window - waits for this
//     process to end and starts retroarch.exe again with the same arguments.
//
// Anything else (another emulator, the x86 DLL, a server without the
// package) keeps the DLL-only update below.
//
// The site is reached over HTTPS, except under Wine: its HTTPS crashes in
// some builds (Batocera's wine-tkg), so there it's the plain-HTTP :8080
// listener. Either way every file is checked against the manifest's sha256.
///////////////////////////////////////////////////////////////////////////////

#define N02_PACKAGE_ARCH_PATH L"/updates/pacote/x64.txt"
#define N02_PACKAGE_FILE_PATH L"/updates/pacote/arquivos/"
#define N02_PACKAGE_MAX_FILES 16
#define N02_PACKAGE_MAX_FILE_SIZE (64 * 1024 * 1024)
#define N02_PACKAGE_EXE "retroarch.exe"
#define N02_PACKAGE_SELF "kailleraclient.dll"
// In every TIERES retroarch.exe (its version string, "1.16.0.K.FFW.TIERES.x")
// - another RetroArch with Kaillera support keeps the DLL-only update.
#define N02_PACKAGE_EXE_MARKER "FFW.TIERES"
// The relauncher asks RetroArch to close every N02_RELAUNCH_CLOSE_EVERY_MS
// for N02_RELAUNCH_CLOSE_FOR_MS, and waits for it N02_RELAUNCH_WAIT_MS.
#define N02_RELAUNCH_CLOSE_EVERY_MS 1500
#define N02_RELAUNCH_CLOSE_FOR_MS (60 * 1000)
#define N02_RELAUNCH_WAIT_MS (5 * 60 * 1000)

static bool g_restart_pending = false;

static bool RunningUnderWine() {
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	return ntdll != NULL && GetProcAddress(ntdll, "wine_get_version") != NULL;
}

static void GetPackageServer(const wchar_t* dir, UpdateServer* s) {
	if (ReadServerOverride(dir, s))
		return;
	_snwprintf(s->host, 128, L"%S", N02_WECAMP_HOST);
	s->host[127] = 0;
	if (RunningUnderWine()) {
		s->port = N02_STREAM_DEFAULT_PORT;
		s->secure = false;
	} else {
		s->port = N02_WECAMP_PORT;
		s->secure = true;
	}
}

///////////////////////////////////////////////////////////////////////////////
// WinHTTP GET, streaming the body to a callback.
///////////////////////////////////////////////////////////////////////////////

// Gets each chunk of the body; returns false to abort the download.
typedef bool (*HttpSink)(void* ctx, const char* data, DWORD len);

// Returns the HTTP status (the body is only read for a 200), or 0 when the
// server couldn't be reached, the connection broke or the sink aborted.
static int WinHttpGetTo(const UpdateServer* srv, const wchar_t* path, HttpSink sink, void* ctx) {
	HINTERNET session = WinHttpOpen(L"n02-kailleraclient", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
		return 0;
	WinHttpSetTimeouts(session, 5000, 5000, 10000, 30000);
	HINTERNET connect = WinHttpConnect(session, srv->host, (INTERNET_PORT)srv->port, 0);
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, NULL,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, srv->secure ? WINHTTP_FLAG_SECURE : 0) : NULL;

	int status = 0;
	if (request
		&& WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
		&& WinHttpReceiveResponse(request, NULL)) {
		DWORD code = 0, size = sizeof(code);
		if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX))
			status = (int)code;
		if (status == 200) {
			static const DWORD chunkSize = 64 * 1024;
			char* chunk = (char*)malloc(chunkSize);
			for (;;) {
				DWORD avail = 0, got = 0;
				if (chunk == NULL || !WinHttpQueryDataAvailable(request, &avail)) {
					status = 0;
					break;
				}
				if (avail == 0)
					break; // end of the body
				if (avail > chunkSize)
					avail = chunkSize;
				if (!WinHttpReadData(request, chunk, avail, &got) || got == 0
					|| !sink(ctx, chunk, got)) {
					status = 0;
					break;
				}
			}
			free(chunk);
		}
	}
	if (request) WinHttpCloseHandle(request);
	if (connect) WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);
	return status;
}

struct MemorySink {
	char* buf;
	int cap;
	int len;
};

static bool MemorySinkWrite(void* ctx, const char* data, DWORD len) {
	MemorySink* m = (MemorySink*)ctx;
	if ((int)len > m->cap - m->len)
		return false; // far bigger than any manifest
	memcpy(m->buf + m->len, data, len);
	m->len += len;
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// SHA-256 (BCrypt), in pieces.
///////////////////////////////////////////////////////////////////////////////

struct Sha256 {
	BCRYPT_ALG_HANDLE alg;
	BCRYPT_HASH_HANDLE hash;
};

static bool Sha256Begin(Sha256* s) {
	s->alg = NULL;
	s->hash = NULL;
	if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) {
		s->alg = NULL;
		return false;
	}
	if (BCryptCreateHash(s->alg, &s->hash, NULL, 0, NULL, 0, 0) != 0) {
		BCryptCloseAlgorithmProvider(s->alg, 0);
		s->alg = NULL;
		s->hash = NULL;
		return false;
	}
	return true;
}

static void Sha256Update(Sha256* s, const void* data, DWORD len) {
	if (s->hash != NULL && len > 0)
		BCryptHashData(s->hash, (PUCHAR)data, len, 0);
}

// Lowercase hex into hex[65]; also releases s (call it exactly once).
static bool Sha256Finish(Sha256* s, char hex[65]) {
	unsigned char digest[32];
	bool ok = s->hash != NULL && BCryptFinishHash(s->hash, digest, sizeof(digest), 0) == 0;
	if (s->hash != NULL) BCryptDestroyHash(s->hash);
	if (s->alg != NULL) BCryptCloseAlgorithmProvider(s->alg, 0);
	s->hash = NULL;
	s->alg = NULL;
	hex[0] = 0;
	if (!ok)
		return false;
	for (int i = 0; i < 32; i++)
		sprintf(hex + i * 2, "%02x", digest[i]);
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Manifest (wg-camp app/updates.py). Tab-separated lines:
//
//     versao   v.TIERES.0.25
//     pacote   RetroArch-1.16.0.FFW.TIERES.0.6
//     arquivo  <sha256>  <bytes>  retroarch.exe
//
// Unknown line types are skipped (room for later additions).
///////////////////////////////////////////////////////////////////////////////

struct PackageFile {
	char path[MAX_PATH];  // relative to the RetroArch folder, '/' separated
	char sha256[65];
	long long size;
	bool download;        // differs from the local copy (filled by the check)
};

struct Package {
	char version[64];
	char name[128];
	PackageFile files[N02_PACKAGE_MAX_FILES];
	int count;
};

static bool EndsWithNoCase(const char* s, const char* suffix) {
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && _stricmp(s + n - m, suffix) == 0;
}

// Only plain relative paths inside the RetroArch folder, and only binaries:
// the light package never carries settings (retroarch.cfg, core options,
// graphics) - a manifest naming one must not overwrite a player's.
static bool SafePackagePath(const char* p) {
	size_t len = strlen(p);
	if (len == 0 || len >= MAX_PATH - 8 || p[0] == '/' || p[len - 1] == '/')
		return false;
	for (const char* c = p; *c; c++) {
		bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
			|| *c == '.' || *c == '_' || *c == '-' || *c == ' ' || *c == '/';
		if (!ok)
			return false;
	}
	const char* seg = p;
	for (;;) {
		const char* end = strchr(seg, '/');
		size_t segLen = end ? (size_t)(end - seg) : strlen(seg);
		if (segLen == 0 || (segLen == 1 && seg[0] == '.') || (segLen == 2 && seg[0] == '.' && seg[1] == '.'))
			return false;
		if (end == NULL)
			break;
		seg = end + 1;
	}
	return EndsWithNoCase(p, ".exe") || EndsWithNoCase(p, ".dll") || EndsWithNoCase(p, ".lck");
}

static bool IsSha256Hex(const char* s) {
	if (strlen(s) != 64)
		return false;
	for (int i = 0; i < 64; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	return true;
}

// Splits line at tabs (in place) into at most max fields; returns the count.
static int SplitTabs(char* line, char** fields, int max) {
	int n = 0;
	char* p = line;
	while (n < max) {
		fields[n++] = p;
		char* tab = strchr(p, '\t');
		if (tab == NULL)
			break;
		*tab = 0;
		p = tab + 1;
	}
	return n;
}

static bool ParseManifest(char* text, Package* pkg) {
	memset(pkg, 0, sizeof(*pkg));
	char* line = text;
	while (line != NULL && *line) {
		char* next = strchr(line, '\n');
		if (next != NULL)
			*next++ = 0;
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\r')
			line[len - 1] = 0;

		char* f[5];
		int n = SplitTabs(line, f, 5);
		if (n == 2 && strcmp(f[0], "versao") == 0) {
			_snprintf(pkg->version, sizeof(pkg->version), "%s", f[1]);
			pkg->version[sizeof(pkg->version) - 1] = 0;
		} else if (n == 2 && strcmp(f[0], "pacote") == 0) {
			_snprintf(pkg->name, sizeof(pkg->name), "%s", f[1]);
			pkg->name[sizeof(pkg->name) - 1] = 0;
		} else if (n == 4 && strcmp(f[0], "arquivo") == 0) {
			if (pkg->count >= N02_PACKAGE_MAX_FILES || !IsSha256Hex(f[1]) || !SafePackagePath(f[3]))
				return false;
			char* end = NULL;
			long long size = _strtoi64(f[2], &end, 10);
			if (end == f[2] || *end != 0 || size < 0 || size > N02_PACKAGE_MAX_FILE_SIZE)
				return false;
			for (int i = 0; i < pkg->count; i++)
				if (_stricmp(pkg->files[i].path, f[3]) == 0)
					return false;
			PackageFile* pf = &pkg->files[pkg->count++];
			strcpy(pf->path, f[3]);
			strcpy(pf->sha256, f[1]);
			pf->size = size;
		}
		line = next;
	}
	return pkg->version[0] != 0 && pkg->count > 0;
}

///////////////////////////////////////////////////////////////////////////////
// Local files.
///////////////////////////////////////////////////////////////////////////////

// dir\path (path's '/' turned into '\'), plus suffix.
static void PackageFilePath(const wchar_t* dir, const char* path, const wchar_t* suffix, wchar_t* out, int cap) {
	_snwprintf(out, cap, L"%s\\%S%s", dir, path, suffix);
	out[cap - 1] = 0;
	for (wchar_t* c = out; *c; c++)
		if (*c == L'/')
			*c = L'\\';
}

// sha256 of a local file; false if it can't be read. With a marker, *found
// says whether those bytes appear anywhere in the file.
static bool HashLocalFile(const wchar_t* path, char hex[65], long long* size, const char* marker, bool* found) {
	if (found) *found = false;
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return false;

	static const DWORD chunkSize = 256 * 1024;
	size_t markerLen = marker ? strlen(marker) : 0;
	// carry (the last markerLen-1 bytes of the previous chunk) + chunk, so a
	// marker split between two reads is still found
	char* buf = (char*)malloc(chunkSize + markerLen);
	Sha256 sha;
	bool ok = buf != NULL && Sha256Begin(&sha);
	if (!ok) {
		free(buf);
		CloseHandle(f);
		return false;
	}

	long long total = 0;
	size_t carry = 0;
	for (;;) {
		DWORD got = 0;
		if (!ReadFile(f, buf + carry, chunkSize, &got, NULL)) {
			ok = false;
			break;
		}
		if (got == 0)
			break;
		Sha256Update(&sha, buf + carry, got);
		total += got;
		size_t have = carry + got;
		if (markerLen > 0 && found && !*found) {
			for (const char* p = buf; p + markerLen <= buf + have; p++) {
				p = (const char*)memchr(p, marker[0], (buf + have) - p);
				if (p == NULL || p + markerLen > buf + have)
					break;
				if (memcmp(p, marker, markerLen) == 0) {
					*found = true;
					break;
				}
			}
		}
		carry = 0;
		if (markerLen > 1) {
			carry = (have < markerLen - 1) ? have : markerLen - 1;
			memmove(buf, buf + have - carry, carry);
		}
	}
	char digest[65];
	bool hashed = Sha256Finish(&sha, digest);
	free(buf);
	CloseHandle(f);
	if (!ok || !hashed)
		return false;
	strcpy(hex, digest);
	*size = total;
	return true;
}

static bool FileExists(const wchar_t* path) {
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Parent folders of dir\path (e.g. "cores").
static void CreateParentDirs(const wchar_t* dir, const char* path) {
	char partial[MAX_PATH];
	for (const char* c = path; *c; c++) {
		if (*c != '/')
			continue;
		size_t n = c - path;
		memcpy(partial, path, n);
		partial[n] = 0;
		wchar_t full[MAX_PATH];
		PackageFilePath(dir, partial, L"", full, MAX_PATH);
		CreateDirectoryW(full, NULL);
	}
}

// "<file>.old" leftovers of a previous update: the plain one and .old2-.old9
// (used when an older .old was still open).
static void OldFilePath(const wchar_t* dir, const char* path, int n, wchar_t* out, int cap) {
	wchar_t suffix[8];
	if (n <= 1)
		wcscpy(suffix, L".old");
	else
		_snwprintf(suffix, 8, L".old%d", n);
	PackageFilePath(dir, path, suffix, out, cap);
}

static void DeleteOldFiles(const wchar_t* dir, const Package* pkg) {
	for (int i = 0; i < pkg->count; i++) {
		for (int n = 1; n <= 9; n++) {
			wchar_t old[MAX_PATH];
			OldFilePath(dir, pkg->files[i].path, n, old, MAX_PATH);
			DeleteFileW(old); // still open somewhere = next time
		}
	}
}

static void DeleteNewFiles(const wchar_t* dir, const Package* pkg) {
	for (int i = 0; i < pkg->count; i++) {
		wchar_t neu[MAX_PATH];
		PackageFilePath(dir, pkg->files[i].path, L".new", neu, MAX_PATH);
		DeleteFileW(neu);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Download (worker thread) + progress window (the thread that called
// kailleraInit - RetroArch's main thread - pumping messages meanwhile, as a
// MessageBox would).
///////////////////////////////////////////////////////////////////////////////

struct DownloadJob {
	UpdateServer server;
	const wchar_t* dir;
	Package* pkg;
	long long total;            // bytes to download
	volatile LONGLONG done;     // bytes so far, all files
	volatile LONG current;      // index of the file being downloaded
	volatile LONG cancel;
	bool ok;
	wchar_t error[256];         // why it failed (shown to the player)
};

struct FileSink {
	HANDLE file;
	Sha256 sha;
	long long got;
	long long expected;
	DownloadJob* job;
	bool writeFailed;
};

static bool FileSinkWrite(void* ctx, const char* data, DWORD len) {
	FileSink* s = (FileSink*)ctx;
	if (s->job->cancel || s->got + len > s->expected)
		return false;
	DWORD written = 0;
	if (!WriteFile(s->file, data, len, &written, NULL) || written != len) {
		s->writeFailed = true;
		return false;
	}
	Sha256Update(&s->sha, data, len);
	s->got += len;
	InterlockedExchangeAdd64(&s->job->done, len);
	return true;
}

static void JobError(DownloadJob* job, const wchar_t* fmt, const char* path) {
	_snwprintf(job->error, 256, fmt, path);
	job->error[255] = 0;
}

static DWORD WINAPI DownloadThread(LPVOID param) {
	DownloadJob* job = (DownloadJob*)param;
	job->ok = false;
	for (int i = 0; i < job->pkg->count; i++) {
		PackageFile* pf = &job->pkg->files[i];
		if (!pf->download)
			continue;
		InterlockedExchange(&job->current, i);

		wchar_t neu[MAX_PATH];
		PackageFilePath(job->dir, pf->path, L".new", neu, MAX_PATH);
		CreateParentDirs(job->dir, pf->path);
		FileSink sink;
		sink.file = CreateFileW(neu, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (sink.file == INVALID_HANDLE_VALUE) {
			JobError(job, L"sem permiss\u00e3o para gravar na pasta do RetroArch (%S)", pf->path);
			return 0;
		}
		sink.got = 0;
		sink.expected = pf->size;
		sink.job = job;
		sink.writeFailed = false;
		if (!Sha256Begin(&sink.sha)) {
			CloseHandle(sink.file);
			JobError(job, L"erro interno ao conferir %S", pf->path);
			return 0;
		}

		int status = 200;
		if (pf->size > 0) {
			wchar_t urlPath[160];
			_snwprintf(urlPath, 160, L"%s%S", N02_PACKAGE_FILE_PATH, pf->sha256);
			urlPath[159] = 0;
			status = WinHttpGetTo(&job->server, urlPath, FileSinkWrite, &sink);
		}
		char digest[65];
		bool hashed = Sha256Finish(&sink.sha, digest);
		CloseHandle(sink.file);

		if (job->cancel) {
			wcscpy(job->error, L"cancelado");
			return 0;
		}
		if (sink.writeFailed) {
			JobError(job, L"n\u00e3o foi poss\u00edvel gravar %S (disco cheio?)", pf->path);
			return 0;
		}
		if (status == 0) {
			JobError(job, L"a conex\u00e3o com o WE Camp caiu ao baixar %S", pf->path);
			return 0;
		}
		if (status != 200) {
			_snwprintf(job->error, 256, L"o WE Camp n\u00e3o tem o arquivo %S (HTTP %d)", pf->path, status);
			job->error[255] = 0;
			return 0;
		}
		if (!hashed || sink.got != pf->size || strcmp(digest, pf->sha256) != 0) {
			JobError(job, L"%S chegou incompleto ou corrompido", pf->path);
			return 0;
		}
	}
	job->ok = true;
	return 0;
}

#define N02_PROGRESS_CLASS L"N02AtualizacaoRetroArch"
#define N02_PROGRESS_TEXT 101
#define N02_PROGRESS_BAR 102
#define N02_PROGRESS_TIMER 1

// "12,3 MB" (Brazilian decimal comma).
static void FormatMB(long long bytes, wchar_t* out, int cap) {
	long long tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
	if (bytes > 0 && tenths == 0)
		tenths = 1;
	_snwprintf(out, cap, L"%lld,%lld MB", tenths / 10, tenths % 10);
	out[cap - 1] = 0;
}

static void UpdateProgress(HWND hwnd, DownloadJob* job) {
	if (job->cancel)
		return;
	int index = 0, count = 0, current = job->current;
	for (int i = 0; i < job->pkg->count; i++) {
		if (!job->pkg->files[i].download)
			continue;
		count++;
		if (i <= current)
			index = count;
	}
	wchar_t done[32], total[32], text[MAX_PATH + 96];
	FormatMB(job->done, done, 32);
	FormatMB(job->total, total, 32);
	_snwprintf(text, MAX_PATH + 96, L"Baixando %S (%d de %d)\n%s de %s",
		job->pkg->files[current].path, index, count, done, total);
	text[MAX_PATH + 95] = 0;
	wchar_t shown[MAX_PATH + 96];
	if (GetDlgItemTextW(hwnd, N02_PROGRESS_TEXT, shown, MAX_PATH + 96) == 0 || wcscmp(shown, text) != 0)
		SetDlgItemTextW(hwnd, N02_PROGRESS_TEXT, text); // only on a change: a STATIC flickers on every set
	int pos = job->total > 0 ? (int)(job->done * 1000 / job->total) : 0;
	SendDlgItemMessageW(hwnd, N02_PROGRESS_BAR, PBM_SETPOS, pos, 0);
}

static LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	DownloadJob* job = (DownloadJob*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_TIMER:
		if (job != NULL)
			UpdateProgress(hwnd, job);
		return 0;
	case WM_COMMAND:
		if (LOWORD(wp) != IDCANCEL)
			break;
		// fall through
	case WM_CLOSE:
		if (job != NULL && !job->cancel) {
			InterlockedExchange(&job->cancel, 1);
			EnableWindow(GetDlgItem(hwnd, IDCANCEL), FALSE);
			SetDlgItemTextW(hwnd, N02_PROGRESS_TEXT, L"Cancelando...");
		}
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

struct FindWindowData {
	DWORD pid;
	HWND hwnd;
};

static BOOL CALLBACK FindRetroArchWindowProc(HWND hwnd, LPARAM lp) {
	FindWindowData* d = (FindWindowData*)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	wchar_t cls[32];
	if (pid == d->pid && GetClassNameW(hwnd, cls, 32) > 0 && wcscmp(cls, L"RetroArch") == 0) {
		d->hwnd = hwnd;
		return FALSE;
	}
	return TRUE;
}

// RetroArch's main window (window class "RetroArch", gfx/common/win32_common.c
// in the fork) in process pid - this one by default - or NULL.
static HWND FindRetroArchWindow(DWORD pid = 0) {
	FindWindowData d;
	d.pid = pid ? pid : GetCurrentProcessId();
	d.hwnd = NULL;
	EnumWindows(FindRetroArchWindowProc, (LPARAM)&d);
	return d.hwnd;
}

static HWND CreateProgressWindow(DownloadJob* job, HFONT font) {
	INITCOMMONCONTROLSEX icc;
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_PROGRESS_CLASS;
	InitCommonControlsEx(&icc);

	WNDCLASSEXW wc;
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = ProgressWndProc;
	wc.hInstance = hx;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = N02_PROGRESS_CLASS;
	RegisterClassExW(&wc); // already registered by an earlier load: fine

	// Layout in 96-DPI pixels, scaled: RetroArch is DPI-aware, so at 125%/150%
	// the message font grows and fixed pixels would cut the text.
	HDC screen = GetDC(NULL);
	int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
	if (screen) ReleaseDC(NULL, screen);
	if (dpi <= 0) dpi = 96;
#define N02_DPI(v) MulDiv((v), dpi, 96)

	RECT rc = { 0, 0, N02_DPI(440), N02_DPI(124) };
	DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
	DWORD exStyle = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
	AdjustWindowRectEx(&rc, style, FALSE, exStyle);
	int w = rc.right - rc.left, h = rc.bottom - rc.top;

	// Centered on RetroArch's window (or its monitor), always inside that
	// monitor's work area - the window may be hidden, tiny or half off-screen.
	HWND ra = FindRetroArchWindow();
	bool onWindow = ra != NULL && IsWindowVisible(ra) && !IsIconic(ra);
	MONITORINFO mi;
	mi.cbSize = sizeof(mi);
	RECT work;
	if (GetMonitorInfoW(MonitorFromWindow(onWindow ? ra : NULL, MONITOR_DEFAULTTOPRIMARY), &mi))
		work = mi.rcWork;
	else
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	RECT area = work;
	if (onWindow)
		GetWindowRect(ra, &area);
	int x = area.left + ((area.right - area.left) - w) / 2;
	int y = area.top + ((area.bottom - area.top) - h) / 2;
	if (x > work.right - w) x = work.right - w;
	if (y > work.bottom - h) y = work.bottom - h;
	if (x < work.left) x = work.left;
	if (y < work.top) y = work.top;

	HWND hwnd = CreateWindowExW(exStyle, N02_PROGRESS_CLASS, L"Atualizando o RetroArch TIERES",
		style, x, y, w, h, NULL, NULL, hx, NULL);
	if (hwnd == NULL)
		return NULL;
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)job);

	HWND text = CreateWindowExW(0, L"STATIC", L"Conectando ao WE Camp...", WS_CHILD | WS_VISIBLE,
		N02_DPI(16), N02_DPI(12), N02_DPI(408), N02_DPI(40), hwnd, (HMENU)(INT_PTR)N02_PROGRESS_TEXT, hx, NULL);
	HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
		N02_DPI(16), N02_DPI(58), N02_DPI(408), N02_DPI(18), hwnd, (HMENU)(INT_PTR)N02_PROGRESS_BAR, hx, NULL);
	HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		N02_DPI(334), N02_DPI(88), N02_DPI(90), N02_DPI(26), hwnd, (HMENU)(INT_PTR)IDCANCEL, hx, NULL);
#undef N02_DPI
	SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
	if (font != NULL) {
		SendMessageW(text, WM_SETFONT, (WPARAM)font, FALSE);
		SendMessageW(cancel, WM_SETFONT, (WPARAM)font, FALSE);
	}
	SetTimer(hwnd, N02_PROGRESS_TIMER, 150, NULL);
	ShowWindow(hwnd, SW_SHOW);
	SetForegroundWindow(hwnd);
	return hwnd;
}

static bool RunDownload(DownloadJob* job) {
	NONCLIENTMETRICSW ncm;
	memset(&ncm, 0, sizeof(ncm));
	ncm.cbSize = sizeof(ncm);
	HFONT font = NULL;
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
		font = CreateFontIndirectW(&ncm.lfMessageFont);

	HANDLE thread = CreateThread(NULL, 0, DownloadThread, job, 0, NULL);
	if (thread == NULL) {
		if (font) DeleteObject(font);
		wcscpy(job->error, L"erro interno (thread)");
		return false;
	}
	HWND wnd = CreateProgressWindow(job, font);

	bool quit = false;
	WPARAM quitCode = 0;
	for (;;) {
		DWORD r = MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_ALLINPUT);
		if (r != WAIT_OBJECT_0 + 1)
			break; // the download finished (or the wait itself failed)
		MSG m;
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
			if (m.message == WM_QUIT) {
				// RetroArch is quitting: stop, and hand the message back below
				quit = true;
				quitCode = m.wParam;
				InterlockedExchange(&job->cancel, 1);
				continue;
			}
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	if (wnd != NULL)
		DestroyWindow(wnd);
	UnregisterClassW(N02_PROGRESS_CLASS, hx);
	if (font) DeleteObject(font);
	if (quit)
		PostQuitMessage((int)quitCode);
	return job->ok;
}

///////////////////////////////////////////////////////////////////////////////
// Swap + restart.
///////////////////////////////////////////////////////////////////////////////

// Moves every downloaded "<file>.new" into place, the current file going to
// "<file>.old". All or nothing: on a failure the ones already swapped go
// back, and failedPath says which file couldn't be replaced.
static bool SwapPackage(const wchar_t* dir, const Package* pkg, char* failedPath, int failedCap) {
	struct Step { int index; bool hadOld; wchar_t old[MAX_PATH]; };
	Step steps[N02_PACKAGE_MAX_FILES];
	int done = 0;
	bool ok = true;

	for (int i = 0; i < pkg->count && ok; i++) {
		const PackageFile* pf = &pkg->files[i];
		if (!pf->download)
			continue;
		wchar_t full[MAX_PATH], neu[MAX_PATH];
		PackageFilePath(dir, pf->path, L"", full, MAX_PATH);
		PackageFilePath(dir, pf->path, L".new", neu, MAX_PATH);
		Step* st = &steps[done];
		st->index = i;
		st->hadOld = FileExists(full);
		if (st->hadOld) {
			// first free .old name (an older one may still be open)
			int n = 1;
			for (; n <= 9; n++) {
				OldFilePath(dir, pf->path, n, st->old, MAX_PATH);
				DeleteFileW(st->old);
				if (!FileExists(st->old))
					break;
			}
			if (n > 9 || !MoveFileExW(full, st->old, 0)) {
				ok = false;
			} else if (!MoveFileExW(neu, full, 0)) {
				MoveFileExW(st->old, full, 0);
				ok = false;
			}
		} else if (!MoveFileExW(neu, full, 0)) {
			ok = false;
		}
		if (ok) {
			done++;
		} else {
			_snprintf(failedPath, failedCap, "%s", pf->path);
			failedPath[failedCap - 1] = 0;
		}
	}
	if (ok)
		return true;

	while (done-- > 0) {
		const PackageFile* pf = &pkg->files[steps[done].index];
		wchar_t full[MAX_PATH], neu[MAX_PATH];
		PackageFilePath(dir, pf->path, L"", full, MAX_PATH);
		PackageFilePath(dir, pf->path, L".new", neu, MAX_PATH);
		MoveFileExW(full, neu, MOVEFILE_REPLACE_EXISTING);
		if (steps[done].hadOld)
			MoveFileExW(steps[done].old, full, 0);
	}
	return false;
}

// The command line minus its first argument (the program), as the Windows
// argument rules split it.
static const wchar_t* SkipFirstArgument(const wchar_t* cmd) {
	const wchar_t* p = cmd;
	if (*p == L'"') {
		p++;
		while (*p && *p != L'"')
			p++;
		if (*p == L'"')
			p++;
	} else {
		while (*p && *p != L' ' && *p != L'\t')
			p++;
	}
	while (*p == L' ' || *p == L'\t')
		p++;
	return p;
}

// Starts "rundll32 <this DLL>,kailleraUpdateRelaunch <pid> "<exe>" <args>":
// once this process ends, it starts retroarch.exe again with the same
// arguments (and, inherited, the same working folder and environment).
static bool StartRelauncher(const wchar_t* dllPath) {
	wchar_t exe[MAX_PATH], sys[MAX_PATH], rundll[MAX_PATH];
	if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0 || GetSystemDirectoryW(sys, MAX_PATH) == 0)
		return false;
	_snwprintf(rundll, MAX_PATH, L"%s\\rundll32.exe", sys);
	rundll[MAX_PATH - 1] = 0;

	const wchar_t* args = SkipFirstArgument(GetCommandLineW());
	size_t cap = wcslen(rundll) + wcslen(dllPath) + wcslen(exe) + wcslen(args) + 96;
	wchar_t* cmd = (wchar_t*)malloc(cap * sizeof(wchar_t));
	if (cmd == NULL)
		return false;
	_snwprintf(cmd, cap, L"\"%s\" \"%s\",kailleraUpdateRelaunch %lu \"%s\"%s%s",
		rundll, dllPath, GetCurrentProcessId(), exe, args[0] ? L" " : L"", args);
	cmd[cap - 1] = 0;

	STARTUPINFOW si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	BOOL ok = CreateProcessW(rundll, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
	free(cmd);
	if (!ok)
		return false;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// The package check itself.
///////////////////////////////////////////////////////////////////////////////

enum PackageResult {
	PACKAGE_NOT_USED, // nothing offered (not the TIERES RetroArch, no package, nothing new): the DLL-only check runs
	PACKAGE_HANDLED   // offered: declined, updated or failed - nothing else to do
};

// The RetroArch folder: the running exe is retroarch.exe and this DLL sits
// next to it (the light package's layout). Whether it's the TIERES build is
// checked later, while hashing the exe.
static bool GetPackageDir(const wchar_t* dllPath, wchar_t* dir, int cap) {
	wchar_t exe[MAX_PATH], exeDir[MAX_PATH];
	if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0)
		return false;
	const wchar_t* name = wcsrchr(exe, L'\\');
	if (name == NULL || _wcsicmp(name + 1, L"" N02_PACKAGE_EXE) != 0)
		return false;
	DirectoryOf(exe, exeDir, MAX_PATH);
	DirectoryOf(dllPath, dir, cap);
	return exeDir[0] != 0 && _wcsicmp(exeDir, dir) == 0;
}

static int AskPackageUpdate(const Package* pkg, int cmp, long long total) {
	wchar_t files[N02_PACKAGE_MAX_FILES * 72];
	files[0] = 0;
	for (int i = 0; i < pkg->count; i++) {
		if (!pkg->files[i].download)
			continue;
		size_t used = wcslen(files);
		_snwprintf(files + used, sizeof(files) / sizeof(files[0]) - used, L"  %.64S\n", pkg->files[i].path);
		files[sizeof(files) / sizeof(files[0]) - 1] = 0;
	}
	wchar_t size[32];
	FormatMB(total, size, 32);
	wchar_t name[160];
	name[0] = 0;
	if (pkg->name[0])
		_snwprintf(name, 160, L" (%S)", pkg->name);
	name[159] = 0;

	wchar_t msg[2048];
	if (cmp > 0) {
		_snwprintf(msg, 2048,
			L"Uma nova vers\u00e3o do RetroArch TIERES est\u00e1 dispon\u00edvel:\n\n"
			L"  Atual: %S\n  Nova:  %S%s\n\n"
			L"Arquivos que v\u00e3o ser atualizados (%s):\n%s\n"
			L"Deseja baixar e atualizar agora? O RetroArch vai fechar e abrir de novo sozinho no final.",
			N02_VERSION, pkg->version, name, size, files);
	} else {
		_snwprintf(msg, 2048,
			L"H\u00e1 arquivos novos do RetroArch TIERES %S%s para baixar (%s):\n\n%s\n"
			L"Deseja baixar e atualizar agora? O RetroArch vai fechar e abrir de novo sozinho no final.",
			pkg->version, name, size, files);
	}
	msg[2047] = 0;
	return MessageBoxW(NULL, msg, L"Atualiza\u00e7\u00e3o dispon\u00edvel", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
}

static PackageResult CheckPackageUpdate(const wchar_t* dllPath) {
	wchar_t dir[MAX_PATH];
	if (!GetPackageDir(dllPath, dir, MAX_PATH))
		return PACKAGE_NOT_USED;

	DownloadJob job;
	memset(&job, 0, sizeof(job));
	GetPackageServer(dir, &job.server);

	static char text[16 * 1024];
	MemorySink mem;
	mem.buf = text;
	mem.cap = sizeof(text) - 1;
	mem.len = 0;
	if (WinHttpGetTo(&job.server, N02_PACKAGE_ARCH_PATH, MemorySinkWrite, &mem) != 200)
		return PACKAGE_NOT_USED; // not published (yet), or offline
	text[mem.len] = 0;

	static Package pkg;
	if (!ParseManifest(text, &pkg))
		return PACKAGE_NOT_USED;

	// Older than this build (a tester's): never offer it - the DLL-only
	// check says the same.
	int cmp = CompareVersionStrings(pkg.version, N02_VERSION);
	if (cmp < 0)
		return PACKAGE_NOT_USED;

	bool tieres = false;
	long long total = 0;
	int count = 0;
	for (int i = 0; i < pkg.count; i++) {
		PackageFile* pf = &pkg.files[i];
		wchar_t full[MAX_PATH];
		PackageFilePath(dir, pf->path, L"", full, MAX_PATH);
		bool isExe = _stricmp(pf->path, N02_PACKAGE_EXE) == 0;

		char hex[65];
		long long size = -1;
		bool found = false, same = false;
		WIN32_FILE_ATTRIBUTE_DATA attr;
		if (GetFileAttributesExW(full, GetFileExInfoStandard, &attr)) {
			long long onDisk = ((long long)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
			// the exe is always read: it also says whether this is the TIERES build
			if ((onDisk == pf->size || isExe)
				&& HashLocalFile(full, hex, &size, isExe ? N02_PACKAGE_EXE_MARKER : NULL, &found))
				same = size == pf->size && strcmp(hex, pf->sha256) == 0;
		}
		if (isExe)
			tieres = found;
		// Same version: this DLL is the package's, whichever build of it.
		bool self = _stricmp(pf->path, N02_PACKAGE_SELF) == 0;
		pf->download = !same && !(cmp == 0 && self);
		if (pf->download) {
			total += pf->size;
			count++;
		}
	}
	if (!tieres)
		return PACKAGE_NOT_USED;

	DeleteOldFiles(dir, &pkg); // left by the previous update
	// Nothing new in the package - a DLL published without it (publish_release.sh
	// --so-dll) still reaches us through the DLL-only check.
	if (count == 0)
		return PACKAGE_NOT_USED;

	if (AskPackageUpdate(&pkg, cmp, total) != IDYES)
		return PACKAGE_HANDLED;

	job.dir = dir;
	job.pkg = &pkg;
	job.total = total;
	for (int i = 0; i < pkg.count; i++) {
		if (pkg.files[i].download) {
			job.current = i;
			break;
		}
	}
	bool ok = RunDownload(&job);
	if (!ok) {
		DeleteNewFiles(dir, &pkg);
		if (wcscmp(job.error, L"cancelado") != 0) {
			wchar_t msg[512];
			_snwprintf(msg, 512,
				L"N\u00e3o foi poss\u00edvel baixar a atualiza\u00e7\u00e3o: %s.\n\n"
				L"Nada foi alterado. Tente de novo mais tarde.", job.error);
			msg[511] = 0;
			MessageBoxW(NULL, msg, L"Atualiza\u00e7\u00e3o", MB_OK | MB_ICONWARNING | MB_TOPMOST);
		}
		return PACKAGE_HANDLED;
	}

	char failed[MAX_PATH];
	if (!SwapPackage(dir, &pkg, failed, sizeof(failed))) {
		DeleteNewFiles(dir, &pkg);
		wchar_t msg[768];
		_snwprintf(msg, 768,
			L"N\u00e3o foi poss\u00edvel instalar a atualiza\u00e7\u00e3o: o arquivo %S est\u00e1 em uso ou a pasta "
			L"n\u00e3o permite altera\u00e7\u00f5es.\n\nNada foi alterado. Feche outros RetroArch abertos e tente de novo, "
			L"ou baixe a vers\u00e3o light no site do WE Camp.", failed);
		msg[767] = 0;
		MessageBoxW(NULL, msg, L"Atualiza\u00e7\u00e3o", MB_OK | MB_ICONWARNING | MB_TOPMOST);
		return PACKAGE_HANDLED;
	}

	MessageBoxW(NULL,
		L"Atualiza\u00e7\u00e3o instalada!\n\nO RetroArch vai fechar e abrir de novo para usar a nova vers\u00e3o.",
		L"Atualiza\u00e7\u00e3o", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
	if (FindRetroArchWindow() != NULL && StartRelauncher(dllPath)) {
		// Kaillera's window doesn't open (kailleraSelectServerDialog checks
		// this); the relauncher closes RetroArch normally and reopens it.
		g_restart_pending = true;
	} else {
		MessageBoxW(NULL,
			L"N\u00e3o foi poss\u00edvel reiniciar o RetroArch sozinho. Feche e abra o RetroArch para usar a nova vers\u00e3o.",
			L"Atualiza\u00e7\u00e3o", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
	}
	return PACKAGE_HANDLED;
}

// rundll32 entry point (see StartRelauncher). cmdLine:
//     <pid> "<retroarch.exe path>" <original arguments>
// Closes that RetroArch, waits for it to end, then starts the exe again. If
// it doesn't close in N02_RELAUNCH_WAIT_MS, nothing is started (the player
// would end up with two).
extern "C" __declspec(dllexport) void CALLBACK kailleraUpdateRelaunchW(HWND, HINSTANCE, LPWSTR cmdLine, int) {
	if (cmdLine == NULL)
		return;
	wchar_t* p = cmdLine;
	DWORD pid = wcstoul(p, &p, 10);
	while (*p == L' ')
		p++;
	if (pid == 0 || *p != L'"')
		return;
	const wchar_t* exeStart = ++p;
	const wchar_t* exeEnd = wcschr(p, L'"');
	if (exeEnd == NULL || exeEnd - exeStart >= MAX_PATH)
		return;
	wchar_t exe[MAX_PATH];
	memcpy(exe, exeStart, (exeEnd - exeStart) * sizeof(wchar_t));
	exe[exeEnd - exeStart] = 0;
	const wchar_t* args = exeEnd + 1;
	while (*args == L' ')
		args++;

	// The close is a WM_CLOSE to RetroArch's window, its normal exit (memory
	// card and config written). The fork drops it while Kaillera is still
	// loaded ("Close Kaillera window first.", win32_check_window), and
	// Kaillera only unloads after kailleraInit() returns - so it's asked
	// again every N02_RELAUNCH_CLOSE_EVERY_MS, for N02_RELAUNCH_CLOSE_FOR_MS.
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (process != NULL) {
		DWORD start = GetTickCount();
		DWORD waited = WAIT_TIMEOUT;
		for (;;) {
			waited = WaitForSingleObject(process, N02_RELAUNCH_CLOSE_EVERY_MS);
			DWORD elapsed = GetTickCount() - start;
			if (waited != WAIT_TIMEOUT || elapsed >= N02_RELAUNCH_WAIT_MS)
				break;
			HWND ra = elapsed < N02_RELAUNCH_CLOSE_FOR_MS ? FindRetroArchWindow(pid) : NULL;
			if (ra != NULL)
				PostMessageW(ra, WM_CLOSE, 0, 0);
		}
		CloseHandle(process);
		if (waited != WAIT_OBJECT_0)
			return;
	} // else: it's already gone

	size_t cap = wcslen(exe) + wcslen(args) + 4;
	wchar_t* cmd = (wchar_t*)malloc(cap * sizeof(wchar_t));
	if (cmd == NULL)
		return;
	_snwprintf(cmd, cap, L"\"%s\"%s%s", exe, args[0] ? L" " : L"", args);
	cmd[cap - 1] = 0;
	STARTUPINFOW si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
	free(cmd);
}

#endif // _WIN64

///////////////////////////////////////////////////////////////////////////////
// Public API
///////////////////////////////////////////////////////////////////////////////

bool n02_update_restart_pending() {
#ifdef _WIN64
	return g_restart_pending;
#else
	return false;
#endif
}

void n02_update_check_and_prompt() {
	char selfPath[MAX_PATH];
	wchar_t selfPathW[MAX_PATH];
	HMODULE hSelf = NULL;
	GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)&n02_update_check_and_prompt, &hSelf);
	if (hSelf == NULL || GetModuleFileNameA(hSelf, selfPath, sizeof(selfPath)) == 0
		|| GetModuleFileNameW(hSelf, selfPathW, MAX_PATH) == 0)
		return; // can't locate our own file on disk - skip silently

	// Finish off anything staged by a run that couldn't complete the swap
	// immediately, before possibly staging another one below.
	SwapInPendingUpdate(selfPath);

	if (strcmp(N02_VERSION, "dev") == 0)
		return; // local/dev build - not tagged, nothing to compare against

#ifdef _WIN64
	if (CheckPackageUpdate(selfPathW) == PACKAGE_HANDLED)
		return;
#endif

	// DLL-only update: the community server's plain-HTTP listener, or the
	// n02.ini test server.
	char host[128];
	int port = N02_STREAM_DEFAULT_PORT;
	_snprintf(host, sizeof(host), "%s", N02_STREAM_DEFAULT_HOST);
	host[sizeof(host) - 1] = 0;
	wchar_t selfDir[MAX_PATH];
	UpdateServer testServer;
	DirectoryOf(selfPathW, selfDir, MAX_PATH);
	if (ReadServerOverride(selfDir, &testServer)) {
		WideCharToMultiByte(CP_UTF8, 0, testServer.host, -1, host, sizeof(host), NULL, NULL);
		port = testServer.port;
	}

	char path[128];
	_snprintf(path, sizeof(path), "/updates/latest?arch=%s", N02_UPDATE_ARCH);

	char resp[512];
	int n = HttpGetSmall(host, port, path, resp, sizeof(resp) - 1);
	if (n <= 0)
		return; // no update published yet, or a network hiccup - stay quiet either way
	resp[n] = 0;

	char version[64];
	int size = 0;
	char crcHex[16];
	if (sscanf(resp, "%63[^\t]\t%d\t%15s", version, &size, crcHex) != 3)
		return;
	if (size <= 0 || size > N02_UPDATE_MAX_SIZE)
		return;
	if (strcmp(version, N02_VERSION) == 0)
		return; // already current

	// Only offer versions strictly newer than this one - a tester build
	// handed out ahead of its release (or any CI build of commits past the
	// last published tag) must never be prompted to "update" back down to
	// what the site still has. If either string doesn't parse, fall back to
	// the plain any-difference check above.
	if (CompareVersionStrings(version, N02_VERSION) <= 0)
		return;

	char msg[512];
	wsprintf(msg,
		"Uma nova versao do Kaillera esta disponivel:\n\n  Atual: %s\n  Nova:  %s\n\nDeseja baixar e atualizar agora?",
		N02_VERSION, version);
	if (MessageBox(NULL, msg, "Atualizacao disponivel", MB_YESNO | MB_ICONQUESTION) != IDYES)
		return;

	char* buf = (char*)malloc(size);
	if (buf == NULL)
		return;

	_snprintf(path, sizeof(path), "/updates/download/%s", N02_UPDATE_ARCH);
	int got = HttpGetToBuffer(host, port, path, buf, size);

	unsigned long expectedCrc = strtoul(crcHex, NULL, 16);
	bool ok = (got == size) && (Crc32((unsigned char*)buf, size) == expectedCrc);
	if (!ok) {
		free(buf);
		MessageBox(NULL,
			"Falha ao baixar a atualizacao (dados incompletos ou corrompidos). Tente novamente mais tarde.",
			"Atualizacao", MB_OK | MB_ICONWARNING);
		return;
	}

	char newPath[MAX_PATH];
	_snprintf(newPath, sizeof(newPath), "%s.new", selfPath);
	newPath[sizeof(newPath) - 1] = 0;

	HANDLE f = CreateFileA(newPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		free(buf);
		return;
	}
	DWORD written = 0;
	WriteFile(f, buf, size, &written, NULL);
	CloseHandle(f);
	free(buf);
	if ((int)written != size) {
		DeleteFileA(newPath);
		return;
	}

	if (SwapInPendingUpdate(selfPath)) {
		MessageBox(NULL,
			"Atualizacao instalada! Feche e abra o emulador novamente para usar a nova versao.",
			"Atualizacao", MB_OK | MB_ICONINFORMATION);
	} else {
		MessageBox(NULL,
			"Atualizacao baixada, mas nao foi possivel instalar agora (arquivo em uso). Feche e abra o emulador novamente para concluir.",
			"Atualizacao", MB_OK | MB_ICONINFORMATION);
	}
}
