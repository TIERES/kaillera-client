# atualizador_test

Testa a atualização do pacote completo do RetroArch TIERES (`common/n02_update.cpp`):
dentro do `retroarch.exe` TIERES, o DLL baixa do WE Camp os arquivos do light que
mudaram (`retroarch.exe`, `kailleraclient.dll`, núcleo), troca todos e reinicia o
RetroArch. Nada aqui fala com o WE Camp de produção.

- `fake_retroarch.c`: um `retroarch.exe` falso que faz com o DLL o que o fork faz ao
  abrir o Kaillera (janela de classe `RetroArch`, `kailleraInit`, o diálogo numa
  thread, recusa o fechamento enquanto o Kaillera está carregado), responde sozinho
  às caixas do DLL e anota tudo em `teste.log`.
- `run_test.py`: compila o DLL de verdade como `v.TIERES.0.98` e `v.TIERES.0.99`
  (de uma cópia do código, porque o `n02p.vcxproj` fixa os `.obj` em `x64\Release`),
  sobe um wg-camp local, publica o pacote com o `deploy/publish_pacote.sh` de
  verdade e roda os cenários numa pasta com espaço e acento no caminho:
  - A: versão nova: troca exe, DLL e núcleo, cria o `.lck`, fecha (pedindo de novo
    enquanto o Kaillera ainda está carregado) e reabre com os mesmos argumentos;
  - B: mesma versão do DLL com exe antigo: baixa só o exe;
  - C: tudo em dia: nenhum aviso;
  - D: build de testador mais nova que a publicada: nenhum aviso;
  - E: outro RetroArch (sem a marca `FFW.TIERES`): só o DLL, como antes;
  - F: arquivo corrompido no servidor: nada muda;
  - G: Cancelar no meio do download: nada muda;
  - H: a janela de progresso mostra arquivo, "n de 4" e MB;
  - I: um arquivo preso por outro programa: desfaz tudo.
- `servidor_local.py`: um wg-camp local com um light publicado, para testar à mão
  com o RetroArch TIERES de verdade ou a task de publicação (`--simular`).

## Como rodar

Com o Python do wg-camp (precisa do Flask):

```
E:\Projetos\wg-camp\.venv\Scripts\python.exe run_test.py --wgcamp E:\Projetos\wg-camp
```

`--sem-build` reaproveita os dois DLLs já compilados; `--cenario AB` roda só alguns.
`FAKE_SHOT_DIR=<pasta>` salva imagens das caixas e da janela de progresso. Abre
janelas de verdade na tela (respondidas sozinhas).

## Teste à mão com o RetroArch TIERES

Numa cópia da pasta do RetroArch (nunca a que você usa), com o `kailleraclient.dll`
de uma versão menor e este `n02.ini`:

```
[SC]
WECAMP_HOST=127.0.0.1
WECAMP_PORT=5056
WECAMP_HTTP=1
```

```
E:\Projetos\wg-camp\.venv\Scripts\python.exe servidor_local.py --wgcamp E:\Projetos\wg-camp ^
    --porta 5056 --light X.light.zip --versao v.TIERES.0.99 --dll kailleraclient.dll
```

Abra o RetroArch e o Kaillera: o aviso aparece, o download baixa só o que mudou e o
RetroArch fecha e abre de novo sozinho.

## Resultado (2026-10-15)

37 verificações, todas OK. Também com o `retroarch.exe` TIERES 0.5 de verdade (o
Kaillera aberto pelo `WM_COMMAND` `ID_KAILLERA` do menu do fork): aviso, 17,4 MB
baixados, o RetroArch fechou normalmente (código 0), reabriu sozinho com o exe e o
DLL novos, e o Kaillera abriu na versão nova; os `.old` foram apagados na abertura
seguinte do Kaillera.
