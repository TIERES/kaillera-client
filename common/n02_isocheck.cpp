#include "n02_isocheck.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// This file is UTF-8: the popup's texts are narrow literals (they keep their
// bytes whatever code page compiles them) converted with CP_UTF8 at run time.

n02_content_id_fn n02_content_id_cb = NULL;

void n02_isoids_clear(n02_isoids* t) {
	t->count = 0;
}

static int Find(const n02_isoids* t, const char* nick) {
	for (int i = 0; i < t->count; i++)
		if (_stricmp(t->nick[i], nick) == 0)
			return i;
	return -1;
}

bool n02_isoids_set(n02_isoids* t, const char* nick, const char* id) {
	int i = Find(t, nick);
	if (i >= 0) {
		if (_stricmp(t->id[i], id) == 0)
			return false;
	} else {
		if (t->count == N02_ISOIDS_MAX)
			return false;
		i = t->count++;
		strncpy(t->nick[i], nick, sizeof(t->nick[i]) - 1);
		t->nick[i][sizeof(t->nick[i]) - 1] = 0;
	}
	strncpy(t->id[i], id, sizeof(t->id[i]) - 1);
	t->id[i][sizeof(t->id[i]) - 1] = 0;
	return true;
}

void n02_isoids_remove(n02_isoids* t, const char* nick) {
	int i = Find(t, nick);
	if (i < 0)
		return;
	t->count--;
	if (i < t->count) {
		memcpy(t->nick[i], t->nick[t->count], sizeof(t->nick[i]));
		memcpy(t->id[i], t->id[t->count], sizeof(t->id[i]));
	}
}

const char* n02_isoids_get(const n02_isoids* t, const char* nick) {
	int i = Find(t, nick);
	return i >= 0 ? t->id[i] : NULL;
}

bool n02_isoid_valid(const char* s) {
	if (s == NULL)
		return false;
	for (int i = 0; i < 8; i++)
		if (!isxdigit((unsigned char)s[i]))
			return false;
	if (s[8] != ':')
		return false;
	int n = 0;
	while (isxdigit((unsigned char)s[9 + n]))
		n++;
	return n > 0 && n <= 12 && s[9 + n] == 0;
}

static void Append(char* out, int cap, const char* text) {
	size_t len = strlen(out);
	if (len + 1 >= (size_t)cap)
		return;
	strncpy(out + len, text, cap - len - 1);
	out[cap - 1] = 0;
}

// "a" / "a e b" / "a, b e c"
static void JoinNames(const char* const* names, int n, char* out, int cap) {
	out[0] = 0;
	for (int i = 0; i < n; i++) {
		if (i > 0)
			Append(out, cap, i == n - 1 ? " e " : ", ");
		Append(out, cap, names[i]);
	}
}

bool n02_iso_verdict(const n02_isoids* t, const char names[][32], int count, n02_isoverdict* v) {
	const char* members[N02_ISO_GROUPS_MAX][N02_ISOIDS_MAX];
	memset(v, 0, sizeof(*v));
	v->majority = -1;
	if (count > N02_ISOIDS_MAX)
		count = N02_ISOIDS_MAX;
	for (int i = 0; i < count; i++) {
		const char* id = n02_isoids_get(t, names[i]);
		if (id == NULL || !id[0]) {
			v->unknown++;
			continue;
		}
		int g;
		for (g = 0; g < v->groups; g++)
			if (_stricmp(v->id[g], id) == 0)
				break;
		if (g == v->groups) {
			if (v->groups == N02_ISO_GROUPS_MAX) {
				v->unknown++;
				continue;
			}
			strncpy(v->id[g], id, N02_ISOID_LEN - 1);
			v->groups++;
		}
		members[g][v->size[g]++] = names[i];
		int p = v->players++;
		strncpy(v->player[p], names[i], sizeof(v->player[p]) - 1);
		v->player_group[p] = g;
	}
	for (int g = 0; g < v->groups; g++)
		JoinNames(members[g], v->size[g], v->nicks[g], sizeof(v->nicks[g]));

	if (v->groups < 2)
		return false;

	int best = 0;
	bool tie = false;
	for (int g = 1; g < v->groups; g++) {
		if (v->size[g] > v->size[best]) {
			best = g;
			tie = false;
		} else if (v->size[g] == v->size[best]) {
			tie = true;
		}
	}
	if (!tie) {
		const char* odd[N02_ISOIDS_MAX];
		int n = 0;
		for (int p = 0; p < v->players; p++)
			if (v->player_group[p] != best)
				odd[n++] = v->player[p];
		v->majority = best;
		v->odd_count = n;
		JoinNames(odd, n, v->odd, sizeof(v->odd));
	}
	return true;
}

void n02_iso_signature(const n02_isoverdict* v, char* out, int cap) {
	out[0] = 0;
	if (v->groups < 2)
		return;
	for (int g = 0; g < v->groups; g++) {
		Append(out, cap, v->id[g]);
		Append(out, cap, "=");
		Append(out, cap, v->nicks[g]);
		Append(out, cap, ";");
	}
}

#define N02_ISO_RISK "Com ISOs diferentes pode dar desync, o M. Card Online pode vir em branco e a partida " \
	"nao aparece no Ao vivo do WE Camp."

void n02_iso_chat_line(const n02_isoverdict* v, char* out, int cap) {
	out[0] = 0;
	if (v->groups < 2)
		return;
	if (v->majority >= 0) {
		if (v->odd_count == 1)
			_snprintf(out, cap - 1, "* ATENCAO: a ISO de %s e diferente da dos outros jogadores da sala. %s precisa baixar o jogo "
				"de novo do mesmo site/fonte dos outros e entrar na sala de novo. " N02_ISO_RISK, v->odd, v->odd);
		else
			_snprintf(out, cap - 1, "* ATENCAO: as ISOs de %s sao diferentes da dos outros jogadores da sala. Eles precisam baixar "
				"o jogo de novo do mesmo site/fonte dos outros e entrar na sala de novo. " N02_ISO_RISK, v->odd);
	} else if (v->groups == 2 && v->size[0] == 1 && v->size[1] == 1) {
		_snprintf(out, cap - 1, "* ATENCAO: as ISOs de %s e de %s sao diferentes (nao da para saber qual esta certa). Baixem o "
			"jogo de novo do mesmo site/fonte e entrem na sala de novo. " N02_ISO_RISK, v->nicks[0], v->nicks[1]);
	} else {
		char groups[N02_ISO_GROUPS_MAX * N02_ISO_NICKS_LEN];
		groups[0] = 0;
		for (int g = 0; g < v->groups; g++) {
			if (g > 0)
				Append(groups, sizeof(groups), " | ");
			Append(groups, sizeof(groups), v->nicks[g]);
		}
		_snprintf(out, cap - 1, "* ATENCAO: a sala tem ISOs diferentes (%s) e nao da para saber qual esta certa. Baixem o jogo "
			"de novo, todos do mesmo site/fonte, e entrem na sala de novo. " N02_ISO_RISK, groups);
	}
	out[cap - 1] = 0;
}

// A nick or game name as UTF-8: RetroArch's game names already are; a Kaillera
// nick with accents comes in the ANSI code page.
static void ToUtf8(const char* s, char* out, int cap) {
	wchar_t w[512];
	out[0] = 0;
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, 512) == 0
		&& MultiByteToWideChar(CP_ACP, 0, s, -1, w, 512) == 0)
		return;
	w[511] = 0;
	if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) == 0)
		out[0] = 0;
	out[cap - 1] = 0;
}

void n02_iso_popup_text(const n02_isoverdict* v, const char* me, const char* game, bool host,
	wchar_t* out, int cap) {
	char text[4096], line[1024], a[N02_ISO_NICKS_LEN * 2], b[N02_ISO_NICKS_LEN * 2], file[512];
	text[0] = 0;
	out[0] = 0;

	// "PCSX ReARMed: Euro vs Copa America 2004.bin" -> the file.
	const char* sep = game ? strstr(game, ": ") : NULL;
	ToUtf8(sep ? sep + 2 : (game ? game : ""), file, sizeof(file));

	int myGroup = -1;
	for (int p = 0; p < v->players; p++)
		if (me && _stricmp(v->player[p], me) == 0)
			myGroup = v->player_group[p];

	if (v->majority >= 0 && myGroup >= 0 && myGroup != v->majority) {
		ToUtf8(v->nicks[v->majority], a, sizeof(a));
		_snprintf(line, sizeof(line) - 1, "A SUA ISO deste jogo é diferente da dos outros jogadores da sala (%s).\n\n"
			"Jogo: %s\n\n"
			"Baixe a ISO de novo do mesmo site/fonte que eles usaram, troque o arquivo e entre na sala de novo.\n\n", a, file);
	} else if (v->majority >= 0) {
		ToUtf8(v->odd, a, sizeof(a));
		_snprintf(line, sizeof(line) - 1, "A ISO de %s é diferente da dos outros jogadores da sala.\n\n"
			"Jogo: %s\n\n"
			"%s %s baixar a ISO de novo do mesmo site/fonte que vocês usaram, trocar o arquivo e entrar na sala de novo.\n\n",
			a, file, a, v->odd_count > 1 ? "precisam" : "precisa");
	} else if (v->groups == 2 && v->size[0] == 1 && v->size[1] == 1) {
		ToUtf8(v->nicks[0], a, sizeof(a));
		ToUtf8(v->nicks[1], b, sizeof(b));
		_snprintf(line, sizeof(line) - 1, "As ISOs de %s e de %s são diferentes, e não dá para saber qual está certa.\n\n"
			"Jogo: %s\n\n"
			"Baixem a ISO de novo do mesmo site/fonte, troquem o arquivo e entrem na sala de novo.\n\n", a, b, file);
	} else {
		_snprintf(line, sizeof(line) - 1, "Os jogadores desta sala estão com ISOs diferentes, e não dá para saber qual está certa.\n\n"
			"Jogo: %s\n\n"
			"Baixem a ISO de novo, todos do mesmo site/fonte, troquem o arquivo e entrem na sala de novo.\n\n", file);
	}
	line[sizeof(line) - 1] = 0;
	Append(text, sizeof(text), line);
	Append(text, sizeof(text), "Com ISOs diferentes a partida pode dessincronizar, o M. Card Online pode vir em branco "
		"e o jogo não aparece no Ao vivo do WE Camp.\n\nCódigo da ISO de cada jogador:\n");
	for (int p = 0; p < v->players; p++) {
		ToUtf8(v->player[p], a, sizeof(a));
		_snprintf(line, sizeof(line) - 1, "    %s: %s\n", a, v->id[v->player_group[p]]);
		line[sizeof(line) - 1] = 0;
		Append(text, sizeof(text), line);
	}
	if (host)
		Append(text, sizeof(text), "\nIniciar a partida mesmo assim?");

	if (MultiByteToWideChar(CP_UTF8, 0, text, -1, out, cap) == 0)
		out[0] = 0;
	out[cap - 1] = 0;
}
