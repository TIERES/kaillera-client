#include "n02_mcchoice.h"

#include <stdio.h>
#include <string.h>

void n02_mcchoices_clear(n02_mcchoices* t) {
	t->count = 0;
}

static int Find(const n02_mcchoices* t, const char* nick) {
	for (int i = 0; i < t->count; i++)
		if (_stricmp(t->nick[i], nick) == 0)
			return i;
	return -1;
}

bool n02_mcchoices_set(n02_mcchoices* t, const char* nick, int choice, bool noLogin) {
	int i = Find(t, nick);
	if (i >= 0) {
		if (t->choice[i] == choice && t->noLogin[i] == noLogin)
			return false;
	} else {
		if (t->count == N02_MCCHOICES_MAX)
			return false;
		i = t->count++;
		strncpy(t->nick[i], nick, sizeof(t->nick[i]) - 1);
		t->nick[i][sizeof(t->nick[i]) - 1] = 0;
	}
	t->choice[i] = choice;
	t->noLogin[i] = noLogin;
	return true;
}

void n02_mcchoices_remove(n02_mcchoices* t, const char* nick) {
	int i = Find(t, nick);
	if (i < 0)
		return;
	t->count--;
	if (i < t->count) {
		memcpy(t->nick[i], t->nick[t->count], sizeof(t->nick[i]));
		t->choice[i] = t->choice[t->count];
		t->noLogin[i] = t->noLogin[t->count];
	}
}

int n02_mcchoices_get(const n02_mcchoices* t, const char* nick) {
	int i = Find(t, nick);
	return i < 0 ? -1 : t->choice[i];
}

bool n02_mcchoices_no_login(const n02_mcchoices* t, const char* nick) {
	int i = Find(t, nick);
	return i >= 0 && t->noLogin[i];
}

void n02_mcchoice_line(int choice, bool noLogin, char* out, int cap) {
	const char* word = choice == N02_MEMCARD_CHOICE_ONLINE ? "online"
		: choice == N02_MEMCARD_CHOICE_ISO ? "iso"
		: noLogin ? "sem login" : "sem";
	_snprintf(out, cap, "%s%s", N02_MCCHOICE_CHAT_PREFIX, word);
	out[cap - 1] = 0;
}

bool n02_mcchoice_parse(const char* text, int* choice, bool* noLogin) {
	*noLogin = false;
	if (strcmp(text, "online") == 0)
		*choice = N02_MEMCARD_CHOICE_ONLINE;
	else if (strcmp(text, "iso") == 0)
		*choice = N02_MEMCARD_CHOICE_ISO;
	else if (strcmp(text, "sem") == 0)
		*choice = N02_MEMCARD_CHOICE_NONE;
	else if (strcmp(text, "sem login") == 0) {
		*choice = N02_MEMCARD_CHOICE_NONE;
		*noLogin = true;
	} else
		return false;
	return true;
}

const char* n02_mcchoice_label(int choice) {
	return choice == N02_MEMCARD_CHOICE_ONLINE ? "M. Card Online"
		: choice == N02_MEMCARD_CHOICE_ISO ? "M. Card da ISO" : "Sem M. Card";
}

bool n02_mcchoice_is_pcsx(const char* game) {
	char lower[256];
	strncpy(lower, game ? game : "", sizeof(lower) - 1);
	lower[sizeof(lower) - 1] = 0;
	_strlwr(lower);
	return strstr(lower, "pcsx") != NULL;
}

static bool InList(const char* list, const char* nick) {
	if (list == NULL)
		return false;
	size_t n = strlen(nick);
	for (const char* p = list; *p; ) {
		const char* comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);
		if (len == n && _strnicmp(p, nick, n) == 0)
			return true;
		if (!comma)
			break;
		p = comma + 1;
	}
	return false;
}

void n02_mcchoice_game_slots(const char names[][32], int count, const n02_mcchoices* t,
	const char* accounts, bool pcsx, int slot[2], bool* outdated) {
	slot[0] = slot[1] = N02_MEMCARD_CHOICE_NONE;
	*outdated = false;
	if (!pcsx || count < 1)
		return;
	for (int i = 0; i < count; i++) {
		if (n02_mcchoices_get(t, names[i]) < 0) {
			*outdated = true;
			return;
		}
	}
	for (int s = 0; s < 2; s++) {
		int p = s < count ? s : 0; // solo: slot 2 only for the 1P's "M. Card da ISO"
		int choice = n02_mcchoices_get(t, names[p]);
		if (p != s && choice != N02_MEMCARD_CHOICE_ISO)
			continue;
		if (!InList(accounts, names[p]))
			continue; // no confirmed account: "Sem M. Card"
		slot[s] = choice;
	}
}
