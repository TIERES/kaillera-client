# broadcaster

O broadcaster assiste uma partida do WE Camp pelo Watch Live e manda o jogo, com
imagem e som, para o YouTube. Fora isso, ele também pode gravar num arquivo.

Ele é feito de duas peças:

- o `retroarch.exe` do branch `feature/wecamp-broadcaster` do `retroarch-k3-ffw`;
- a `kailleraclient.dll` deste branch, compilada com `build.bat`.

Roda sem janela, sem placa de som e sem controle. Funciona até por SSH, sem
ninguém logado na máquina.

## Como funciona

1. O `start-broadcast.cmd` define as variáveis `WECAMP_*` e abre o RetroArch já
   com a ISO da partida.
2. No primeiro frame, o fork carrega a DLL.
3. A DLL roda em modo headless: sem janela, ela entra no Watch Live da sessão,
   pelo módulo de Playback.
4. Quando o jogo do Watch Live começa, o driver de gravação `wecamp` do fork
   (`record/drivers/record_wecamp.c`) abre o `ffmpeg.exe`. O ffmpeg recebe as
   imagens do core e o áudio por dois named pipes e envia H.264/AAC em FLV para
   a URL, que pode ser RTMP ou um arquivo.
5. O ffmpeg é alimentado no ritmo do relógio. Se o input ao vivo atrasar, a
   transmissão segue com a última imagem e silêncio, em vez de travar.
6. Quando a sessão termina, o fork fecha o ffmpeg com calma, para a live
   terminar direito, e o RetroArch sai.
7. Se a sessão não começar em 120 s (`WECAMP_BROADCASTER_START_TIMEOUT`), o
   RetroArch também sai.

## Agente (`agent\`): o que transmitir, YouTube e ISOs

Quem decide o que vai para o YouTube é o wg-camp (`app/broadcaster.py`):

- **Jogos de campeonato:** entram sozinhos (ajuste em `/admin/transmissoes`).
- **Outras partidas:** entram pelo botão **Transmitir** do card no `/ao-vivo`, só para admin.

O `agent\agent.ps1` roda como tarefa agendada (SYSTEM, na inicialização). A cada
5 s ele faz `POST /api/broadcaster/poll` com o token deste broadcaster.

**Para começar uma partida**, o agente:

1. **Garante a ISO:**
   - procura a ISO do cadastro (campeonato ou amistosos) em `games\`, pelo MD5;
   - se faltar, baixa do archive.org com `curl.exe`;
   - liga ao nome de arquivo da sala com um hard link.
2. **Cria a live no YouTube** com a API v3:
   - título e descrição vêm do site;
   - a miniatura é a `midia\miniatura.jpg`;
   - é ligada a um stream próprio deste broadcaster, um por live simultânea,
     guardados em `agent\state\youtube-streams.json` (as chaves não saem da máquina).
3. **Põe no ar o vídeo de espera** (`midia\aguarde.mp4`, em loop) nesse stream.
4. **Abre o `start-broadcast.cmd`** com:
   - `WECAMP_BROADCAST_FROM_FRAME`: avança rápido, sem transmitir, até 10 s antes do pontapé;
   - `WECAMP_CONTROL_FILE`: o arquivo de controle da parada;
   - `WECAMP_FFMPEG`: o `ffmpeg-jogo.cmd`, que tira o vídeo de espera do ar logo
     antes de o jogo entrar (veja **Vídeos e miniatura**).

**Para parar:**

- O site manda o quadro final (fim da partida + 10 s) e o vídeo de encerramento,
  e o agente escreve o quadro no arquivo de controle.
- O RetroArch encerra a transmissão nesse quadro e sai.
- O agente põe no ar o vídeo de encerramento, espera 15 s depois dele, encerra a
  live no YouTube e informa ao site.

**Reprise:** num card **Encerrado**, o admin também pode clicar em **Transmitir**.

- O broadcaster manda o replay inteiro da sala (o `.krec` do Stream ao vivo), do quadro 0 até o fim, sem avanço rápido.
- Só funciona enquanto o replay estiver no servidor.
- Com todas as vagas ocupadas, um jogo ao vivo de campeonato interrompe uma reprise.

**Vagas:** `slots` no `config.json`. No i5 M 460 recomendo 2: aguenta 3 em 480p, mas o avanço rápido até o pontapé
ocupa um núcleo inteiro por ~1 min e poderia engasgar as outras.

**Atualização do agente:** a tarefa agendada roda o agente na inicialização e, se ele não estiver rodando, a cada 5 min.
Quando o `agent.ps1` ou o `config.json` mudam, o agente sai sozinho assim que estiver ocioso, e a tarefa o abre de novo
já com a versão nova.

**Atualização das ISOs:** a cada hora o agente consulta `GET /api/broadcaster/isos`.

- ISO atualizada no cadastro (mesma referência, outro MD5): baixa a nova.
- Com `sync_all`, baixa também as que faltam, mantendo `min_free_gb` livres.

**Arquivos do agente:**

- `config.json`: copiado do `config.example.json`, com o token gerado em `/admin/transmissoes`.
- `autorizar-youtube.cmd`: autoriza o canal uma vez. Rode nesta máquina, com um
  navegador logado na conta Google do canal. Usa a credencial do Google em
  `assets\client_secret_*.json`.
- `instalar-agente.ps1`: instala a tarefa agendada e os ajustes de energia. Rode como Administrador.
- `agent.ps1 -Check`: testa o site, o YouTube e as ISOs.
- `logs\agent.log`: o log do agente.

## Vídeos e miniatura (`midia\`)

| Arquivo | Quando vai ao ar |
|---|---|
| `miniatura.jpg` (ou `.png`) | miniatura da live: aparece antes de ela começar, na lista do canal e nos links compartilhados |
| `aguarde.mp4` | em loop, desde a criação da live até o jogo entrar (o avanço rápido até o pontapé) |
| `fim.mp4` | depois do jogo, quando a partida terminou no apito final |
| `interrompida.mp4` | depois do jogo, em qualquer outro fim: alguém saiu antes do apito, o Stream ao vivo acabou antes (o anfitrião saiu), a transmissão foi cancelada ou o RetroArch falhou |

- **Quem decide entre `fim` e `interrompida`:** o site, junto com a parada
  (`ending`). Sem essa informação, vale `fim` só quando o RetroArch chegou ao
  quadro de parada (ou a reprise chegou ao fim do replay).
- **Formato:** qualquer vídeo serve. O agente converte cada um uma vez para
  `midia\cache\<nome>.flv`, no mesmo formato do jogo (640x480, 30 fps, H.264 com
  GOP de 2 s, AAC 44,1 kHz), e envia com `-c copy`. Trocou o vídeo, mesmo com
  data mais antiga: ele converte de novo. Os de encerramento ganham 5 s do último
  quadro, para o fim da live não cortá-los.
- **Troca sem dois envios ao mesmo tempo:** cada chave de stream do YouTube
  aceita um envio por vez.
  - O agente grava o PID do ffmpeg do vídeo de espera em `run\job-<id>.intro`.
  - O fork abre o `ffmpeg-jogo.cmd` no lugar do ffmpeg (`WECAMP_FFMPEG`). O
    script para esse PID (só se for um ffmpeg, com `Stop-Process`), apaga o
    arquivo e roda o ffmpeg de verdade (`WECAMP_FFMPEG_REAL`) com os mesmos
    argumentos. Se o ffmpeg do jogo reiniciar, o arquivo já não existe e nada
    mais é parado.
  - O fork não muda nada para isso.
- **O vídeo de encerramento entra logo:** entre uma consulta e outra ao site, o
  agente confere a cada 250 ms se o RetroArch saiu.
- **Ajustes no `config.json`:**
  - `media_dir` (padrão `midia`);
  - `intro_max_minutes` (30): o vídeo de espera para sozinho depois disso, caso
    o agente morra;
  - `end_delay_seconds` (15);
  - `ffmpeg` (o do PATH).

Para trocar um vídeo, substitua o arquivo em `midia\` com o mesmo nome.

## Pasta na máquina (`C:\broadcaster`)

| Arquivo | Origem |
|---|---|
| `retroarch.exe` | `retroarch-k3-ffw`: `make OUTDIR=out -j8`, depois `out\retroarch.exe` |
| `libwinpthread-1.dll`, `libstdc++-6.dll`, `libgcc_s_seh-1.dll` | `C:\msys64\mingw64\bin` (os do mesmo toolchain do build) |
| `kailleraclient.dll` | este repositório: `build.bat`, depois `x64\Release\kailleraclient.dll` (define `WECAMP_BROADCASTER`) |
| `retroarch.cfg`, `start-broadcast.cmd`, `ffmpeg-jogo.cmd` | esta pasta |
| `midia\` | a miniatura e os três vídeos (veja **Vídeos e miniatura**); `midia\cache\` é do agente |
| `cores\pcsx_rearmed_libretro.dll`, `info\pcsx_rearmed_libretro.info` | o pacote do RetroArch TIERES |
| `system\SCPH-1001.BIN` | BIOS |
| `config\PCSX-ReARMed\PCSX-ReARMed.opt` | opções do core. O fork força as de sincronia. |
| `games\<ISO>` | a ISO da sala, com o **mesmo nome de arquivo** que os jogadores usam (`PCSX ReARMed: <arquivo>` é o nome do jogo no Kaillera) |
| `logs\` | `retroarch-<sessao>.log`, `ffmpeg-<sessao>.log`, `broadcaster.log` |

O ffmpeg é o do winget, instalado com `winget install --id Gyan.FFmpeg.Shared
--exact --scope machine`, e fica no PATH. Depois de instalar com
`--scope machine`, rode `icacls "<pasta do pacote>" /reset /T` como admin. Sem
isso, só o usuário que instalou consegue executar o ffmpeg.

## Uso

```
start-broadcast.cmd SESSAO "games\ARQUIVO DO JOGO" SAIDA [SALA]
```

- `SAIDA`: `rtmp://a.rtmp.youtube.com/live2/<chave>`, ou um arquivo para teste
  (`C:\broadcaster\out\teste.flv`).
- Opcionais, por variável de ambiente: `WECAMP_STREAM_SIZE` (640x480),
  `WECAMP_STREAM_FPS` (30), `WECAMP_STREAM_VBITRATE` (1500k),
  `WECAMP_STREAM_ABITRATE` (128k), `WECAMP_STREAM_PRESET` (veryfast),
  `WECAMP_FFMPEG`.

A chave do YouTube não aparece no log do RetroArch, porque o fork mostra só
`rtmp://host/...`. Mas ela vai na linha de comando do ffmpeg.

## Testes

- `test-broadcast.ps1`: roda uma sessão por N segundos (padrão 100) gravando em
  arquivo, mede a CPU e mostra os logs e o `ffprobe`.
- `test-full-session.ps1`: roda a sessão inteira e confere se o RetroArch sai
  sozinho no fim.
- `test-videos.ps1`: o vídeo de espera, o jogo e o de encerramento, sem o
  YouTube. Um `ffmpeg -listen` local faz o papel do YouTube (cada envio vira um
  `out\teste-videos\NN.flv`), e o código do próprio agente roda um trabalho. No
  fim, mostra quando cada envio entrou e saiu. Use `-Agent` para testar um
  `agent.ps1` fora de `agent\`, sem reiniciar o agente que está rodando.

Os dois primeiros aceitam `-Session`, `-Game` e `-Out`. O `test-videos.ps1`
aceita `-Session`, `-Game`, `-StartFrame`, `-StopFrame` e `-Ending`
(`fim`, `interrompida` ou `auto`, para o caso sem a informação do site). Sessões já encerradas também
servem, porque o wg-camp entrega o stream inteiro e o fork roda a 1×.

Resultados em 2026-11-18, no notebook i5 M 460, com a sessão
`36368-1794977438` (WE PW26):

- **Teste curto:** imagem e som corretos, cerca de 15% de CPU (ffmpeg ~11%,
  RetroArch ~4%).
- **Sessão inteira (28 min 16 s):**
  - o RetroArch saiu sozinho no fim, o ffmpeg terminou com exit 0 e não sobrou
    nenhum processo;
  - CPU média de 22%, máxima de 29%;
  - 1,54 Mbps a 1×;
  - 0 frames pulados, 22 microfaltas de áudio, 0 reinícios do ffmpeg.

**Limitação do Watch Live (não é do broadcaster).** Quando a partida tem um
comando "carregar estado" sem um "salvar estado" antes, na mesma sessão, o
espectador não tem o arquivo. Ele continua sem carregar e pode divergir do
que os jogadores viram, porque eles carregam o estado da máquina deles.
