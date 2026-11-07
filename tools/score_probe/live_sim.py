"""Simula o placar ao vivo de uma partida: reproduz um .krec no krec_harness
e manda para o WE Camp (POST /api/live/match) o mesmo estado que o RetroArch
TIERES manda - a lógica de MatchWatch abaixo é a do fork (retroarch-k3,
kaillera/kaillera_sync.c, ksync_live_*); mantenha as duas iguais.

    live_sim.py <partida.krec> --game <iso> [--card1 x.mcd] [--url https://...]
                [--speed 4] [--both] [--dry]

--speed: 1 = tempo real, 4 = quatro vezes mais rápido (0 = sem esperar).
--both: manda como os dois PCs (o WE Camp junta e confere o placar no fim).
--dry: só imprime o que mandaria.
As simulações vão com test=1: só o admin do site vê os cards.
"""
import argparse
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score_probe as SP  # noqa: E402
from krec import Krec  # noqa: E402

DEFAULT_KEY = "25dc982372ced3c0f18a53e18d9224a8ebbec36974e9f6520ea8fb3e88b0aa22"  # N02_STREAM_DEFAULT_API_KEY

# Mapa da RAM de cada motor (README.md) - endereços físicos (sem o 0x80000000).
# O jogo é reconhecido pelo nome do executável que o kernel guarda em SERIAL;
# os patches mantêm o do motor. Lado 0 = LOCAL do jogo.
SERIAL = 0x9E19
TEAMS = 128
ENGINES = {
    "SLPM_870.56": {  # Winning Eleven 2002 (Euro vs Copa America, CAN 2025, BR Turbo...)
        "label": "WE2002", "team_id": 0x75564, "period": 0x75566, "clock": 0x7591C, "in_play": None,
        "score": 0x75702, "goals": (0x755C4, 0x75644), "roster": (0x64570, 0x646E0), "roster_len": 23,
        "name_len": 16, "pads": 0x758D9,
        # Só na tela de seleção de times: ponteiro para o nome de cada time e
        # o código de 3 letras (aqui, os próprios códigos, time 0 por último).
        "sel_names": 0xBF0E8, "sel_codes": 0x800DC, "code_ptrs": False, "mixed_case": False,
    },
    "SLPM_862.91": {  # Winning Eleven 4 (Traducida)
        "label": "WE4", "team_id": 0x750FB, "period": 0x750FD, "clock": 0x754A8, "in_play": 0x754B9,
        "score": 0x75293, "goals": (0x75157, 0x751D7), "roster": (0x7490C, 0x749E8), "roster_len": 22,
        "name_len": 10, "pads": 0x75469,
        "sel_names": 0xA67C0, "sel_codes": 0xA6AC0, "code_ptrs": True, "mixed_case": True,
    },
}

SAMPLE_FRAMES = 30       # lê a RAM a cada meio segundo
HEARTBEAT_FRAMES = 900   # e manda pelo menos a cada 15 s (o relógio do card)
MIN_KICKOFF_CLOCK = 1800  # um tempo de pelo menos 30 s (partidas de 2 minutos para cima)


def cstr(ram, addr, cap):
    raw = ram[addr:addr + cap]
    end = raw.find(b"\0")
    raw = raw[:end if end >= 0 else cap]
    return raw.decode("latin-1") if raw and all(32 <= b < 127 for b in raw) else ""


def engine_of(ram):
    serial = ram[SERIAL:SERIAL + 11].decode("latin-1", "replace")
    return serial, ENGINES.get(serial)


def pointer(ram, addr):
    (ptr,) = struct.unpack_from("<I", ram, addr)
    return ptr & 0x1FFFFF if 0x80000000 <= ptr < 0x80000000 + len(ram) else None


def capture_names(ram, m):
    """Nomes e códigos de todos os times enquanto a tela de seleção está na
    RAM, ou None: só vale a tabela inteira, do time 0 até depois dos dois
    times escolhidos agora."""
    names, codes = [], []
    for team in range(TEAMS):
        name_at = pointer(ram, m["sel_names"] + 4 * team)
        code_at = pointer(ram, m["sel_codes"] + 4 * team) if m["code_ptrs"] else m["sel_codes"] - 4 * team
        if name_at is None or code_at is None or (not m["code_ptrs"] and ram[code_at + 3] != 0):
            break
        name, code = cstr(ram, name_at, 24)[:16], cstr(ram, code_at, 4)
        if not name or (not m["mixed_case"] and name.upper() != name) or len(code) != 3:
            break
        names.append(name)
        codes.append(code)
    need = max(ram[m["team_id"]], ram[m["team_id"] + 1])
    return (names, codes) if len(names) >= 8 and len(names) > need else None


def read_state(ram, m):
    """Estado da partida na RAM, ou None se não parece uma partida desse motor."""
    period, clock = ram[m["period"]], struct.unpack_from("<H", ram, m["clock"])[0]
    pads = list(ram[m["pads"]:m["pads"] + 4])
    if period > 4 or any(p not in (0, 1, 0xFF) for p in pads):
        return None
    teams = []
    size = m["name_len"]
    for side in (0, 1):
        team_id, goals = ram[m["team_id"] + side], ram[m["score"] + side]
        if team_id >= TEAMS or goals > 30:
            return None
        scorers = []
        base = m["goals"][side]
        for n in range(32):
            scorer, assist, minute, _ = ram[base + 4 * n:base + 4 * n + 4]
            if scorer == 0xFF:
                break
            if scorer >= m["roster_len"] or minute > 200:
                return None
            roster = m["roster"][side]
            scorers.append([cstr(ram, roster + size * scorer, size), minute,
                            cstr(ram, roster + size * assist, size) if assist < m["roster_len"] else ""])
        teams.append({"id": team_id, "name": "", "code": "", "goals": goals, "scorers": scorers})
    return {"period": period, "clock": clock, "pads": pads, "teams": teams,
            "in_play": ram[m["in_play"]] if m["in_play"] else 1}


class MatchWatch:
    """Pontapé inicial, gols, troca de tempo e fim - o que o fork faz por frame."""

    def __init__(self, content):
        self.content = content
        self.active = False
        self.kickoff = 0
        self.half = 18000
        self.last = None        # último estado lido
        self.sent_key = None    # o que mudou desde o último envio
        self.sent_frame = 0
        self.names = None       # (nomes, códigos) da última tela de seleção

    def sample(self, frame, ram):
        """Chamado a cada SAMPLE_FRAMES; devolve o estado a mandar, ou None."""
        serial, m = engine_of(ram)
        if m and not self.active:
            self.names = capture_names(ram, m) or self.names
        st = read_state(ram, m) if m else None
        prev, self.last = self.last, st
        if st is None:
            self.active = False
            return None
        final = False
        if prev is not None:
            # Pontapé inicial do 1º tempo: o relógio salta para a duração do tempo
            # (de 0, ou - WE4, depois de sair pelo menu de pausa - de onde ele seguia descendo).
            if st["period"] == 0 and st["clock"] >= MIN_KICKOFF_CLOCK and st["clock"] >= prev["clock"] + MIN_KICKOFF_CLOCK:
                self.active, self.kickoff, self.half = True, frame, st["clock"]
            # Entrou no meio (retry-connect): o relógio já está correndo.
            elif not self.active and 0 < st["clock"] < prev["clock"] and st["in_play"]:
                self.active, self.kickoff = True, 0
            elif self.active and m["in_play"]:
                # WE4: a bola para depois do 2º tempo com o relógio em 0 = fim; com o relógio correndo = saiu pela pausa.
                if prev["in_play"] and not st["in_play"] and st["period"] == 1:
                    if st["clock"] == 0:
                        final = True
                    else:
                        self.active = False
            # WE2002: depois do 2º tempo, o período volta a 0 com o relógio zerado.
            elif self.active and prev["period"] == 1 and st["period"] == 0 and st["clock"] == 0:
                final = True
        if not self.active:
            return None
        key = json.dumps([st["period"], st["pads"], st["teams"]])
        if not final and key == self.sent_key and frame - self.sent_frame < HEARTBEAT_FRAMES:
            return None
        self.sent_key, self.sent_frame = key, frame
        if final:
            self.active = False
            st = dict(st, period=1, clock=0)  # o placar fica; no WE2002 o período já voltou a 0
        st = {k: v for k, v in st.items() if k != "in_play"}
        if self.names:
            for team in st["teams"]:
                if team["id"] < len(self.names[0]):
                    team["name"], team["code"] = self.names[0][team["id"]], self.names[1][team["id"]]
        return {"v": 2, "engine": serial, "content": self.content, "kickoff": self.kickoff, "frame": frame,
                "half": self.half, "final": int(final), **st}


def post(url, key, state, players, reporter, room):
    body = urllib.parse.urlencode({"state": json.dumps(state), "players": ",".join(players), "reporter": reporter,
                                   "room": room, "test": "1"}).encode()
    req = urllib.request.Request(url.rstrip("/") + "/api/live/match", data=body, headers={"X-Api-Key": key})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.read().decode().strip().replace("\n", " ")
    except urllib.error.HTTPError as e:
        return "HTTP %d %s" % (e.code, e.read()[:200])
    except OSError as e:
        return "erro: %s" % e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("krec")
    ap.add_argument("--game", required=True)
    ap.add_argument("--ra", default=r"D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.0.5")
    ap.add_argument("--card1")
    ap.add_argument("--card2")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "live_sim"))
    ap.add_argument("--url", default="http://localhost:5000")
    ap.add_argument("--key", default=DEFAULT_KEY)
    ap.add_argument("--speed", type=float, default=4)
    ap.add_argument("--both", action="store_true")
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--frames", type=int, default=0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    k = Krec(a.krec)
    ns = argparse.Namespace(opt=[], ra=a.ra, game=a.game, out=a.out, card1=a.card1, card2=a.card2, swap=False)
    core = SP.open_core(ns, k)
    content = "%08X:%X" % SP.content_id(a.game)
    players = [n for n in k.names[:k.numplayers] if n]
    room = k.game
    watch = MatchWatch(content)
    last = min(len(k.frames), a.frames) if a.frames else len(k.frames)
    t0, paced_from = time.time(), None
    print("%s | %s | %s | jogadores %s" % (os.path.basename(a.krec), room, content, players), flush=True)
    for f in range(1, last + 1):
        core.set_input(k.frames[f - 1])
        core.run()
        if f % SAMPLE_FRAMES:
            continue
        st = watch.sample(f, core.ram())
        if watch.active and paced_from is None:
            paced_from = (f, time.time())  # do pontapé inicial em diante, no ritmo pedido
        if paced_from and a.speed > 0:
            ahead = (f - paced_from[0]) / 60.0 / a.speed - (time.time() - paced_from[1])
            if ahead > 0:
                time.sleep(ahead)
        if st is None:
            continue
        t1, t2 = st["teams"]
        line = "%6d %s per %d relogio %5d  %s %d x %d %s%s" % (
            f, SP.KH.fmt_time(f), st["period"], st["clock"], t1["name"] or t1["id"], t1["goals"], t2["goals"],
            t2["name"] or t2["id"], "  FIM" if st["final"] else "")
        if a.dry:
            print(line, flush=True)
            continue
        replies = [post(a.url, a.key, st, players, r, room) for r in (players if a.both else players[:1])]
        print(line, " -> ", " | ".join(replies), flush=True)
    print("fim em %.0f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
