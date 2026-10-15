#pragma once

// Self-update check for kailleraclient.dll against the community server
// (see app/updates.py in the wg-camp repo). Call once per process, early in
// kailleraInit() - it's a synchronous network call plus a possible blocking
// MessageBox, so it must not run on any latency-sensitive path.
//
// If a previous run staged an update (kailleraclient.dll.new sitting next
// to this DLL, e.g. because the swap couldn't complete immediately last
// time), this also finishes installing it before checking for anything
// newer.
//
// Inside the TIERES RetroArch (x64), the update covers the whole light
// package - retroarch.exe, this DLL, the core - and ends by restarting
// RetroArch; see the package section of n02_update.cpp.
void n02_update_check_and_prompt();

// True once a package update was installed and RetroArch was asked to close
// (and reopen): kailleraSelectServerDialog() then returns at once instead of
// opening Kaillera's window.
bool n02_update_restart_pending();
