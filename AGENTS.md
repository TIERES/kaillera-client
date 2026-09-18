# AGENTS.md

Este arquivo fornece orientações ao Codex ao trabalhar com o código deste repositório.

## Visão geral do projeto

N02 é uma implementação de DLL cliente Kaillera que fornece funcionalidade de jogo em rede para emuladores de N64. Ela implementa a API Kaillera (`kailleraclient.dll`) e oferece três modos de jogo em rede:

- **Modo P2P** — Conexões diretas ponto a ponto.
- **Modo cliente** — Jogo em rede tradicional baseado em servidor Kaillera (compatível com kaillera.com).
- **Modo de reprodução** — Reprodução de gravações de arquivos `.krec`.

## Comandos de compilação

### Compilação local (preferencial)

```bash
./build.bat
```

Esta é a forma padrão de compilar localmente. O script detecta automaticamente o VS2019/2022/2026 e executa o MSBuild com as configurações corretas.

O script de compilação localiza automaticamente o MSBuild em:

- VS2026 Community/Professional/Enterprise (toolset v145)
- VS2022 Community/Professional/Enterprise (toolset v143)
- VS2019 Build Tools/Community (toolset v142)

Saída: `x64\Release\kailleraclient.dll`

Para usar em um emulador (por exemplo, RMG), copie a DLL gerada para a pasta do emulador com o nome `kailleraclient.dll`.

### Compilação manual

```bash
# VS2026 (toolset v145)
msbuild n02p.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0

# VS2022 (toolset v143)
msbuild n02p.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0

# VS2019 (toolset v142)
msbuild n02p.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142 /p:WindowsTargetPlatformVersion=10.0
```

### CI

O workflow do GitHub Actions (`.github/workflows/build.yml`) compila em pushes de tags `v*` e em PRs. Os artefatos são enviados automaticamente.

### Dependências

- Windows SDK 10.0 (WinSock2: `ws2_32.lib`, `comctl32.lib`)
- Visual Studio 2019 ou posterior com as ferramentas de compilação C++

## Arquitetura

### Módulos de código-fonte

**Ponto de entrada / exportações da DLL** (`kailleraclient.cpp`)

- Implementa a API Kaillera padrão: `kailleraInit`, `kailleraSetInfos`, `kailleraSelectServerDialog`, `kailleraModifyPlayValues`, `kailleraChatSend`, `kailleraEndGame`, `kailleraShutdown`.
- Alterna módulos por meio da estrutura `n02_MODULE`, com ponteiros de função para cada modo.
- Possui um wrapper opcional de gravação que captura entradas em arquivos `.krec`.

**Modo P2P** (`core/`)

- `p2p_core.cpp/h` — Gerenciamento de conexões P2P, sincronização de jogo e chat.
- `p2p_message.cpp/h` — Enquadramento de mensagens UDP com cache de instruções e retransmissão.
- `p2p_instruction.cpp/h` — Tipos de instrução do protocolo P2P.

**Modo cliente Kaillera** (`kcore/`)

- `kaillera_core.cpp/h` — Conexão com servidor, lobby e gerenciamento de salas de jogo.
- `k_message.h` — Enquadramento de mensagens do protocolo Kaillera (semelhante a `p2p_message`, mas com seriais de 16 bits).
- `k_instruction.cpp/h` — Codificação e decodificação de instruções do protocolo Kaillera.

**Camada de interface**

- `p2p_ui.cpp/h` — Diálogos do modo P2P (hospedar/entrar, lista de espera do jogo).
- `kaillera_ui.cpp/h` — Diálogos do modo Kaillera (navegador de servidores, lobby e sala de jogo).
- `player.cpp/h` — Modo de reprodução de gravações.

**Utilitários comuns** (`common/`)

- `k_socket.cpp/h` — Wrapper de socket UDP multiplataforma com polling baseado em `select`.
- `k_framecache.cpp/h` — Buffer dinâmico para acumular dados de quadros.
- `nThread.cpp/h` — Abstração de threads.
- `nSettings.cpp/h` — Armazenamento de configurações baseado no Registro do Windows.
- `slist.h`, `oslist.h`, `odlist.h`, `dlist.h` — Contêineres de lista personalizados.

### Padrões principais

**Protocolo de mensagens**: os modos P2P e Kaillera usam um protocolo UDP confiável com:

- Serialização de instruções com números de sequência.
- Cache de saída para retransmitir pacotes recentes.
- Cache de entrada para reordenar pacotes fora de sequência.
- Múltiplas instruções por pacote para redundância.

**Máquina de estados**: o `KSSDFA` (Autômato finito do diálogo de seleção de servidor Kaillera) controla os estados do loop principal:

- Estado 0: polling de eventos de rede.
- Estado 1: callback de jogo pendente.
- Estado 2: jogo em execução.
- Estado 3: encerramento.

**Callbacks**: o emulador fornece callbacks por meio da estrutura `kailleraInfos`:

- `gameCallback` — Chamado quando o jogo inicia (retorna o número do jogador).
- `chatReceivedCallback` — Mensagens de chat recebidas.
- `clientDroppedCallback` — Jogador desconectado.

### Formato de gravação (`.krec`)

Cabeçalho: magic `KRC0` + nome do aplicativo (128) + nome do jogo (128) + timestamp (4) + jogador (4) + número de jogadores (4).

Registros: byte de tipo + dados (`0x12` = quadro de entrada, `0x08` = chat, `0x14` = saída de jogador).
