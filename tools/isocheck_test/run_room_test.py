"""Teste de ponta a ponta do aviso de ISO diferente na sala do Kaillera.

Precisa de um EmuLinker-K rodando nesta maquina (--porta, padrao 27888) e do
DLL compilado (x64\\Release\\kailleraclient.dll). Cada jogador e um
fake_room.exe numa pasta propria com o DLL e um n02.ini (nick, o servidor
local, WE Camp desligado). Abre janelas de verdade na tela, que se operam
sozinhas. Os logs ficam em <saida>\\<cenario>\\<jogador>\\fake.log.

    python run_room_test.py --saida <pasta> --elk <pasta do EmuLinker> --java <java.exe 17+>
        [--cenario tres,iguais,dois,antigo]

Com --elk/--java o teste sobe um EmuLinker-K novo para cada cenario (pasta com
lib\\emulinker-k-*.jar e conf\\ com masterList.touch*=false) e o derruba no fim.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

AQUI = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(AQUI, "..", "..", "x64", "Release", "kailleraclient.dll")
OFICIAL = "BA9BDE21:1C473F60"
OUTRA = "73E91CFE:1C473F60"

# nick, papel, iso, atraso do hash (ms)
CENARIOS = {
    # 3 jogadores, um com a ISO diferente (que demora a calcular): o host diz
    # "nao" e depois "sim".
    "tres": dict(jogadores=[("hostA", "host", OFICIAL, 800), ("luizB", "join", OFICIAL, 800),
                            ("warleyC", "join", OUTRA, 4000)], respostas="nao,sim"),
    # 2 jogadores com a mesma ISO: nada aparece, o jogo comeca.
    "iguais": dict(jogadores=[("hostA", "host", OFICIAL, 800), ("luizB", "join", OFICIAL, 800)], respostas="nao"),
    # 2 jogadores com ISOs diferentes: ninguem sabe qual esta certa.
    "dois": dict(jogadores=[("warleyA", "host", OUTRA, 800), ("luizB", "join", OFICIAL, 800)], respostas="nao"),
    # o outro jogador sem o callback (outro emulador / RetroArch antigo): nao compara.
    "antigo": dict(jogadores=[("hostA", "host", OFICIAL, 800), ("velhoB", "join", "none", 0)], respostas="nao"),
}


def ini(nick, porta):
    return ("[SC]\r\nUSRN=%s\r\nSLInit=1\r\nSLC=1\r\nSLS1=Teste local ISO\r\nSLH1=127.0.0.1:%d\r\n"
            "WECAMP_HOST=127.0.0.1\r\nWECAMP_PORT=9\r\nWECAMP_HTTP=1\r\nKAILLERA_STREAM_LIVE=0\r\n") % (nick, porta)


def subir_servidor(elk, java, log):
    jar = [f for f in os.listdir(os.path.join(elk, "lib")) if f.endswith(".jar")][0]
    out = open(log, "wb")
    p = subprocess.Popen([java, "-Xms64m", "-Xmx256m", "-XX:+UseSerialGC", "-cp", "conf;lib/" + jar,
                          "org.emulinker.kaillera.pico.ServerMainKt"], cwd=elk, stdout=out, stderr=subprocess.STDOUT)
    for _ in range(150):
        time.sleep(0.2)
        with open(log, "rb") as f:
            if b"Ready to accept connections" in f.read():
                return p
    p.kill()
    raise SystemExit("EmuLinker nao subiu - veja " + log)


def rodar(nome, cenario, saida, porta, run_ms, elk=None, java=None):
    base = os.path.join(saida, nome)
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    servidor = subir_servidor(elk, java, os.path.join(base, "emulinker.log")) if elk else None
    procs = []
    n = len(cenario["jogadores"])
    for i, (nick, papel, iso, atraso) in enumerate(cenario["jogadores"]):
        pasta = os.path.join(base, nick)
        os.makedirs(pasta)
        shutil.copy(os.path.join(AQUI, "fake_room.exe"), pasta)
        shutil.copy(DLL, pasta)
        with open(os.path.join(pasta, "n02.ini"), "w", newline="") as f:
            f.write(ini(nick, porta))
        env = dict(os.environ, FAKE_ROLE=papel, FAKE_ISO=iso, FAKE_DELAY_MS=str(atraso), FAKE_PLAYERS=str(n),
                   FAKE_SETTLE_MS="6000", FAKE_ANSWERS=cenario["respostas"], FAKE_RUN_MS=str(run_ms - i * 4000))
        procs.append(subprocess.Popen([os.path.join(pasta, "fake_room.exe")], cwd=pasta, env=env))
        time.sleep(4)
    for p in procs:
        try:
            p.wait(timeout=run_ms / 1000 + 30)
        except subprocess.TimeoutExpired:
            p.kill()
    if servidor:
        servidor.kill()
    for nick, *_ in cenario["jogadores"]:
        print("=" * 20, nome, nick)
        with open(os.path.join(base, nick, "fake.log"), encoding="utf-8", errors="replace") as f:
            print(f.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--saida", required=True)
    ap.add_argument("--porta", type=int, default=27888)
    ap.add_argument("--cenario", default="tres,iguais,dois,antigo")
    ap.add_argument("--run-ms", type=int, default=50000)
    ap.add_argument("--elk")
    ap.add_argument("--java")
    a = ap.parse_args()
    for nome in a.cenario.split(","):
        rodar(nome, CENARIOS[nome], a.saida, a.porta, a.run_ms, a.elk, a.java)


if __name__ == "__main__":
    sys.exit(main())
