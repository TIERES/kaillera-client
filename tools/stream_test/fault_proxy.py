"""Sobe um wg-camp local (banco temporário, chave do DLL) atrás de um proxy que
estraga parte dos POSTs do /spectate/ingest, como em produção, e confere se o
stream que chegou é exatamente o que o stream_test.exe enviou.

    <wg-camp>\\.venv\\Scripts\\python.exe fault_proxy.py <wg-camp> esperado.bin [--port 5070]

Depois rode o stream_test.exe (outro terminal). Falhas, pela ordem do POST:
  - 502 sem repassar (o que o Caddy devolvia quando o gunicorn fechava a conexão);
  - repassa e fecha sem responder (o servidor gravou, a resposta se perdeu);
  - repassa e só responde depois de 6,5 s (o DLL desiste antes e reenvia);
  - fecha logo depois dos cabeçalhos (o corpo nem chega).
"""
import argparse
import re
import socket
import socketserver
import sqlite3
import sys
import tempfile
import threading
import time
from pathlib import Path


def dll_api_key():
    header = Path(__file__).resolve().parents[2] / "common" / "n02_stream.h"
    return re.search(r'N02_STREAM_DEFAULT_API_KEY\s+"([0-9a-f]+)"', header.read_text(encoding="latin-1")).group(1)


FAULTS = {3: "502", 5: "sem-resposta", 8: "resposta-atrasada", 11: "fecha-antes-do-corpo"}
stats = {"total": 0}
stats_lock = threading.Lock()


def count(key):
    with stats_lock:
        stats[key] = stats.get(key, 0) + 1


def fault_for(n):
    # A cada 12 POSTs, os 4 tipos de falha, nunca dois seguidos.
    return FAULTS.get(n % 12)


def read_request(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(65536)
        if not chunk:
            return None, None, 0
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)
    m = re.search(rb"(?i)content-length:\s*(\d+)", head)
    length = int(m.group(1)) if m else 0
    return head, rest, length


class Proxy(socketserver.BaseRequestHandler):
    upstream = None

    def handle(self):
        sock = self.request
        got = read_request(sock)
        if got[0] is None:
            return
        head, body, length = got
        is_ingest = head.startswith(b"POST /spectate/ingest")
        with stats_lock:
            n = stats["total"] = stats["total"] + 1 if is_ingest else stats["total"]
        fault = fault_for(n) if is_ingest else None
        if fault == "fecha-antes-do-corpo":
            count(fault)
            sock.close()
            return
        while len(body) < length:
            chunk = sock.recv(65536)
            if not chunk:
                return
            body += chunk
        if fault == "502":
            count(fault)
            sock.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            sock.close()
            return
        up = socket.create_connection(self.upstream)
        up.sendall(head + b"\r\n\r\n" + body)
        reply = b""
        while True:
            chunk = up.recv(65536)
            if not chunk:
                break
            reply += chunk
        up.close()
        if b"X-Duplicate" in reply:
            count("repetido-ignorado")
        if fault == "sem-resposta":
            count(fault)
            sock.close()
            return
        if fault == "resposta-atrasada":
            count(fault)
            time.sleep(6.5)
        try:
            sock.sendall(reply)
        except OSError:
            pass


class ThreadedServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wgcamp")
    ap.add_argument("expected", help="o esperado.bin que o stream_test.exe grava")
    ap.add_argument("--port", type=int, default=5070)
    ap.add_argument("--timeout", type=int, default=180)
    args = ap.parse_args()

    sys.path.insert(0, args.wgcamp)
    from app import create_app
    from app.db import init_db
    from werkzeug.serving import make_server

    root = Path(tempfile.mkdtemp(prefix="stream-test-"))
    app = create_app({
        "TESTING": True, "SECRET_KEY": "x", "DATABASE": str(root / "arena17.db"),
        "DOWNLOADS_DIR": str(root / "d"), "UPLOAD_TMP_DIR": str(root / "u"), "LIVE_DIR": str(root / "live"),
        "UPDATES_DIR": str(root / "up"), "REPLAY_BACKUPS_DIR": str(root / "rb"), "MEMCARDS_DIR": str(root / "mc"),
        "SPECTATE_API_KEY": dll_api_key(),
    })
    with app.app_context():
        init_db()
    # O http.server chama getfqdn(), que quebra no Python 3.14 com o nome do PC acentuado.
    socket.getfqdn = lambda name="": name or "localhost"
    app_server = make_server("127.0.0.1", args.port + 1, app, threaded=True)
    threading.Thread(target=app_server.serve_forever, daemon=True).start()
    Proxy.upstream = ("127.0.0.1", args.port + 1)
    proxy = ThreadedServer(("127.0.0.1", args.port), Proxy)
    threading.Thread(target=proxy.serve_forever, daemon=True).start()
    print("wg-camp em 127.0.0.1:%d, proxy com falhas em 127.0.0.1:%d - rode o stream_test.exe" % (args.port + 1, args.port), flush=True)

    db = sqlite3.connect(root / "arena17.db")
    deadline = time.time() + args.timeout
    row = None
    while time.time() < deadline:
        row = db.execute("SELECT session_id, status, last_sequence, lost_batches, bytes_received FROM live_sessions").fetchone()
        if row and row[1] == "finished":
            break
        time.sleep(0.5)
    if not row or row[1] != "finished":
        print("FALHOU: a sessao nao terminou em %ds (%r)" % (args.timeout, row))
        return 1

    time.sleep(1)  # uma resposta atrasada ainda pode estar saindo
    got = (root / "live" / (row[0] + ".krec")).read_bytes()
    expected = Path(args.expected).read_bytes()
    print("POSTs no ingest: %d, falhas: %s" % (stats["total"], {k: v for k, v in stats.items() if k != "total"}))
    print("sessao %s: ultima sequencia %d, lotes perdidos (buracos) %d, %d bytes" % (row[0], row[2], row[3], row[4]))
    body = got[400:]
    if got[:4] == b"KRC1" and body == expected:
        print("RESULTADO: OK - o stream do servidor e identico ao enviado (%d bytes de registros)" % len(body))
        return 0
    n = min(len(body), len(expected))
    first = next((i for i in range(n) if body[i] != expected[i]), n)
    print("RESULTADO: DIFERENTE - servidor %d bytes x enviado %d bytes, primeira diferenca no byte %d dos registros"
          % (len(body), len(expected), first))
    return 1


if __name__ == "__main__":
    sys.exit(main())
