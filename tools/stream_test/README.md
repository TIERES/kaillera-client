# stream_test

Testa o envio do "Stream ao vivo!" (`common/n02_stream.cpp`) contra um wg-camp
de verdade, com um proxy no meio que estraga parte dos POSTs do
`/spectate/ingest`, e confere se o stream que ficou no servidor é exatamente o
que o host enviou.

Foi feito para a correção de 2026-10-20. Em produção, o Caddy às vezes
respondia 502 ao ingest, e o DLL contava o lote como enviado assim que o
`send()` terminava, sem ler a resposta. Um lote de ~0,3 s sumia, e quem
acompanhava a partida ao vivo (ou via o replay online) dessincronizava dali em
diante. Agora o lote só conta com um 2xx: senão vai de novo, com o mesmo
`X-Sequence`, e o wg-camp ignora o que já tem.

- `fault_proxy.py` sobe o wg-camp (banco temporário, a mesma chave de
  espectador do DLL) e, na frente dele, um proxy que a cada 12 POSTs:
  - responde 502 sem repassar (o caso de produção);
  - repassa e fecha sem responder (o servidor gravou, a resposta se perdeu);
  - repassa e só responde depois de 6,5 s (o DLL desiste antes e reenvia);
  - fecha logo depois dos cabeçalhos (o corpo nem chega).

  Quando a sessão termina, ele compara o `.krec` do servidor com o
  `esperado.bin`.
- `stream_test.cpp` é um host com o `n02_stream.cpp` de verdade: uns 40 s de
  partida a 60 fps (2 jogadores, conteúdo diferente em cada frame) e uma linha
  de chat a cada 300 frames. Grava em `esperado.bin` os registros na ordem em
  que foram enviados ao stream.

## Como rodar

```
build.cmd
<wg-camp>\.venv\Scripts\python.exe fault_proxy.py <wg-camp> esperado.bin --port 5070
stream_test.exe 5070 2400
```

O proxy fica em primeiro plano. Rode o `stream_test.exe` em outro terminal, na
mesma pasta do `esperado.bin`.

Contra o servidor de verdade (sem falhas injetadas, só confere que o DLL lê a
resposta do Caddy): `stream_test.exe 8080 600 we2002.wgs.dev.br`. Uma sessão
de 10 s não aparece nos Replays Online, que só listam partidas de 5 min ou mais.

## Resultado (2026-10-21)

| DLL | POSTs | falhas injetadas | reenvios ignorados pelo servidor | lotes perdidos | stream no servidor |
|---|---|---|---|---|---|
| master (antes) | 141 | 47 | 0 | 23 | diferente a partir do byte 597 |
| corrigido | 94 | 31 | 16 | 0 | idêntico (65 024 bytes) |

Em produção (2026-10-21, logo depois do deploy do ingest novo), 600 frames
foram em 43 lotes, todos com 204 e sem reenvio. No servidor: `last_sequence`
42, `lost_batches` 0, e os registros idênticos aos enviados.

O wg-camp com o ingest novo registra no log cada sequência pulada ("N lote(s)
perdido(s) antes da sequência X") e conta em `live_sessions.lost_batches`. É
assim que dá para ver, em produção, as partidas de DLLs antigos que ainda
perdem lote.
