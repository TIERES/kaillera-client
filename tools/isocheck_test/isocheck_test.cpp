// Tests of common/n02_isocheck.cpp (the room's ISO check). build.cmd builds
// it; run isocheck_test.exe - exit code 0 = all passed. The popup texts go
// to popups.txt (UTF-8) to be read; "isocheck_test.exe popup" also shows them.
#include "../../common/n02_isocheck.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FALHOU linha %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static FILE* popups;
static bool show;

static void Dump(const char* title, const n02_isoverdict* v, const char* me, const char* game, bool host) {
	wchar_t w[3000];
	char u[9000];
	n02_iso_popup_text(v, me, game, host, w, 3000);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL);
	fprintf(popups, "===== %s (me=%s, host=%d)\n%s\n\n", title, me, host, u);
	if (show)
		MessageBoxW(NULL, w, N02_ISO_POPUP_TITLE, (host ? MB_YESNO | MB_DEFBUTTON2 : MB_OK) | MB_ICONWARNING);
}

static const char* GAME = "PCSX ReARMed: Euro vs Copa America 2004.bin";

int main(int argc, char** argv) {
	show = argc > 1 && strcmp(argv[1], "popup") == 0;
	popups = fopen("popups.txt", "wb");
	n02_isoids t;
	n02_isoverdict v;
	char line[2048], sig[1024];

	// Ids
	CHECK(n02_isoid_valid("73E91CFE:1C473F60"));
	CHECK(n02_isoid_valid("4B0B3C85:F8DF64D"));
	CHECK(n02_isoid_valid("ba9bde21:1c473f60"));
	CHECK(!n02_isoid_valid("73E91CFE"));
	CHECK(!n02_isoid_valid("73E91CF:1C473F60"));
	CHECK(!n02_isoid_valid("73E91CFE:"));
	CHECK(!n02_isoid_valid("73E91CFE:1C473F60 x"));
	CHECK(!n02_isoid_valid("?"));
	CHECK(!n02_isoid_valid(""));
	CHECK(!n02_isoid_valid(N02_ISO_WARN));

	// Table
	n02_isoids_clear(&t);
	CHECK(n02_isoids_set(&t, "luiz", "BA9BDE21:1C473F60"));
	CHECK(!n02_isoids_set(&t, "LUIZ", "ba9bde21:1c473f60")); // same nick and id, other case
	CHECK(n02_isoids_set(&t, "luiz", "73E91CFE:1C473F60"));  // changed (downloaded again)
	CHECK(strcmp(n02_isoids_get(&t, "Luiz"), "73E91CFE:1C473F60") == 0);
	n02_isoids_remove(&t, "luiz");
	CHECK(n02_isoids_get(&t, "luiz") == NULL);

	// 2 players, same ISO: nothing
	{
		const char names[][32] = { "warleyvieira", "luiz" };
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "warleyvieira", "BA9BDE21:1C473F60");
		n02_isoids_set(&t, "luiz", "BA9BDE21:1C473F60");
		CHECK(!n02_iso_verdict(&t, names, 2, &v));
		CHECK(v.groups == 1);
		n02_iso_signature(&v, sig, sizeof(sig));
		CHECK(sig[0] == 0);
		n02_iso_chat_line(&v, line, sizeof(line));
		CHECK(line[0] == 0);
	}

	// 2 players, different (the 2026-11-16 case): nobody can tell which is right
	{
		const char names[][32] = { "warleyvieira", "luiz" };
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "warleyvieira", "73E91CFE:1C473F60");
		n02_isoids_set(&t, "luiz", "BA9BDE21:1C473F60");
		CHECK(n02_iso_verdict(&t, names, 2, &v));
		CHECK(v.groups == 2 && v.majority == -1 && v.unknown == 0);
		CHECK(strcmp(v.nicks[0], "warleyvieira") == 0 && strcmp(v.nicks[1], "luiz") == 0);
		n02_iso_chat_line(&v, line, sizeof(line));
		printf("2 diferentes: %s\n", line);
		CHECK(strstr(line, "as ISOs de warleyvieira e de luiz sao diferentes") != NULL);
		n02_iso_signature(&v, sig, sizeof(sig));
		CHECK(strcmp(sig, "73E91CFE:1C473F60=warleyvieira;BA9BDE21:1C473F60=luiz;") == 0);
		Dump("2 jogadores, ISOs diferentes", &v, "warleyvieira", GAME, true);
		Dump("2 jogadores, ISOs diferentes", &v, "luiz", GAME, false);
	}

	// 3 players, one different: named
	{
		const char names[][32] = { "tassio", "warleyvieira", "luiz" };
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "tassio", "BA9BDE21:1C473F60");
		n02_isoids_set(&t, "warleyvieira", "73E91CFE:1C473F60");
		n02_isoids_set(&t, "luiz", "BA9BDE21:1C473F60");
		CHECK(n02_iso_verdict(&t, names, 3, &v));
		CHECK(v.groups == 2 && v.majority == 0 && v.odd_count == 1);
		CHECK(strcmp(v.odd, "warleyvieira") == 0);
		CHECK(strcmp(v.nicks[0], "tassio e luiz") == 0);
		n02_iso_chat_line(&v, line, sizeof(line));
		printf("3, 1 diferente: %s\n", line);
		CHECK(strstr(line, "a ISO de warleyvieira e diferente da dos outros") != NULL);
		Dump("3 jogadores, 1 diferente", &v, "tassio", GAME, true);
		Dump("3 jogadores, 1 diferente", &v, "warleyvieira", GAME, false);
		Dump("3 jogadores, 1 diferente", &v, "luiz", GAME, false);
	}

	// 4 players, two different ones, each with its own ISO: both named
	{
		const char names[][32] = { "a1", "b2", "c3", "d4", "e5" };
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "a1", "11111111:10");
		n02_isoids_set(&t, "b2", "22222222:10");
		n02_isoids_set(&t, "c3", "11111111:10");
		n02_isoids_set(&t, "d4", "33333333:10");
		CHECK(n02_iso_verdict(&t, names, 5, &v)); // e5 never announced
		CHECK(v.groups == 3 && v.majority == 0 && v.odd_count == 2 && v.unknown == 1);
		CHECK(strcmp(v.odd, "b2 e d4") == 0);
		n02_iso_chat_line(&v, line, sizeof(line));
		printf("4, 2 diferentes: %s\n", line);
		CHECK(strstr(line, "as ISOs de b2 e d4 sao diferentes") != NULL);
		Dump("4 jogadores (+1 sem id), 2 diferentes", &v, "d4", GAME, false);
		Dump("4 jogadores (+1 sem id), 2 diferentes", &v, "c3", GAME, false);
	}

	// 4 players, 2 x 2: a tie
	{
		const char names[][32] = { "a1", "b2", "c3", "d4" };
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "a1", "11111111:10");
		n02_isoids_set(&t, "b2", "22222222:10");
		n02_isoids_set(&t, "c3", "22222222:10");
		n02_isoids_set(&t, "d4", "11111111:10");
		CHECK(n02_iso_verdict(&t, names, 4, &v));
		CHECK(v.groups == 2 && v.majority == -1);
		n02_iso_chat_line(&v, line, sizeof(line));
		printf("2 x 2: %s\n", line);
		CHECK(strstr(line, "a sala tem ISOs diferentes (a1 e d4 | b2 e c3)") != NULL);
		Dump("4 jogadores, 2 x 2", &v, "a1", GAME, true);
	}

	// Only one player announced (the other with an old DLL): not compared
	{
		const char names[][32] = { "warleyvieira", "luiz" };
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "warleyvieira", "73E91CFE:1C473F60");
		CHECK(!n02_iso_verdict(&t, names, 2, &v));
		CHECK(v.groups == 1 && v.unknown == 1);
	}

	// Somebody who left the room isn't in the list any more: not counted
	{
		const char names[][32] = { "warleyvieira" };
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "warleyvieira", "73E91CFE:1C473F60");
		n02_isoids_set(&t, "luiz", "BA9BDE21:1C473F60");
		CHECK(!n02_iso_verdict(&t, names, 1, &v));
	}

	// Accents: the popup has them (UTF-8 literal -> UTF-16)
	{
		const char names[][32] = { "warleyvieira", "luiz" };
		wchar_t w[3000];
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "warleyvieira", "73E91CFE:1C473F60");
		n02_isoids_set(&t, "luiz", "BA9BDE21:1C473F60");
		n02_iso_verdict(&t, names, 2, &v);
		n02_iso_popup_text(&v, "luiz", GAME, true, w, 3000);
		CHECK(wcsstr(w, L"s\x00e3o diferentes") != NULL);  // "são"
		CHECK(wcsstr(w, L"C\x00f3" L"digo da ISO") != NULL); // "Código"
		CHECK(wcsstr(w, L"Jogo: Euro vs Copa America 2004.bin") != NULL);
		CHECK(wcsstr(w, L"Iniciar a partida mesmo assim?") != NULL);
		n02_iso_popup_text(&v, "luiz", GAME, false, w, 3000);
		CHECK(wcsstr(w, L"mesmo assim") == NULL);
		// A nick in the ANSI code page (not UTF-8) still comes out right.
		const char names2[][32] = { "Jo\xe3o", "luiz" }; // "João" in cp1252
		n02_isoids_clear(&t);
		n02_isoids_set(&t, "Jo\xe3o", "73E91CFE:1C473F60");
		n02_isoids_set(&t, "luiz", "BA9BDE21:1C473F60");
		n02_iso_verdict(&t, names2, 2, &v);
		n02_iso_popup_text(&v, "luiz", GAME, false, w, 3000);
		CHECK(wcsstr(w, L"Jo\x00e3o: 73E91CFE:1C473F60") != NULL);
	}

	fclose(popups);
	printf(failures ? "\n%d FALHA(S)\n" : "\nTodos os testes passaram.\n", failures);
	return failures ? 1 : 0;
}
