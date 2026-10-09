"""Sobe um wg-camp local (banco temporário) com duas contas confirmadas, para o
wecamp_client.exe testar o cliente do DLL. Uso:

    python local_wgcamp.py C:\\TIERES\\wg-camp --port 5055 --users Pele,Mtgamess --password segredo123

A chave de espectador é a mesma do DLL (N02_STREAM_DEFAULT_API_KEY), para o
download de cartão por X-Api-Key também ser testado.
"""
import argparse
import re
import sys
import tempfile
from pathlib import Path


def dll_api_key():
    header = Path(__file__).resolve().parents[2] / "common" / "n02_stream.h"
    match = re.search(r'N02_STREAM_DEFAULT_API_KEY\s+"([0-9a-f]+)"', header.read_text())
    return match.group(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wgcamp")
    parser.add_argument("--port", type=int, default=5055)
    parser.add_argument("--users", default="Pele,Mtgamess")
    parser.add_argument("--password", default="segredo123")
    args = parser.parse_args()

    sys.path.insert(0, args.wgcamp)
    from app import create_app
    from app.db import get_db, init_db, now
    from werkzeug.security import generate_password_hash
    from werkzeug.serving import run_simple

    root = Path(tempfile.mkdtemp(prefix="wgcamp-test-"))
    app = create_app({
        "SECRET_KEY": "local-test",
        "DATABASE": str(root / "arena17.db"),
        "DOWNLOADS_DIR": str(root / "downloads"),
        "UPLOAD_TMP_DIR": str(root / "tmp"),
        "MEMCARDS_DIR": str(root / "memcards"),
        "SPECTATE_API_KEY": dll_api_key(),
    })
    Path(app.config["MEMCARDS_DIR"]).mkdir(parents=True, exist_ok=True)
    with app.app_context():
        init_db()
        db = get_db()
        for name in args.users.split(","):
            db.execute(
                "INSERT INTO players (username, email, password_hash, email_verified_at, created_at, updated_at) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (name, f"{name.lower()}@example.com", generate_password_hash(args.password), now(), now(), now()),
            )
        db.commit()
    print(f"wg-camp local em http://127.0.0.1:{args.port} (dados em {root})", flush=True)
    run_simple("127.0.0.1", args.port, app, threaded=True)


if __name__ == "__main__":
    main()
