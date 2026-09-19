/******************************************************************************
Live spectator streaming.

While a game is being recorded locally to a .krec file, the host can also
enable this to push a copy of the same records (frame/chat/drop) to an HTTP
endpoint in small periodic batches, so a separate web service can relay the
match to spectators. This module owns the outbound batching/POSTing and never
blocks the caller - all network I/O happens on its own background thread.

Wire contract (for the receiving web endpoint):
  - POST {host:port/path} roughly every N02_STREAM_BATCH_MS while there is
    data to send, plus one final POST when the session ends.
  - Headers: X-Session-Id: <id>, X-Sequence: <uint, 0-based per session>,
    X-Session-End: true (only on the last POST of a session), and
    X-Api-Key: <key> when an API key has been configured.
  - The first POST of a session is prefixed with a 400-byte header, byte
    identical to the local .krec KRC1 header (magic + appName + gameName +
    timestamp + playerno + numplayers + 4x32-byte player names).
  - The body (after the optional header) is a straight concatenation of
    records in the same format written to .krec: 0x12+short len+len bytes
    (frame), 0x08+nick\0+msg\0 (chat), 0x14+nick\0+4-byte player number
    (drop). Concatenating all batches of a session in X-Sequence order
    reproduces a valid .krec record stream.
******************************************************************************/
#pragma once

// Built-in default spectate endpoint (the community server), used whenever
// the user leaves the corresponding part of the "Endpoint" field blank - so
// enabling "Stream live" works with zero configuration. The API key here is
// only a mild abuse deterrent, not a real secret: it ships inside a DLL
// handed out to every player, so anyone can extract it from the binary.
#define N02_STREAM_DEFAULT_HOST "we2002.wgs.dev.br"
#define N02_STREAM_DEFAULT_PORT 8080
#define N02_STREAM_DEFAULT_PATH "/spectate/ingest"
#define N02_STREAM_DEFAULT_API_KEY "25dc982372ced3c0f18a53e18d9224a8ebbec36974e9f6520ea8fb3e88b0aa22"

// Sets the destination for the next session(s). apiKey may be NULL/empty if
// the endpoint doesn't require one. Safe to call any time; takes effect on
// the next n02_stream_start_session().
void n02_stream_configure(const char* host, int port, const char* path, const char* apiKey);

// Parses a user-typed "[apikey@]host[:port][/path]" endpoint string (any
// part may be omitted - e.g. "", "/ingest", ":9090", "secret@host:9090/ingest")
// and configures the stream, falling back to defaultHost/defaultPort/
// defaultPath/defaultApiKey for whichever parts were omitted (an omitted
// "key@" prefix falls back to defaultApiKey, which may itself be "" for no
// X-Api-Key header).
void n02_stream_configure_from_text(const char* rawEndpoint, const char* defaultHost, int defaultPort, const char* defaultPath, const char* defaultApiKey);

// Starts a new streaming session and (lazily) the background sender thread.
// playerNames must point to 4 buffers of 32 bytes each (may be NULL).
// ownerName is the hosting user's own name (may be NULL/empty) - sent as the
// X-Owner-Name header on every batch, so a spectator's /spectate/lookup can
// disambiguate rooms that share the same name by also matching the host,
// same as the room list already shows both a game name and an owner.
void n02_stream_start_session(const char* appName, const char* gameName, int playerno, int numplayers, char playerNames[4][32], const char* ownerName);

// Enqueues one record, mirroring exactly what's written to the local .krec
// file for the same event. No-op if no session is active. Never blocks.
void n02_stream_push_frame(const void* values, int len);
void n02_stream_push_chat(const char* nick, const char* text);
void n02_stream_push_drop(const char* nick, int playernb);

// Marks the current session finished; the next batch sent will carry
// X-Session-End: true. Safe to call even if no session is active.
void n02_stream_end_session();

// Called from kailleraShutdown(): ends any active session and stops the
// background thread.
void n02_stream_shutdown();
