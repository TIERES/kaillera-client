#pragma once

// A worker thread that keeps this DLL loaded until it ends (2026-10-31).
// RetroArch unloads the DLL as soon as the Kaillera window closes, and a
// worker still running in it then crashes the whole process the moment it
// wakes - the M. Card sends of a match that just ended (McFinishThread waits
// 15 s before its last one), a WE Camp request... The thread takes its own
// reference to the DLL and drops it as it exits (FreeLibraryAndExitThread),
// so the DLL only leaves memory once the last one is done.
//
// Only for threads that always end on their own: one stopped with
// TerminateThread() never drops its reference, and the DLL then stays in
// memory until RetroArch closes.

#include <windows.h>

struct n02_thread_start {
	LPTHREAD_START_ROUTINE start;
	void* arg;
	HMODULE self;
};

static DWORD WINAPI n02_thread_proc(LPVOID param) {
	n02_thread_start s = *(n02_thread_start*)param;
	delete (n02_thread_start*)param;
	FreeLibraryAndExitThread(s.self, s.start(s.arg)); // never returns
}

// CreateThread(NULL, 0, start, arg, 0, NULL), with the DLL kept loaded.
// NULL if the thread couldn't be created - then nothing runs and nothing is
// held.
static inline HANDLE n02_create_thread(LPTHREAD_START_ROUTINE start, void* arg) {
	n02_thread_start* s = new n02_thread_start;
	s->start = start;
	s->arg = arg;
	s->self = NULL;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&n02_thread_proc, &s->self)) {
		delete s;
		return NULL;
	}
	HANDLE t = CreateThread(NULL, 0, n02_thread_proc, s, 0, NULL);
	if (t == NULL) {
		FreeLibrary(s->self);
		delete s;
	}
	return t;
}
