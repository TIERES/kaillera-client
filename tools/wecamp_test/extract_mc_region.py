"""Copia o trecho real do M. Card online do kailleraclient.cpp (da struct
McSession até o kailleraMemcardFinish) para o mc_region_real.inc, que o
mc_session_test.cpp inclui - assim o teste roda o código que vai no DLL."""
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
src = (here.parents[1] / "kailleraclient.cpp").read_bytes().decode("utf-8").replace("\r\n", "\n")
start = src.find("\t// One live match's online cards")
finish = src.find("\tvoid KAILLERA_DLLEXP kailleraMemcardFinish(){")
end = src.find("\n\t}\n", finish) + len("\n\t}\n")
if start < 0 or finish < 0 or end <= finish:
    sys.exit("trecho do M. Card nao encontrado no kailleraclient.cpp")
(here / "mc_region_real.inc").write_text(src[start:end], encoding="utf-8")
print(f"kailleraclient.cpp linhas {src[:start].count(chr(10)) + 1}-{src[:end].count(chr(10))} -> mc_region_real.inc")
