# kmsg_burst_test

Testa o `kcore/k_message.h` de verdade contra a rajada pós-login do EmuLinker-K.

Depois do último ServerACK, o servidor manda 12 bundles de uma vez: ServerStatus
(serial 4), 10 InfoMsgs e o UserJoined do próprio jogador. Cada bundle leva a
mensagem nova e as 4 anteriores, então só os 5 primeiros carregam o ServerStatus.
Nos servidores da Oracle (SP e VIN) os 12 saem em ~0,4 ms. No provedor do Tássio
os primeiros se perdiam, e a DLL até a v.TIERES.0.21 ficava presa em
"logging in": o `while (has_data())` do `kaillera_step` girava para sempre
esperando o buraco.

- `fake_elk.py` faz um login falso (HELLO, 4 ServerACKs, rajada) e decide o que a
  "rede" faz com a rajada:
  - `clean`: tudo, em ordem;
  - `drop5`: os 5 primeiros bundles somem, que é o caso do Tássio;
  - `reorder`: os bundles 6..12 chegam antes dos 1..5;
  - `drop10`: os 10 primeiros somem, um salto de 10+ seriais.
- `kmsg_client.cpp` usa o `k_message` do mesmo jeito que o `kaillera_step`. Códigos
  de saída: 0 = UserJoined entregue, 2 = laço de recepção girando sem voltar,
  3 = timeout.

## Como rodar

No "x64 Native Tools Command Prompt" (VS2019 BuildTools serve), a partir desta pasta:

```
cl /nologo /EHsc kmsg_client.cpp ..\..\common\k_socket.cpp ws2_32.lib
python fake_elk.py --port 27999 --mode drop5
kmsg_client.exe 27999
```

Rode o `fake_elk.py` em outro terminal, ou em segundo plano. Ele atende um login
e sai.

## Resultado (2026-09-29)

| modo      | v.TIERES.0.21 | v.TIERES.0.22                      |
|-----------|---------------|------------------------------------|
| `clean`   | OK            | OK                                 |
| `drop5`   | SPIN          | OK em ~420 ms, sem o ServerStatus  |
| `reorder` | SPIN          | OK na hora, com o ServerStatus     |
| `drop10`  | TIMEOUT       | OK                                 |
