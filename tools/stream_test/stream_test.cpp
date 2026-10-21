// Roda o common/n02_stream.cpp de verdade como um host com "Stream ao vivo!":
// ~40 s de partida a 60 fps (2 jogadores, 24 bytes por frame, conteúdo
// diferente em cada frame) mais uma linha de chat a cada 300 frames, contra o
// fault_proxy.py. Grava em esperado.bin os registros na ordem em que foram
// entregues ao stream - o que o servidor tem que ter no fim, byte a byte.
//
//   stream_test.exe [porta] [frames] [host]     (padrão: 5070, 2400, 127.0.0.1)
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/n02_stream.h"

// stats.h - o DLL escreve isso na janela /stats; aqui vai para o console.
void StatsAppendLine(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	printf("[%6.1fs] ", GetTickCount() / 1000.0);
	vprintf(fmt, args);
	printf("\n");
	fflush(stdout);
	va_end(args);
}

// errr.h - o rastreio de crash do DLL, que o nThread.cpp chama.
void _n02_TRACE(char*, int) {}

static FILE* g_expected;

static void Record(const char* bytes, int len) {
	fwrite(bytes, 1, len, g_expected);
}

int main(int argc, char** argv) {
	int port = argc > 1 ? atoi(argv[1]) : 5070;
	int frames = argc > 2 ? atoi(argv[2]) : 2400;
	const char* host = argc > 3 ? argv[3] : "127.0.0.1";

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	g_expected = fopen("esperado.bin", "wb");
	if (g_expected == NULL)
		return 1;

	n02_stream_configure(host, port, N02_STREAM_DEFAULT_PATH, N02_STREAM_DEFAULT_API_KEY);
	char names[4][32] = { "host", "convidado", "", "" };
	n02_stream_start_session("RetroArch teste", "PCSX ReARMed: teste do stream", 1, 2, names, "host");

	for (int f = 0; f < frames; f++) {
		char input[24];
		for (int i = 0; i < (int)sizeof(input); i++)
			input[i] = (char)((f * 31 + i * 7) & 0xFF);
		n02_stream_push_frame(input, sizeof(input));
		char rec[3 + sizeof(input)];
		rec[0] = 0x12;
		short len = sizeof(input);
		memcpy(rec + 1, &len, 2);
		memcpy(rec + 3, input, sizeof(input));
		Record(rec, sizeof(rec));

		if (f % 300 == 299) {
			char text[64];
			_snprintf(text, sizeof(text), "[SYNC] teste janela %d", f / 300);
			n02_stream_push_chat("host", text);
			char chat[128];
			int p = 0;
			chat[p++] = 0x08;
			memcpy(chat + p, "host", 5); p += 5;
			memcpy(chat + p, text, strlen(text) + 1); p += (int)strlen(text) + 1;
			Record(chat, p);
		}
		Sleep(16);
	}
	fclose(g_expected);

	n02_stream_end_session();
	printf("partida terminou (%d frames) - esperando o envio acabar\n", frames);
	fflush(stdout);
	Sleep(25000); // retries + a resposta atrasada de 6,5 s do proxy
	n02_stream_shutdown();
	WSACleanup();
	return 0;
}
