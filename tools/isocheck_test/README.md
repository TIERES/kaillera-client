# isocheck_test

Testa o aviso de ISO diferente na sala do Kaillera (`common/n02_isocheck.*`, o
`kaillera_ui.cpp` e o `kailleraContentIdForGame` do retroarch-k3).

Caso real (2026-11-16): warleyvieira x luiz jogaram a "Euro vs Copa America
2004.bin" com arquivos diferentes, do mesmo tamanho e com outros bytes. A RAM
dos dois PCs divergiu desde o primeiro segundo e o M. Card Online do outro
jogador veio em branco. O Ao vivo do WE Camp recebeu duas meias partidas que
ele nunca mostra.

Como funciona: o RetroArch calcula o "CRC32:TAMANHO" da ISO do jogo da sala
(o mesmo id da verificação de sincronia e do placar ao vivo) assim que o
jogador entra. Cada DLL anuncia o seu id no chat da sala (`[ISO] <id>`, linha
escondida) e repete para quem entra depois.

Com ids diferentes:
- uma linha vermelha aparece no chat de todos;
- no Start do host, a popup aparece em todos os PCs (`[ISO] aviso`), e a do
  host pergunta "Iniciar a partida mesmo assim?";
- com 3 ou mais jogadores, quem fica fora do grupo maior é apontado como o da
  ISO diferente. Com 2 (ou empate), o aviso diz que não dá para saber qual
  está certa.

Com a mesma ISO, nada aparece. Também não há comparação com um DLL antigo,
outro emulador ou um RetroArch sem a função.

## Testes

- `build.cmd` + `isocheck_test.exe`: a lógica (grupos, maioria, textos do chat
  e da popup, acentos). Os textos das popups vão para `popups.txt`;
  `isocheck_test.exe popup` mostra as caixas na tela.
- `build_fake.cmd` + `run_room_test.py`: de ponta a ponta com o DLL de verdade.
  Cada jogador é um `fake_room.exe` (um "RetroArch" falso, com um id de ISO
  fixo) numa pasta própria, contra um EmuLinker-K local que o script sobe a cada
  cenário. Cenários:
  - `tres`: 3 jogadores e 1 diferente que calcula devagar; o host responde
    "não" e depois "sim";
  - `iguais`: nada aparece e o jogo começa;
  - `dois`: o caso de empate;
  - `antigo`: o outro jogador não tem o callback.
- `run_real_retroarch.py`: o `retroarch.exe` TIERES de verdade entra na sala
  de um `fake_room.exe`. Confere o id que o fork calcula da ISO do Histórico. O
  RetroArch roda numa CÓPIA de instalação, com o DLL novo, um
  `content_history.lpl` só com o jogo e um `n02.ini` apontando para
  `127.0.0.1:27888`.

O EmuLinker local: copie `emulinker/build/libs/emulinker-k-*.jar` para
`<pasta>\lib` e os `.cfg`/`.properties` de `release\` para `<pasta>\conf`, com
`masterList.touchKaillera=false` e `masterList.touchEmulinker=false` (assim
não aparece na lista mestre). Ele precisa de Java 17+, por exemplo um Temurin
17 JRE em zip.

```
python run_room_test.py --saida <pasta> --elk <pasta do EmuLinker> --java <java.exe>
python run_real_retroarch.py --ra <cópia do RetroArch> --saida <pasta> --elk <...> --java <...>
```

Os n02.ini dos testes apontam o WE Camp para `127.0.0.1:9`, então nada fala
com o site de produção. As janelas abrem de verdade na tela e se operam sozinhas.

## Resultado (2026-11-16)

Todos os cenários passaram. No `run_real_retroarch.py`, o RetroArch calculou
`BA9BDE21:1C473F60` para a Euro local. É o mesmo CRC do arquivo e o mesmo da
ISO "EUROCOPA 2004" do portal.

Observação: quando outro processo está em primeiro plano, o Windows às vezes
não deixa a caixa ficar "sempre visível". O `SetWindowPos(HWND_TOPMOST)`
devolve OK e não muda nada. A caixa reforça o pedido a cada 250 ms e fica por
cima assim que o RetroArch volta para frente (por exemplo, quando o jogo abre).
