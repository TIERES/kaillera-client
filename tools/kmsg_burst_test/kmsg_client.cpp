// Drives the real kcore/k_message.h through a Kaillera login against fake_elk.py, the same way
// kaillera_core.cpp's kaillera_step() does ("while (has_data()) receive_instruction(...)"), and
// reports whether the login burst got through. Exit codes: 0 = own UserJoined delivered,
// 2 = the receive loop spun without returning (the old hole bug), 3 = timed out.
//
// Build (x64 Native Tools prompt, from this folder):
//   cl /nologo /EHsc /I..\..\common kmsg_client.cpp ..\..\common\k_socket.cpp ws2_32.lib
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kcore/k_message.h"

int PACKETLOSSCOUNT, PACKETMISOTDERCOUNT, PACKETINCDSCCOUNT, PACKETIADSCCOUNT;
int SOCK_RECV_PACKETS, SOCK_RECV_BYTES, SOCK_RECV_RETR, SOCK_SEND_PACKETS, SOCK_SEND_BYTES, SOCK_SEND_RETR;
void __cdecl kprintf(char*, ...) {}

int main(int argc, char** argv) {
	int port = argc > 1 ? atoi(argv[1]) : 27999;
	char username[] = "burst_test";
	k_socket::Initialize();

	k_socket hello;
	hello.initialize(0, 2048);
	hello.set_address("127.0.0.1", (u_short)port);
	hello.send((char*)"HELLO0.83", 10);
	DWORD t0 = GetTickCount();
	while ((!k_socket::check_sockets(0, 100) || !hello.has_data()) && GetTickCount() - t0 < 3000);
	char rsp[257];
	int rl = 256;
	sockaddr_in addr;
	if (!hello.check_recv(rsp, &rl, false, &addr) || strncmp(rsp, "HELLOD00D", 9) != 0) {
		printf("no HELLOD00D\n");
		return 3;
	}
	addr.sin_port = htons((u_short)atoi(rsp + 9));

	k_message conn;
	conn.initialize(0);
	conn.set_addr(&addr);
	k_instruction login;
	login.type = USERLOGN;
	login.store_string("kmsg_client");
	login.store_char(1);
	login.set_username(username);
	conn.send_instruction(&login);

	bool longsucc = false;
	t0 = GetTickCount();
	while (GetTickCount() - t0 < 4000) {
		k_socket::check_sockets(0, 200);
		long spins = 0;
		while (conn.has_data()) {
			if (++spins > 2000000) {
				printf("SPIN: receive loop never returned (queue stuck behind a hole)\n");
				return 2;
			}
			k_instruction ki;
			sockaddr_in saddr;
			if (!conn.receive_instruction(&ki, false, &saddr))
				continue;
			printf("  +%4lums  serial %u  type %d  user '%s'\n", GetTickCount() - t0,
				conn.last_processed_instruction, (int)ki.type, ki.user);
			if (ki.type == SERVPING) {
				k_instruction pong;
				pong.type = USERPONG;
				for (int x = 0; x < 4; x++)
					pong.store_int(x);
				conn.send_instruction(&pong);
			} else if (ki.type == LONGSUCC) {
				longsucc = true;
			} else if (ki.type == USERJOIN && strcmp(ki.user, username) == 0) {
#ifdef KMSG_HOLE_SKIP_MS
				unsigned int skipped = conn.holes_skipped;
#else
				unsigned int skipped = 0; // pre-fix k_message.h has no hole skipping
#endif
				printf("OK: own UserJoined after %lums, ServerStatus %s, holes skipped %u\n",
					GetTickCount() - t0, longsucc ? "received" : "LOST", skipped);
				return 0;
			}
		}
	}
	printf("TIMEOUT: own UserJoined never delivered\n");
	return 3;
}
