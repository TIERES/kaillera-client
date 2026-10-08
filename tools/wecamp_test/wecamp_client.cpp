// Exercises common/n02_wecamp.cpp (the DLL's WE Camp / Memory Card online
// client) against a local wg-camp - see README.md. Two "players" log in,
// check out the same match, both send back the same saved card, and the
// server must keep it as a new version; then a third checkout must return it.
#include "../../common/n02_wecamp.h"
#include "../../common/nSettings.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

HINSTANCE hx = NULL; // nSettings: n02.ini next to this .exe

static int failures = 0;
#define CHECK(cond, what) do { if (cond) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); failures++; } } while (0)

int main(int argc, char** argv) {
	if (argc < 4) {
		printf("uso: wecamp_client <usuario1> <usuario2> <senha>\n");
		return 1;
	}
	const char* p1 = argv[1];
	const char* p2 = argv[2];
	const char* pass = argv[3];
	char err[300];
	char players[100];
	_snprintf(players, sizeof(players), "%s,%s", p1, p2);
	const char* content = "1a2b3c4d:2a3b4c00";
	// local_wgcamp.py registers <user in lowercase>@example.com.
	char mail1[96], mail2[96];
	_snprintf(mail1, sizeof(mail1), "%s@example.com", p1);
	_snprintf(mail2, sizeof(mail2), "%s@example.com", p2);
	_strlwr(mail1);
	_strlwr(mail2);

	nSettings::Initialize((char*)"SC");
	n02_wecamp_load();
	n02_wecamp_logout();
	CHECK(!n02_wecamp_logged_in(), "comeca desconectado");

	CHECK(!n02_wecamp_login(mail1, "senha-errada", err, sizeof(err)), "senha errada e recusada");
	printf("     -> %s\n", err);

	// Player 2 first (its token is used for the second report).
	CHECK(n02_wecamp_login(mail2, pass, err, sizeof(err)), "login do jogador 2");
	char token2[128];
	nSettings::get_str((char*)"WECAMP_TOKEN", token2, (char*)"");
	n02_wecamp_checkout_result co2;
	CHECK(n02_wecamp_checkout(content, "WE2002.bin", players, &co2, err, sizeof(err)), "checkout do jogador 2");

	CHECK(n02_wecamp_login(mail1, pass, err, sizeof(err)), "login do jogador 1");
	CHECK(strcmp(n02_wecamp_username(), p1) == 0, "nome de usuario devolvido pelo servidor");
	CHECK(strcmp(n02_wecamp_email(), mail1) == 0, "e-mail da conta devolvido pelo servidor");
	CHECK(!n02_wecamp_login(p1, pass, err, sizeof(err)), "login pelo nome (sem e-mail) e recusado no DLL");
	printf("     -> %s\n", err);
	n02_wecamp_load();
	CHECK(n02_wecamp_logged_in(), "token salvo no n02.ini e recarregado");

	n02_wecamp_checkout_result co;
	CHECK(n02_wecamp_checkout(content, "WE2002.bin", players, &co, err, sizeof(err)), "checkout do jogador 1");
	CHECK(co.slots == 2 && _stricmp(co.player[0], p1) == 0 && _stricmp(co.player[1], p2) == 0, "slot 1 = 1P, slot 2 = 2P");
	CHECK(strcmp(co.sha256[0], co2.sha256[0]) == 0 && strcmp(co.sha256[1], co2.sha256[1]) == 0, "os dois jogadores recebem os mesmos cartoes");
	printf("     -> slot1 %s v%d %.16s...\n     -> slot2 %s v%d %.16s...\n",
		co.player[0], co.version[0], co.sha256[0], co.player[1], co.version[1], co.sha256[1]);

	static char card[N02_MCD_SIZE + 1];
	CHECK(n02_wecamp_get_card(co.sha256[0], card, false), "download do cartao com o token");
	CHECK(card[0] == 'M' && card[1] == 'C', "cartao formatado (cabecalho MC)");
	static char card2[N02_MCD_SIZE + 1];
	CHECK(n02_wecamp_get_card(co.sha256[0], card2, true), "download do cartao com a chave do DLL (espectador)");
	CHECK(memcmp(card, card2, N02_MCD_SIZE) == 0, "mesmo conteudo nos dois downloads");

	n02_wecamp_checkout_result missing;
	CHECK(!n02_wecamp_checkout(content, "WE2002.bin", "SemConta123,Outro456", &missing, err, sizeof(err)), "checkout sem estar na lista e recusado");
	printf("     -> %s\n", err);
	char withMissing[100];
	_snprintf(withMissing, sizeof(withMissing), "%s,SemConta123", p1);
	CHECK(!n02_wecamp_checkout(content, "WE2002.bin", withMissing, &missing, err, sizeof(err)), "jogador sem conta e recusado");
	printf("     -> %s\n", err);

	// "Save" something on slot 1's card and report it from both players.
	card[8192] = 0x5A;
	card[8193] = (char)GetTickCount();
	char status[64];
	int version = 0;
	// The host (player 1) sends right away and that already saves it.
	n02_wecamp_commit(content, co.player[0], co.sha256[0], players, card, status, sizeof(status), &version);
	CHECK(strcmp(status, "committed") == 0, "envio do host grava a nova versao");
	printf("     -> %s v%d\n", status, version);

	// Player 2's later send of the same card changes nothing.
	nSettings::set_str((char*)"WECAMP_TOKEN", token2);
	nSettings::set_str((char*)"WECAMP_USER", (char*)p2);
	n02_wecamp_load();
	n02_wecamp_commit(content, co.player[0], co.sha256[0], players, card, status, sizeof(status), &version);
	CHECK(strcmp(status, "already") == 0, "envio igual do outro jogador nao muda nada");
	printf("     -> %s v%d\n", status, version);

	// A different card from the same starting point (a desynced PC) is refused.
	static char other[N02_MCD_SIZE + 1];
	memcpy(other, card, N02_MCD_SIZE);
	other[9000] ^= 0x11;
	n02_wecamp_commit(content, co.player[0], co.sha256[0], players, other, status, sizeof(status), &version);
	CHECK(strcmp(status, "conflict") == 0, "cartao diferente do salvo vira conflito");

	n02_wecamp_checkout_result after;
	CHECK(n02_wecamp_checkout(content, "WE2002.bin", players, &after, err, sizeof(err)), "checkout depois da partida");
	CHECK(after.version[0] == co.version[0] + 1, "slot 1 na versao nova");
	static char back[N02_MCD_SIZE + 1];
	CHECK(n02_wecamp_get_card(after.sha256[0], back, false) && memcmp(back, card, N02_MCD_SIZE) == 0, "cartao salvo volta igual");

	// "So logados" rooms: player 2 (logged in now) shows a ticket for room
	// 42; the host (player 1) checks it.
	char ticket[64], account[32];
	CHECK(n02_wecamp_get_ticket("42", ticket, sizeof(ticket), err, sizeof(err)), "jogador 2 pega ingresso da sala");
	CHECK(strlen(ticket) > 10 && strlen(ticket) < 40, "ingresso curto (cabe no chat)");
	char token1[128];
	nSettings::set_str((char*)"WECAMP_TOKEN", (char*)"");
	CHECK(n02_wecamp_login(mail1, pass, err, sizeof(err)), "host logado");
	CHECK(!n02_wecamp_verify_ticket(ticket, "43", account, sizeof(account), err, sizeof(err)), "ingresso de outra sala e recusado");
	printf("     -> %s\n", err);
	CHECK(n02_wecamp_verify_ticket(ticket, "42", account, sizeof(account), err, sizeof(err)), "host confere o ingresso");
	CHECK(_stricmp(account, p2) == 0, "ingresso identifica a conta do jogador 2");
	CHECK(!n02_wecamp_verify_ticket(ticket, "42", account, sizeof(account), err, sizeof(err)), "ingresso nao pode ser reusado");
	printf("     -> %s\n", err);
	CHECK(!n02_wecamp_verify_ticket("ingresso-falso", "42", account, sizeof(account), err, sizeof(err)), "ingresso falso e recusado");
	(void)token1;

	n02_wecamp_logout();
	CHECK(!n02_wecamp_logged_in(), "logout limpa o token");

	printf(failures ? "\n%d FALHA(S)\n" : "\nTUDO OK\n", failures);
	return failures ? 1 : 0;
}
