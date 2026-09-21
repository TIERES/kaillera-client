#pragma once

// Standalone .krec parser, extracted from player.cpp's PlayBackBufferC +
// player_MPV()'s record-type switch (retry-connect Fase 1). Behavior mirrors
// that code exactly - same header detection (KRC0 272 bytes / KRC1 400
// bytes), same load_char/load_short/load_int/load_bytes/load_str semantics,
// same 0x12 (input) / 0x08 (chat) / 0x14 (drop) record layout - but as an
// instantiable object instead of a single shared global, and classifying a
// record instead of acting on it (feeding the emulator, calling
// chatReceivedCallback, ...), which stays the caller's job.
//
// Not used by player.cpp's watch-mode/streaming path (player_watch_begin(),
// PlayBackBuffer_Append()) or by the Records list's duration scan
// (RecordsList_Populate_fn()) - those keep using the original PlayBackBufferC
// global unchanged for now; only player_play()/player_MPV() (static local
// .krec playback) were moved onto this reader.

#include <windows.h>
#include <string.h>
#include <stdlib.h>

#ifndef min
#define min(a,b) ((a<b)? a:b)
#endif

enum krec_record_type {
	KREC_EOF = -1,
	KREC_UNKNOWN = 0,
	KREC_CHAT = 0x08,
	KREC_INPUT = 0x12,
	KREC_DROP = 0x14,
};

class krec_reader {
public:
	char* buffer;
	char* ptr;
	char* end;
	bool owns_buffer;

	char appName[128];
	char gameName[128];
	int playerno;
	int numplayers;
	bool is_krc1;

	// Count of 0x12 (input) records consumed so far - does not exist anywhere
	// in the original PlayBackBufferC/player_MPV code; new for retry-connect.
	int frames_consumed;

	// Filled in by next_record() when it returns KREC_DROP/KREC_CHAT - kept
	// separate from out_values/max_size (which are the caller's *input*
	// buffer, e.g. RetroArch's controller-state buffer, and may be far
	// smaller than a nick+message) rather than risking overflowing it, the
	// same way the original player_MPV() decoded these into its own local
	// stack buffers instead of into `values`.
	char last_drop_nick[100];
	int last_drop_playerno;
	char last_chat_nick[100];
	char last_chat_msg[500];

	krec_reader() : buffer(NULL), ptr(NULL), end(NULL), owns_buffer(false),
		playerno(0), numplayers(0), is_krc1(false), frames_consumed(0),
		last_drop_playerno(0) {
		appName[0] = 0;
		gameName[0] = 0;
		last_drop_nick[0] = 0;
		last_chat_nick[0] = 0;
		last_chat_msg[0] = 0;
	}

	~krec_reader() {
		close();
	}

	void close() {
		if (owns_buffer && buffer)
			free(buffer);
		buffer = ptr = end = NULL;
		owns_buffer = false;
		frames_consumed = 0;
	}

	// --- primitives, identical semantics to PlayBackBufferC (player.cpp) ---

	void load_bytes(void* out, unsigned int len) {
		if (ptr + 10 < end) {
			unsigned int p = min(len, (unsigned int)(end - ptr));
			memcpy(out, ptr, p);
			ptr += p;
		}
	}
	void load_str(char* out, unsigned int maxlen) {
		unsigned int len = min(maxlen, (unsigned int)strlen(ptr) + 1);
		len = min(len, (unsigned int)(end - ptr + 1));
		load_bytes(out, len);
		out[len] = 0;
	}
	int load_int() {
		int x = 0;
		load_bytes(&x, 4);
		return x;
	}
	unsigned char load_char() {
		unsigned char x = 0;
		load_bytes(&x, 1);
		return x;
	}
	unsigned short load_short() {
		unsigned short x = 0;
		load_bytes(&x, 2);
		return x;
	}

	// --- header parsing, mirrors player_play()'s header handling ---

	// Parses the KRC0/KRC1 header out of an already-in-memory buffer. The
	// reader does not take ownership - caller frees `data` after close().
	bool open_from_memory(char* data, DWORD len) {
		close();
		if (len < 272)
			return false;
		is_krc1 = (memcmp(data, "KRC1", 4) == 0);
		DWORD headerSize = is_krc1 ? 400 : 272;
		if (len < headerSize)
			return false;

		buffer = data;
		end = data + len;

		ptr = buffer + 4;
		load_str(appName, sizeof(appName));

		ptr = buffer + 132;
		load_str(gameName, sizeof(gameName));

		ptr = buffer + 264;
		playerno = load_int();
		numplayers = load_int();

		ptr = buffer + headerSize;
		owns_buffer = false;
		return true;
	}

	// Reads a whole file from disk and parses its header. Reader owns the
	// buffer (freed on close()/destruction).
	bool open_file(const char* path) {
		close();
		HANDLE in = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (in == INVALID_HANDLE_VALUE)
			return false;

		DWORD len = SetFilePointer(in, 0, NULL, FILE_END);
		SetFilePointer(in, 0, NULL, FILE_BEGIN);
		if (len < 272) {
			CloseHandle(in);
			return false;
		}

		char* data = (char*)malloc(len);
		if (!data) {
			CloseHandle(in);
			return false;
		}
		DWORD bytesRead;
		ReadFile(in, data, len, &bytesRead, NULL);
		CloseHandle(in);

		if (!open_from_memory(data, len)) {
			free(data);
			return false;
		}
		owns_buffer = true;
		return true;
	}

	bool at_end() const {
		return !(ptr + 10 < end);
	}

	int frame_index() const {
		return frames_consumed;
	}

	// Mirrors player_MPV()'s record-type switch (player.cpp), but classifies
	// and extracts a record instead of acting on it - the caller decides what
	// a chat/drop/input record means. Returns the record type, or KREC_EOF
	// once the buffer is drained (matching player_MPV()'s "ptr+10>=end"
	// check).
	//   KREC_INPUT: out_values/max_size receive the raw input bytes (as many
	//               as fit), out_len receives the record's real length.
	//   KREC_CHAT:  decoded into last_chat_nick/last_chat_msg; out_values
	//               untouched.
	//   KREC_DROP:  decoded into last_drop_nick/last_drop_playerno;
	//               out_values untouched.
	int next_record(void* out_values, int max_size, int* out_len) {
		if (out_len) *out_len = 0;
		if (at_end())
			return KREC_EOF;

		unsigned char b = load_char();

		if (b == KREC_INPUT) {
			// Same widening as the original PlayBackBufferC::load_short()
			// call site: unsigned short -> int always zero-extends (never
			// negative), so this "l < 0" check is dead code there too - kept
			// only to mirror the original exactly, not because it can fire.
			int l = load_short();
			if (l < 0)
				return KREC_EOF;
			if (l > 0 && out_values) {
				int n = min(l, max_size);
				load_bytes(out_values, n);
			}
			if (out_len) *out_len = l;
			frames_consumed++;
			return KREC_INPUT;
		}

		if (b == KREC_DROP) {
			load_str(last_drop_nick, sizeof(last_drop_nick));
			last_drop_playerno = load_int();
			return KREC_DROP;
		}

		if (b == KREC_CHAT) {
			load_str(last_chat_nick, sizeof(last_chat_nick));
			load_str(last_chat_msg, sizeof(last_chat_msg));
			return KREC_CHAT;
		}

		// Unknown record type - player_MPV() has no default branch either,
		// it just falls through and the caller ends up returning -1.
		return KREC_UNKNOWN;
	}
};
