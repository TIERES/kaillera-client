# krec_harness

Roda o core `pcsx_rearmed_libretro.dll` sem janela, direto do Python (ctypes),
alimentando os inputs de um `.krec` do mesmo jeito que o retroarch-k3 faz numa
partida Kaillera:

- um registro de input por `retro_run()`;
- 12 bytes por jogador;
- registro vazio vira input neutro;
- JOYPAD em todas as portas;
- mesmas opções forçadas do `kaillera_sync.c`.

O digest de RAM é o mesmo do detector de desync. Por isso dá para conferir
qualquer gravação contra as linhas `[SYNC] ... d=<janela>:<hash>` que os próprios
jogadores gravam dentro do stream.

Validado em 2026-09-28: reproduz bit a bit a partida
`..._Jefferson_Coimbra_new_player.krec`.

## Requisitos

- Python 3 64 bits (`py -3`). Não precisa de bibliotecas extras.
- Uma pasta do RetroArch TIERES com `cores\pcsx_rearmed_libretro.dll`,
  `system\` (BIOS) e `config\PCSX-ReARMed\PCSX-ReARMed.opt`.
  Padrão: `--ra D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.0.3`.
- O arquivo do jogo, via `--game` (padrão: o `.bin` do Master League em Downloads).

Mantenha `FORCED` em `krec_harness.py` igual a `ksync_forced_options[]`
(retroarch-k3, `kaillera/kaillera_sync.c`).

## Modos

```
py -3 krec_harness.py info    partida.krec
py -3 krec_harness.py audit   partida.krec              # replay do frame 1 contra os digests dos jogadores
py -3 krec_harness.py compare local.krec --other servidor.krec
py -3 krec_harness.py golive  servidor.krec --state records\watch_debug\spectator_<sessao>_f<frame>_o<offset>.state
```

- **audit**: equivale a um espectador que abriu "Acompanhar ao vivo!" no começo.
  - `SEM DESYNC`: o stream reproduz a RAM dos jogadores do começo ao fim.
  - `DIVERGE na janela N`: a partir dali o stream não bate mais. Pode ser lote
    perdido ou duplicado, comando no input, rollback ou memory card.
- **compare**: compara byte a byte a gravação local do host (checkbox Gravar) com
  a cópia do servidor (Replays Online). Mostra onde faltam ou sobram bytes, ou
  seja, um lote perdido ou duplicado.
- **golive**: pega o state que o espectador baixou num "Ir ao vivo!" e roda a
  partir dele. Confere duas coisas:
  - se o offset cai exatamente no input do frame do host;
  - se, depois do load, a RAM continua batendo com os jogadores.

  O dump não faz parte da versão. Para um teste, aplique
  `git apply tools/krec_harness/watch_debug_dump.patch`, compile o
  `kailleraclient` e crie a pasta `records\watch_debug` ao lado do
  `retroarch.exe` (no host e no espectador). O host grava `host_*.state` e o
  espectador grava `spectator_*.state`, mais uma linha em `watch_debug.txt`.
  Depois do teste, desfaça com `git apply -R` do mesmo patch.

### Experimentos de determinismo (um processo por execução)

O DLL do core guarda estado global, então cada "máquina" é um processo:

```
py -3 krec_harness.py ref       partida.krec --F 60000 --K 12000 --state s.bin --out ref.json
py -3 krec_harness.py roundtrip partida.krec --F 60000 --K 12000 --out rt.json
py -3 krec_harness.py spectator partida.krec --F 60000 --K 12000 --state s.bin --G 57000 --out sp.json
py -3 krec_harness.py diffhash  ref.json rt.json sp.json
```

Opções úteis:

- `--opt chave=valor`: roda com uma opção diferente do core.
- `--input-from N`: simula o salto recusado (state do frame F com o input do frame N).
- `--pace` / `--burst`: ritmo de 60 fps e pausas na borda do ao vivo.
- `--av`: flags de áudio/vídeo.

Resultados de 2026-09-28:

- save/load do PCSX é determinístico: o espectador fica idêntico ao host;
- o salto recusado diverge no primeiro frame;
- `pcsx_rearmed_gpu_thread_rendering` diferente entre as máquinas diverge.

## Core com espaço de código maior (RetroArch fechando no meio da partida)

O dynarec do PCSX-ReARMed (Lightrec) guarda o código compilado num espaço de
8 MB. Em algumas máquinas o core avisa "Memory map is sub-par" e gera código
maior, e numa partida longa esse espaço pode acabar. O log mostra
"Could not alloc even after removing old blocks!" e depois
"Unable to compile block!". Daí há dois desfechos possíveis:

- o core termina o processo ("Exiting at cycle ...", o RetroArch fecha sem aviso);
- ou a emulação diverge (desync).

Casos de 2026-09-28:

- a partida `..._Nicolas_BSB_TIERES_R7_SemMCard.krec` fecha no frame 79 152;
- a partida `..._Jefferson_Coimbra_new_player.krec` dessincroniza em ~23:30.

O core corrigido está em https://github.com/TIERES/pcsx_rearmed (branch
`tieres`, build com `tieres/build-win64.sh`). É o mesmo commit `8625c39`, com
as mesmas strings de versão e `CODE_BUFFER_SIZE` de 32 MB. Validado no harness:

- as duas partidas acima vão até o fim, com a RAM igual à dos jogadores;
- nas três partidas de teste, as 920 janelas de digest foram todas iguais às dos jogadores;
- states do core oficial carregam no novo e vice-versa, com emulação idêntica.

Para testar um core com o harness: `--core caminho\pcsx_rearmed_libretro.dll`.
Para achar onde um core fecha o processo: `--trace-from <frame>`.

## Limitações

- Não reproduz rollback (BACKSPACE). A auditoria para na primeira linha `e=` > 0.
- Não reproduz memory card. Uma partida "COM memory card" pode divergir quando
  o jogo ler o cartão.
- Ignora comandos no input (swap/save/load/reset), mas avisa quando encontra.
