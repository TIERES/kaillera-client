"""wg-camp local com um pacote publicado, para testar o atualizador à mão
(o RetroArch TIERES de verdade) ou a task de publicação (--simular):

    py -3 servidor_local.py --wgcamp E:\\Projetos\\wg-camp --porta 5056 \\
        --light X.light.zip --versao v.TIERES.0.99 [--dll kailleraclient.dll]

Publica o conteúdo do zip como pacote (com o deploy/publish_pacote.sh de
verdade) e, com --dll, também o DLL da atualização só-DLL. Fica rodando até
Ctrl+C. Na pasta do RetroArch de teste, o n02.ini aponta o DLL para cá:

    [SC]
    WECAMP_HOST=127.0.0.1
    WECAMP_PORT=5056
    WECAMP_HTTP=1
"""
import argparse
import tempfile
import time
import zipfile
from pathlib import Path

from run_test import Servidor


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wgcamp", required=True)
    parser.add_argument("--porta", type=int, default=5056)
    parser.add_argument("--light", required=True)
    parser.add_argument("--versao", required=True)
    parser.add_argument("--dll")
    args = parser.parse_args()

    raiz = Path(tempfile.mkdtemp(prefix="atualizador-servidor-"))
    srv = Servidor(args.wgcamp, raiz, porta=args.porta)
    extraido = raiz / "light"
    with zipfile.ZipFile(args.light) as z:
        z.extractall(extraido)
    arquivos = {p.relative_to(extraido).as_posix(): p for p in extraido.rglob("*") if p.is_file()}
    print(srv.publicar_pacote(args.versao, arquivos, nome=Path(args.light).name.removesuffix(".light.zip")))
    if args.dll:
        srv.publicar_dll(args.versao, args.dll)
    print(f"Servidor em http://127.0.0.1:{srv.porta} (pasta {raiz}) - Ctrl+C para parar", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
