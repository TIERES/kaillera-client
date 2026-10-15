"""Teste de ponta a ponta do atualizador do pacote RetroArch TIERES
(common/n02_update.cpp), sem tocar no WE Camp de produção:

- compila o kailleraclient.dll de verdade duas vezes, como v.TIERES.0.98 e
  v.TIERES.0.99, e o retroarch.exe falso (fake_retroarch.c) em três versões:
  v1 e v2 (com a marca do TIERES) e "outro" (sem a marca);
- sobe um wg-camp local (as rotas /updates/* de verdade) e publica nele o
  pacote com o deploy/publish_pacote.sh de verdade (Git Bash);
- monta uma pasta "RetroArch" (com espaço e acento no caminho) com um n02.ini
  que aponta o DLL para o servidor local, roda o exe falso e confere o
  teste.log, os arquivos e as sobras (.new/.old).

    py -3 run_test.py --wgcamp E:\\Projetos\\wg-camp [--sem-build] [--cenario A]

Abre janelas de verdade (as caixas de mensagem do DLL e a de progresso),
respondidas sozinhas pelo exe falso.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

AQUI = Path(__file__).resolve().parent
REPO = AQUI.parents[1]
GIT_BASH = r"C:\Program Files\Git\bin\bash.exe"
VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"

RESULTADOS = []


def ok(cond, msg):
    RESULTADOS.append((bool(cond), msg))
    print(("  OK   " if cond else "  FALHA ") + msg)
    return cond


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


# --- build ------------------------------------------------------------------

def vs_paths():
    install = subprocess.check_output([VSWHERE, "-all", "-products", "*",
                                       "-property", "installationPath"],
                                      text=True).strip().splitlines()[0]
    version = subprocess.check_output([VSWHERE, "-all", "-products", "*",
                                       "-property", "installationVersion"],
                                      text=True).strip().splitlines()[0]
    toolset = {"18": "v145", "17": "v143", "16": "v142"}[version.split(".")[0]]
    msbuild = Path(install) / "MSBuild" / "Current" / "Bin" / "MSBuild.exe"
    vcvars = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return msbuild, vcvars, toolset


def build_dll(version, out):
    # De uma cópia do código: o n02p.vcxproj fixa os .obj em .\x64\Release
    # (ObjectFileName), e duas versões ali se misturariam - e sujariam o
    # build normal do repositório.
    msbuild, _, toolset = vs_paths()
    src = out / "src"
    if src.exists():
        shutil.rmtree(src)
    shutil.copytree(REPO, src, ignore=shutil.ignore_patterns(".git", ".vs", "x64", "Release", "Debug", "tools", "*.dll"))
    cmd = [str(msbuild), str(src / "n02p.vcxproj"), "/nologo", "/v:minimal", "/m",
           "/p:Configuration=Release", "/p:Platform=x64", f"/p:PlatformToolset={toolset}",
           "/p:WindowsTargetPlatformVersion=10.0", f"/p:ExtraDefines=GIT_REVISION={version}"]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, cwd=str(src))
    shutil.copyfile(src / "x64" / "Release" / "kailleraclient.dll", out / "kailleraclient.dll")
    return out / "kailleraclient.dll"


def build_fake(name, defines, out):
    _, vcvars, _ = vs_paths()
    out.mkdir(parents=True, exist_ok=True)
    flags = " ".join(f"/D{d}" for d in defines)
    script = out / f"build_{name}.cmd"
    script.write_text(
        f'@call "{vcvars}" >nul\r\n'
        f'cd /d "{out}"\r\n'
        f'cl /nologo /O1 /D_CRT_SECURE_NO_WARNINGS {flags} "{AQUI / "fake_retroarch.c"}" '
        f'/Fe{name}.exe /Fo{name}.obj user32.lib gdi32.lib >nul\r\n', encoding="ascii")
    subprocess.run(["cmd", "/c", str(script)], check=True)
    return out / f"{name}.exe"


# --- servidor local -----------------------------------------------------------

class Servidor:
    def __init__(self, wgcamp, raiz, porta=0):
        sys.path.insert(0, str(wgcamp))
        from app import create_app
        from werkzeug.serving import make_server
        self.raiz = raiz
        self.updates = raiz / "updates"
        self.pacote = raiz / "pacote"
        self.wgcamp = Path(wgcamp)
        self.lento = 0.0  # segundos por pedaço de 64 KB dos arquivos do pacote
        app = create_app({
            "TESTING": True,
            "SECRET_KEY": "teste",
            "DATABASE": str(raiz / "arena17.db"),
            "DOWNLOADS_DIR": str(raiz / "downloads"),
            "UPLOAD_TMP_DIR": str(raiz / "tmp"),
            "LIVE_DIR": str(raiz / "live"),
            "MEMCARDS_DIR": str(raiz / "memcards"),
            "REPLAY_BACKUPS_DIR": str(raiz / "replay-backups"),
            "UPDATES_DIR": str(self.updates),
        })
        with app.app_context():
            from app.db import init_db
            init_db()
        self.pedidos = []
        inner = app.wsgi_app
        servidor = self

        def middleware(environ, start_response):
            path = environ.get("PATH_INFO", "")
            servidor.pedidos.append(path)
            result = inner(environ, start_response)
            if servidor.lento and path.startswith("/updates/pacote/arquivos/"):
                def devagar():
                    for parte in result:
                        for i in range(0, len(parte), 65536):
                            time.sleep(servidor.lento)
                            yield parte[i:i + 65536]
                    if hasattr(result, "close"):
                        result.close()
                return devagar()
            return result

        app.wsgi_app = middleware
        # http.server pergunta o nome da máquina (getfqdn), que aqui tem acento
        # em cp1252 e quebra o decode - não precisamos dele.
        import socket
        socket.getfqdn = lambda name="": name
        self.http = make_server("127.0.0.1", porta, app, threaded=True)
        self.porta = self.http.server_port
        threading.Thread(target=self.http.serve_forever, daemon=True).start()

    def publicar_dll(self, versao, dll):
        self.updates.mkdir(parents=True, exist_ok=True)
        (self.updates / "version.txt").write_text(versao, encoding="utf-8")
        shutil.copyfile(dll, self.updates / "kailleraclient-x64.dll")

    def publicar_pacote(self, versao, arquivos, nome="RetroArch-teste"):
        """arquivos: {caminho no pacote: arquivo local}. Usa o publish_pacote.sh."""
        trabalho = Path(tempfile.mkdtemp(dir=self.raiz))
        novos = trabalho / "novos"
        novos.mkdir()
        linhas = [f"versao\t{versao}", f"pacote\t{nome}"]
        for caminho, origem in sorted(arquivos.items()):
            dados = Path(origem).read_bytes()
            sha = hashlib.sha256(dados).hexdigest()
            (novos / sha).write_bytes(dados)
            linhas.append(f"arquivo\t{sha}\t{len(dados)}\t{caminho}")
        manifesto = trabalho / "x64.txt"
        manifesto.write_bytes(("\n".join(linhas) + "\n").encode("utf-8"))
        env = dict(os.environ, ARENA17_PACOTE_DIR=self.pacote.as_posix())
        r = subprocess.run([GIT_BASH, str(self.wgcamp / "deploy" / "publish_pacote.sh"),
                            manifesto.as_posix(), novos.as_posix()],
                           env=env, capture_output=True, text=True, encoding="utf-8")
        if r.returncode != 0:
            raise RuntimeError(f"publish_pacote.sh falhou ({r.returncode}):\n{r.stdout}{r.stderr}")
        shutil.rmtree(trabalho)
        return r.stdout


# --- pasta do RetroArch -------------------------------------------------------

def montar_pasta(raiz, nome, arquivos, porta):
    pasta = raiz / nome / "RetroArch Teste ção"
    if pasta.exists():
        shutil.rmtree(pasta)
    (pasta / "cores").mkdir(parents=True)
    for caminho, origem in arquivos.items():
        destino = pasta / caminho.replace("/", "\\")
        destino.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origem, destino)
    (pasta / "n02.ini").write_text(
        f"[SC]\r\nWECAMP_HOST=127.0.0.1\r\nWECAMP_PORT={porta}\r\nWECAMP_HTTP=1\r\n", encoding="ascii")
    return pasta


def rodar(pasta, args=(), env_extra=None, esperar_relancamento=False, timeout=60):
    env = dict(os.environ)
    env.update(env_extra or {})
    log = pasta / "teste.log"
    p = subprocess.Popen([str(pasta / "retroarch.exe"), *args], cwd=str(pasta), env=env)
    p.wait(timeout=timeout)
    if esperar_relancamento:
        fim = time.time() + timeout
        while time.time() < fim:
            texto = log.read_text(encoding="utf-8") if log.exists() else ""
            if texto.count("start ") >= 2 and texto.rstrip().endswith("exit"):
                break
            time.sleep(0.2)
        time.sleep(0.5)
    return log.read_text(encoding="utf-8") if log.exists() else ""


def sobras(pasta):
    return sorted(str(p.relative_to(pasta)) for p in pasta.rglob("*")
                  if p.suffix in (".new", ".old") or ".old" in p.suffixes)


def arquivos_iguais(pasta, esperado):
    return all(sha256(pasta / c.replace("/", "\\")) == sha256(o) for c, o in esperado.items())


# --- cenários -----------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wgcamp", required=True)
    parser.add_argument("--sem-build", action="store_true")
    parser.add_argument("--cenario", default="ABCDEFGHI")
    args = parser.parse_args()

    trabalho = Path(os.environ.get("ATUALIZADOR_TESTE_DIR", tempfile.gettempdir())) / "atualizador_teste"
    build = trabalho / "build"
    print("Compilando...")
    if not args.sem_build:  # os dois DLLs (os exes falsos sempre: são rápidos)
        build_dll("v.TIERES.0.98", build / "dll098")
        build_dll("v.TIERES.0.99", build / "dll099")
    build_fake("v1", ['FAKE_VERSION=\\"v1\\"', "FAKE_TIERES"], build / "fake")
    build_fake("v2", ['FAKE_VERSION=\\"v2\\"', "FAKE_TIERES"], build / "fake")
    build_fake("outro", ['FAKE_VERSION=\\"outro\\"'], build / "fake")
    dll098 = build / "dll098" / "kailleraclient.dll"
    dll099 = build / "dll099" / "kailleraclient.dll"
    v1, v2, outro = (build / "fake" / f"{n}.exe" for n in ("v1", "v2", "outro"))
    core1, core2, lck = build / "core1.dll", build / "core2.dll", build / "vazio.lck"
    core1.write_bytes(os.urandom(300_000))
    core2.write_bytes(os.urandom(2_500_000))
    lck.write_bytes(b"")

    raiz = trabalho / "rodada"
    if raiz.exists():
        shutil.rmtree(raiz)
    raiz.mkdir(parents=True)
    srv = Servidor(args.wgcamp, raiz / "servidor")
    print(f"Servidor local em 127.0.0.1:{srv.porta}")

    novo = {"retroarch.exe": v2, "kailleraclient.dll": dll099,
            "cores/pcsx_rearmed_libretro.dll": core2, "cores/pcsx_rearmed_libretro.dll.lck": lck}
    srv.publicar_pacote("v.TIERES.0.99", novo)
    srv.publicar_dll("v.TIERES.0.99", dll099)
    ok((srv.pacote / "x64.txt").exists() and len(list((srv.pacote / "arquivos").iterdir())) == 4,
       "publish_pacote.sh publicou o manifesto e 4 arquivos")

    argumentos = ["--menu", "arg com espaço", "ção"]

    if "A" in args.cenario:
        print("A) versao nova: troca exe, DLL, nucleo, cria o .lck e reinicia com os mesmos argumentos")
        pasta = montar_pasta(raiz, "A", {"retroarch.exe": v1, "kailleraclient.dll": dll098,
                                         "cores/pcsx_rearmed_libretro.dll": core1}, srv.porta)
        # Kaillera "carregado" por 2,5 s: os primeiros WM_CLOSE do relançador são recusados
        log = rodar(pasta, argumentos, {"FAKE_DIALOG": "1", "FAKE_KAILLERA_MS": "2500"}, esperar_relancamento=True)
        print("    " + log.replace("\n", "\n    "))
        ok("Atual: v.TIERES.0.98" in log and "Nova:  v.TIERES.0.99" in log, "A: aviso com as duas versoes")
        ok("retroarch.exe" in log.split("msgbox")[1] and "pcsx_rearmed_libretro.dll.lck" in log, "A: aviso lista os arquivos")
        ok("Atualização instalada!" in log, "A: 'Atualização instalada!'")
        ok("dialog returned" in log, "A: kailleraSelectServerDialog voltou sem abrir o Kaillera")
        ok("close refused (kaillera open)" in log and "wm_close" in log.splitlines(),
           "A: fechamento recusado com o Kaillera carregado, aceito depois (pedido de novo)")
        ok("start v2" in log, "A: o exe novo (v2) foi aberto")
        segundo = log[log.find("start v2"):]
        ok("args=[--menu \"arg com espaço\" ção]" in segundo, "A: mesmos argumentos no relancamento")
        ok("msgbox Atualiza" not in segundo and "msgbox N02 v.TIERES.0.99" in segundo,
           "A: o exe novo nao pergunta de novo e abre o Kaillera normalmente")
        ok(arquivos_iguais(pasta, novo), "A: todos os arquivos iguais aos do servidor")
        ok(sobras(pasta) == [], f"A: sem .new/.old ({sobras(pasta)})")

    if "B" in args.cenario:
        print("B) mesma versao do DLL, exe antigo: baixa so o exe, sem o DLL")
        pasta = montar_pasta(raiz, "B", {"retroarch.exe": v1, "kailleraclient.dll": dll099,
                                         "cores/pcsx_rearmed_libretro.dll": core2,
                                         "cores/pcsx_rearmed_libretro.dll.lck": lck}, srv.porta)
        srv.pedidos.clear()
        log = rodar(pasta, [], {"FAKE_DIALOG": "1"}, esperar_relancamento=True)
        print("    " + log.replace("\n", "\n    "))
        ok("Há arquivos novos do RetroArch TIERES v.TIERES.0.99" in log, "B: aviso de arquivos novos da mesma versao")
        baixados = [p for p in srv.pedidos if p.startswith("/updates/pacote/arquivos/")]
        ok(baixados == ["/updates/pacote/arquivos/" + sha256(v2)], f"B: baixou so o exe ({len(baixados)} arquivo(s))")
        ok("start v2" in log and arquivos_iguais(pasta, novo), "B: reiniciou no exe novo, tudo igual ao servidor")
        ok(sobras(pasta) == [], f"B: sem sobras ({sobras(pasta)})")

    if "C" in args.cenario:
        print("C) tudo em dia: nenhum aviso")
        pasta = montar_pasta(raiz, "C", novo, srv.porta)
        srv.pedidos.clear()
        log = rodar(pasta)
        ok("msgbox" not in log and "no wm_close (timer)" in log, "C: nenhum aviso, nada fechou")
        ok("/updates/latest" in srv.pedidos, "C: a checagem so-DLL ainda roda (nada novo no pacote)")

    if "D" in args.cenario:
        print("D) build de testador mais nova que a publicada: nenhum aviso")
        pasta = montar_pasta(raiz, "D", {"retroarch.exe": v1, "kailleraclient.dll": dll099,
                                         "cores/pcsx_rearmed_libretro.dll": core1}, srv.porta)
        srv.publicar_pacote("v.TIERES.0.98", {"retroarch.exe": v2, "kailleraclient.dll": dll098})
        srv.publicar_dll("v.TIERES.0.98", dll098)
        log = rodar(pasta)
        ok("msgbox" not in log, "D: nenhum aviso")
        ok(sha256(pasta / "retroarch.exe") == sha256(v1), "D: exe intocado")
        srv.publicar_pacote("v.TIERES.0.99", novo)
        srv.publicar_dll("v.TIERES.0.99", dll099)

    if "E" in args.cenario:
        print("E) outro RetroArch (sem a marca TIERES): so o DLL, como antes")
        pasta = montar_pasta(raiz, "E", {"retroarch.exe": outro, "kailleraclient.dll": dll098}, srv.porta)
        log = rodar(pasta)
        print("    " + log.replace("\n", "\n    "))
        ok("Uma nova versao do Kaillera" in log, "E: aviso antigo (so DLL)")
        ok(sha256(pasta / "retroarch.exe") == sha256(outro), "E: exe intocado")
        ok(sha256(pasta / "kailleraclient.dll") == sha256(dll099), "E: DLL trocado")
        ok("wm_close" not in log.splitlines() and not (pasta / "cores" / "pcsx_rearmed_libretro.dll").exists(),
           "E: sem reinicio e sem nucleo baixado")

    if "F" in args.cenario:
        print("F) arquivo corrompido no servidor: nada muda")
        pasta = montar_pasta(raiz, "F", {"retroarch.exe": v1, "kailleraclient.dll": dll098,
                                         "cores/pcsx_rearmed_libretro.dll": core1}, srv.porta)
        alvo = srv.pacote / "arquivos" / sha256(core2)
        original = alvo.read_bytes()
        alvo.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        log = rodar(pasta)
        alvo.write_bytes(original)
        print("    " + log.replace("\n", "\n    "))
        ok("chegou incompleto ou corrompido" in log, "F: mensagem de arquivo corrompido")
        ok(sha256(pasta / "retroarch.exe") == sha256(v1) and sha256(pasta / "kailleraclient.dll") == sha256(dll098),
           "F: exe e DLL intocados")
        ok(sobras(pasta) == [], f"F: sem sobras ({sobras(pasta)})")

    if "G" in args.cenario:
        print("G) cancelar no meio do download (servidor lento)")
        pasta = montar_pasta(raiz, "G", {"retroarch.exe": v1, "kailleraclient.dll": dll098,
                                         "cores/pcsx_rearmed_libretro.dll": core1}, srv.porta)
        srv.lento = 0.05
        log = rodar(pasta, [], {"FAKE_CANCEL": "1"})
        srv.lento = 0.0
        print("    " + "\n    ".join(log.splitlines()[:12]))
        ok("clicking Cancelar" in log, "G: janela de progresso apareceu e Cancelar foi clicado")
        ok("Atualização instalada" not in log and "Não foi possível" not in log, "G: sem mensagem depois de cancelar")
        ok(sha256(pasta / "retroarch.exe") == sha256(v1), "G: exe intocado")
        ok(sobras(pasta) == [], f"G: sem sobras ({sobras(pasta)})")

    if "H" in args.cenario:
        print("H) progresso: servidor lento, janela mostra o arquivo e os MB")
        pasta = montar_pasta(raiz, "H", {"retroarch.exe": v1, "kailleraclient.dll": dll098,
                                         "cores/pcsx_rearmed_libretro.dll": core1}, srv.porta)
        srv.lento = float(os.environ.get("ATUALIZADOR_LENTO", "0.01"))
        log = rodar(pasta, [], {"FAKE_DIALOG": "1"}, esperar_relancamento=True, timeout=120)
        srv.lento = 0.0
        progresso = [l for l in log.splitlines() if l.startswith("progress")]
        print("    " + "\n    ".join(progresso[:3] + ["..."] + progresso[-2:]))
        ok(any("Baixando cores/pcsx_rearmed_libretro.dll (1 de 4)" in l for l in progresso)
           and any("de 4)" in l and " MB de " in l for l in progresso), "H: texto 'Baixando <arquivo> (n de 4) / x MB de y MB'")
        ok("start v2" in log and arquivos_iguais(pasta, novo), "H: atualizou e reiniciou")

    if "I" in args.cenario:
        print("I) um arquivo preso por outro programa: desfaz tudo")
        pasta = montar_pasta(raiz, "I", {"retroarch.exe": v1, "kailleraclient.dll": dll098,
                                         "cores/pcsx_rearmed_libretro.dll": core1}, srv.porta)
        # aberto sem FILE_SHARE_DELETE (como o open() do Python): nao da para renomear
        preso = open(pasta / "retroarch.exe", "rb")
        try:
            log = rodar(pasta)
        finally:
            preso.close()
        print("    " + log.replace("\n", "\n    "))
        ok("Não foi possível instalar a atualização: o arquivo retroarch.exe" in log, "I: mensagem cita o retroarch.exe")
        ok(sha256(pasta / "retroarch.exe") == sha256(v1) and sha256(pasta / "kailleraclient.dll") == sha256(dll098)
           and sha256(pasta / "cores" / "pcsx_rearmed_libretro.dll") == sha256(core1), "I: tudo voltou ao original")
        ok(not (pasta / "cores" / "pcsx_rearmed_libretro.dll.lck").exists(), "I: o .lck novo foi desfeito")
        ok(sobras(pasta) == [], f"I: sem sobras ({sobras(pasta)})")
        ok("wm_close" not in log.splitlines(), "I: nao reiniciou")

    srv.http.shutdown()
    falhas = [m for passou, m in RESULTADOS if not passou]
    print(f"\n{len(RESULTADOS) - len(falhas)}/{len(RESULTADOS)} verificacoes OK")
    for m in falhas:
        print("  FALHA: " + m)
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
