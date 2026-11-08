#pragma once

// Placar ao vivo (2026-11-07): the match the frontend reads from the game's
// RAM goes to WE Camp (wg-camp's POST /api/live/match), which shows it as a
// "Ao vivo" card and links it to the players' championship game or friendly.
//
// RetroArch TIERES (retroarch-k3, kaillera/kaillera_sync.c) watches the
// Winning Eleven 2002 engine's match - teams, which pad controls each side,
// score, clock, scorers - and calls the kailleraLiveMatch() export with a
// JSON state at every goal, period change, end of match and every ~15 s.
// Every PC of the match sends it (the game runs in lockstep, so they all see
// the same); WE Camp merges them and checks the final scores agree.
//
// A worker thread sends: each state carries the whole match, so only the
// newest one waiting matters - one that arrives while the previous is still
// going replaces the one waiting, except that a final state is never
// replaced by a later non-final one.

// Queues a state for WE Camp. stateJson: the frontend's JSON; players: the
// game's nicks in 1P,2P,... order, comma separated; reporter: our nick;
// room: the room's game name. final: the end of the match (sent with retries).
void n02_live_submit(const char* stateJson, const char* players, const char* reporter, const char* room, bool final);

// The player left the match (Leave, Drop, the Kaillera window closed - the
// game ended without its final): the last state goes again with "left=1",
// and WE Camp marks the match being played as interrupted right away
// (2026-11-08). Nothing after the match's final. The worker keeps the DLL
// loaded until it's sent (n02_thread.h), even if Kaillera is closing.
void n02_live_left();
