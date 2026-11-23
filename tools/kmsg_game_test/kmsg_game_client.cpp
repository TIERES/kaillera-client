// Plays a one-player Kaillera game against a real EmuLinker-K and throws away bursts of the
// server's packets mid-game, to check what kcore/k_message.h does with the hole that leaves.
//
// The transport is the DLL's own k_message.h / k_instruction; the game loop copies
// kaillera_modify_play_values() (send our input, wait for the server's data, resend_message(5)
// every >10 ms while waiting, the same drop rules). Every input carries a frame counter, and in a
// one-player game the server sends back exactly our own input - so the counters received must be
// 0, 1, 2, ... with no gap. A gap = a skipped GAMEDATA/GAMCDATA = what would be a desync.
//
// Usage: kmsg_game_client <port> <hold|skip> <pong_delay_ms> <frames> <frame:packets,...>
//   hold = k_message::hold_holes on during the game (the fix); skip = the old 250 ms skip.
//   pong_delay_ms raises the ping the server measures at login, and so the game's frame delay
//   (the number of frames in flight - more than 4 are needed for a hole the bundles can't cover).
//   frame:packets = at that frame, the next <packets> datagrams from the server are dropped.
// Exit codes: 0 = every frame arrived in order, 1 = gap/wrong data, 2 = left the game (lost data),
//             3 = setup failed.
//
// Build (x64 Native Tools prompt, from this folder):
//   cl /nologo /EHsc kmsg_game_client.cpp ..\..\common\k_socket.cpp ws2_32.lib
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kcore/k_message.h"

int PACKETLOSSCOUNT, PACKETMISOTDERCOUNT, PACKETINCDSCCOUNT, PACKETIADSCCOUNT;
int SOCK_RECV_PACKETS, SOCK_RECV_BYTES, SOCK_RECV_RETR, SOCK_SEND_PACKETS, SOCK_SEND_BYTES, SOCK_SEND_RETR;
void __cdecl kprintf(char*, ...) {}

static bool g_live = false;
static bool g_hold = false;
static bool hold_holes() { return g_live && g_hold; }

// Allocated in main(): a static k_message would register itself in k_socket's static socket
// list before that list is constructed (static init order), and never see its data.
static k_message* connp;
#define conn (*connp)
static int drop_budget = 0;
static int dropped_total = 0;

// Network loss: datagrams that arrive while drop_budget > 0 never reach k_message.
static void eat_lost_packets() {
	while (drop_budget > 0 && k_socket::check_sockets(0, 0) && conn.has_data_waiting) {
		char buf[0x5E20];
		int len = sizeof(buf);
		sockaddr_in a;
		if (!conn.k_socket::check_recv(buf, &len, false, &a))
			break;
		drop_budget--;
		dropped_total++;
	}
}

#define DATALEN 12
static oslist<char*, 256> cache;     // the client's copy of the server's game data cache
static oslist<char*, 256> queue;    // frames received, oldest first

static void on_instruction(k_instruction& ki) {
	if (getenv("KMSG_TRACE_MSG") && (ki.type == GAMEDATA || ki.type == GAMCDATA))
		printf("  msg serial %u type %d value %u (buffer_pos %u)\n", (unsigned)conn.last_processed_instruction,
			(int)ki.type, ki.type == GAMEDATA ? *(unsigned*)(ki.buffer + 2) : (unsigned char)ki.buffer[0],
			ki.buffer_pos);
	if (ki.type == GAMEDATA) {
		short len = ki.load_short();
		char* kd = (char*)malloc(len);
		ki.load_bytes(kd, len);
		if (cache.length == 256) {
			free(cache[0]);
			cache.removei(0);
		}
		cache.add(kd);
		char* q = (char*)malloc(DATALEN);
		memcpy(q, kd, DATALEN);
		queue.add(q);
	} else if (ki.type == GAMCDATA) {
		int index = (unsigned char)ki.load_char();
		char* q = (char*)malloc(DATALEN);
		if (index < cache.length)
			memcpy(q, cache[index], DATALEN);
		else
			memset(q, 0xEE, DATALEN); // the real client would read garbage here
		queue.add(q);
	} else if (ki.type == SERVPING) {
		k_instruction pong;
		pong.type = USERPONG;
		for (int x = 0; x < 4; x++)
			pong.store_int(x);
		conn.send_instruction(&pong);
	}
}

static bool pump(int ms) {
	k_socket::check_sockets(0, ms);
	eat_lost_packets();
	bool any = false;
	while (conn.has_data()) {
		eat_lost_packets();
		k_instruction ki;
		sockaddr_in a;
		if (!conn.receive_instruction(&ki, false, &a))
			break;
		on_instruction(ki);
		any = true;
	}
	return any;
}

// Waits for an instruction of `type` (handling pings meanwhile). Pongs are delayed to raise the
// ping the server measures.
static bool wait_for(INSTRUCTION type, int pong_delay, k_instruction* out, int timeout_ms = 5000) {
	DWORD t0 = GetTickCount();
	while (GetTickCount() - t0 < (DWORD)timeout_ms) {
		k_socket::check_sockets(0, 50);
		while (conn.has_data()) {
			k_instruction ki;
			sockaddr_in a;
			if (!conn.receive_instruction(&ki, false, &a))
				break;
			if (getenv("KMSG_VERBOSE"))
				printf("  <- instruction %d\n", (int)ki.type);
			if (ki.type == SERVPING) {
				if (pong_delay)
					Sleep(pong_delay);
				k_instruction pong;
				pong.type = USERPONG;
				for (int x = 0; x < 4; x++)
					pong.store_int(x);
				conn.send_instruction(&pong);
				continue;
			}
			if (ki.type == type) {
				if (out)
					out->clone(&ki);
				return true;
			}
		}
	}
	return false;
}

int main(int argc, char** argv) {
	if (argc < 6) {
		printf("usage: kmsg_game_client <port> <hold|skip> <pong_delay_ms> <frames> <frame:packets,...>\n");
		return 3;
	}
	int port = atoi(argv[1]);
	g_hold = strcmp(argv[2], "hold") == 0;
	int pong_delay = atoi(argv[3]);
	int frames = atoi(argv[4]);
	int loss_frame[16], loss_packets[16], nloss = 0;
	for (char* p = argv[5]; *p && nloss < 16;) {
		loss_frame[nloss] = atoi(p);
		char* c = strchr(p, ':');
		if (!c)
			break;
		loss_packets[nloss++] = atoi(c + 1);
		char* n = strchr(c, ',');
		if (!n)
			break;
		p = n + 1;
	}

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

	connp = new k_message;
	conn.initialize(0);
	conn.set_addr(&addr);
	conn.hold_holes = hold_holes;
	char user[32];
	sprintf(user, "gt_%s_%u", argv[2], (unsigned)(GetTickCount() % 100000));
	k_instruction login;
	login.type = USERLOGN;
	login.store_string("kmsg_game_test");
	login.store_char(1);
	login.set_username(user);
	conn.send_instruction(&login);
	if (!wait_for(LONGSUCC, pong_delay, NULL)) {
		printf("login failed\n");
		return 3;
	}

	k_instruction mk;
	mk.type = GAMEMAKE;
	mk.store_string("kmsg_game_test game");
	mk.store_char(0);
	mk.store_int(-1);
	conn.send_instruction(&mk);
	if (!wait_for(GAMRSLST, 0, NULL)) {
		printf("game not created\n");
		return 3;
	}

	k_instruction begin;
	begin.type = GAMEBEGN;
	begin.store_int(-1);
	conn.send_instruction(&begin);
	k_instruction gb;
	if (!wait_for(GAMEBEGN, 0, &gb)) {
		printf("no GAMEBEGN\n");
		return 3;
	}
	int throughput = gb.load_short();
	int dframeno = (throughput + 1) * 1 - 1;

	k_instruction rdy;
	rdy.type = GAMRSRDY;
	conn.send_instruction(&rdy);
	if (!wait_for(GAMRSRDY, 0, NULL)) {
		printf("no GAMRSRDY\n");
		return 3;
	}
	g_live = true;
	printf("mode=%s frame delay=%d (in flight %d)\n", argv[2], throughput, dframeno + 1);

	int frameno = 0, received = 0, bad = 0, next_loss = 0, padding = 0;
	DWORD start = GetTickCount();
	for (int f = 0; f < frames; f++) {
		if (next_loss < nloss && f == loss_frame[next_loss]) {
			drop_budget += loss_packets[next_loss];
			printf("frame %d: dropping the next %d packets from the server\n", f, loss_packets[next_loss]);
			next_loss++;
		}
		// 60 fps, like an emulator.
		DWORD due = start + (DWORD)((f * 1000ULL) / 60);
		while ((int)(due - GetTickCount()) > 0)
			pump(1);
		// Our input for this frame: the frame counter, a value that repeats every 300 frames (so the
		// server's cache answers with GAMCDATA as well), and a marker.
		unsigned int in[3] = { (unsigned)f, (unsigned)(f % 300), 0xABCD1234u };
		k_instruction kx;
		kx.type = GAMEDATA;
		kx.store_short(DATALEN);
		kx.store_bytes(in, DATALEN);
		conn.send_instruction(&kx);

		int pix = 0;
		DWORD ttx = 0;
		bool delay_frame = false;
		for (;;) {
			pump(pix);
			if (frameno < dframeno) {
				frameno++;
				delay_frame = true; // return 0: one of the input-delay frames
				break;
			}
			// kaillera_modify_play_values(): game data lost for good -> leave the game.
			if (conn.lost_in_game || (queue.length <= 0 && conn.head_hole_ms() > KMSG_GAME_HOLE_DROP_MS)) {
				printf("frame %d: LEFT THE GAME - %s (holes held %u, skipped %u, resend requests %u, dropped %d packets)\n", f,
					conn.lost_in_game ? "gap too big to wait for" : "hole not filled in time",
					conn.holes_held, conn.holes_skipped, conn.resend_requests, dropped_total);
				return 2;
			}
			if (queue.length > 0)
				break;
			if (ttx == 0) {
				pix++;
				ttx = GetTickCount();
			} else if (GetTickCount() - ttx > 10) {
				ttx = GetTickCount();
				conn.resend_message(5);
				pix++;
				if (pix == 500) {
					printf("frame %d: Lost Connection\n", f);
					return 3;
				}
			}
		}
		if (delay_frame)
			continue;
		unsigned int out[3];
		memcpy(out, queue[0], DATALEN);
		free(queue[0]);
		queue.removei(0);
		frameno++;
		if (getenv("KMSG_TRACE") && f < atoi(getenv("KMSG_TRACE")))
			printf("  f=%d got %u (q=%d)\n", f, out[0], queue.length);
		if (f < 120) {
			// The first 2 s are not checked: EmuLinker sends zero frames at the start (the delay
			// compensation padding) and drops some of the first inputs on its own - the same for
			// every player and with no packet lost (seen with an independent Python client too).
			padding++;
			received = out[2] == 0xABCD1234u ? (int)out[0] + 1 : 0;
			continue;
		}
		if (out[0] != (unsigned)received || out[1] != (unsigned)(received % 300) || out[2] != 0xABCD1234u) {
			if (bad < 5)
				printf("frame %d: expected input %d, got %u/%u/%08X\n", f, received, out[0], out[1], out[2]);
			bad++;
			received = out[0] + 1;
		} else
			received++;
	}
	printf("%d frames in %u ms, %d start padding, %d inputs back in order, %d wrong, holes held %u, skipped %u, resend requests %u, dropped %d packets\n",
		frames, GetTickCount() - start, padding, received, bad, conn.holes_held, conn.holes_skipped, conn.resend_requests, dropped_total);

	k_instruction drop;
	drop.type = GAMRDROP;
	drop.store_char(0);
	drop.store_char(0);
	conn.send_instruction(&drop);
	k_instruction leave;
	leave.type = USERLEAV;
	leave.store_short(-1);
	leave.store_string("bye");
	conn.send_instruction(&leave);
	Sleep(100);
	return bad ? 1 : 0;
}
