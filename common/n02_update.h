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
void n02_update_check_and_prompt();
