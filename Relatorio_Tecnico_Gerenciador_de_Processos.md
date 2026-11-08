# RELATÓRIO TÉCNICO — GERENCIADOR DE PROCESSOS (GP) PARA GNU/LINUX

**Instituição:** Instituto Federal de Educação, Ciência e Tecnologia do Ceará (*Campus* Crato)  
**Curso:** Bacharelado em Sistemas de Informação  
**Disciplina:** Sistemas Distribuídos  
**Professor:** Guilherme Esmeraldo  
**Projeto:** Gerenciador de Processos (GP) para GNU/Linux  

---

## 1. Identificação do Autor

| Nome Completo do Aluno | E-mail | GitHub |
| :--- | :--- | :--- |
| **Kauê Rodrigues Grangeiro** | `kauerodrigues089@gmail.com` | [@kauerodri](https://github.com/kauerodri) |

**Data de Entrega:** `08/11/2026`

---

## 2. Introdução e Objetivos

Em Sistemas Operacionais modernos multitarefa (como o GNU/Linux), o núcleo (*kernel*) atua como escalonador de tempo de processador (*time-sharing*), alternando fatias de tempo (*time slices*) entre múltiplos processos concorrentes. Para administrar esses recursos de forma eficaz, o sistema operacional disponibiliza primitivas (chamadas de sistema) e interfaces virtuais do kernel que permitem inspecionar e controlar o ciclo de vida e a prioridade de escalonamento das tarefas.

O presente relatório descreve o projeto e a implementação de um **Gerenciador de Processos (GP)** desenvolvido em **linguagem C** para o sistema operacional **GNU/Linux**, utilizando diretamente as chamadas de sistema POSIX/Linux e o sistema de arquivos virtual `/proc` (*procfs*), sem depender de comandos externos via `system()` (como `ps`, `top` ou `kill` de shell).

---

## 3. Arquitetura e Soluções de Software Adotadas

### 3.1. Leitura Direta do Sistema de Arquivos Virtual `/proc` (*procfs*)
Em vez de invocar programas externos, o Gerenciador de Processos consulta diretamente as estruturas de dados do kernel expostas em `/proc`:
- **Varredura de Processos Ativos (`opendir` / `readdir`):** O diretório `/proc` é percorrido filtrando-se apenas os subdiretórios cujo nome é estritamente numérico, cada um correspondendo ao `PID` de um processo ativo no sistema.
- **Identificação do Proprietário (`stat` e `getpwuid` / `getpwnam`):** A chamada `stat("/proc/[pid]", &st)` obtém o `st_uid` do inode do processo, que reflete o UID real do proprietário. Em seguida, a função da biblioteca padrão POSIX `getpwuid()` consulta a base de usuários (`/etc/passwd`) para traduzir o UID para o nome de login (`USER`).
- **Extração de Estado, Prioridade e Tempo de CPU (`/proc/[pid]/stat`):** O arquivo `/proc/[pid]/stat` contém os metadados internos da `task_struct` do kernel. Como o 2º campo (`comm`) fica entre parênteses e pode conter espaços ou parênteses internos, o parser implementado localiza o primeiro `'('` com `strchr()` e o último `')'` com `strrchr()`, garantindo extração exata dos campos subsequentes:
  - **Campo 3 (`state`):** Estado atual do processo (`R`, `S`, `D`, `T`, `t`, `Z`, `I`).
  - **Campos 14 e 15 (`utime` e `stime`):** Tempo de CPU acumulado em modo usuário e modo kernel, medidos em *clock ticks* (*jiffies*). A conversão para segundos reais é realizada dividindo-se `(utime + stime)` pelo valor retornado por `sysconf(_SC_CLK_TCK)`.
  - **Campo 19 (`nice`):** Valor de prioridade `NICE` do processo no intervalo de `-20` (maior prioridade) a `+19` (menor prioridade).
- **Recuperação da Linha de Comando (`/proc/[pid]/cmdline`):** Utilizada tanto para exibição detalhada quanto para reconstruir o vetor `argv[]` original na operação de **Reiniciar processo**.

### 3.2. Chamadas de Sistema (System Calls) Utilizadas
- `kill(pid, sig)`: Envio de sinais POSIX para transição de estados (`SIGSTOP` para bloquear, `SIGCONT` para continuar, `SIGTERM` e `SIGKILL` para finalizar).
- `fork()` e `execvp()` / `execv()`: Criação de novos processos filhos e substituição da imagem de memória para execução e reinicialização de programas.
- `setpriority(PRIO_PROCESS, pid, nice)` e `getpriority(PRIO_PROCESS, pid)`: Consulta e alteração direta da prioridade de escalonamento (`NICE`) tanto de processos já em execução quanto de novos processos antes da chamada `execvp()`.
- `waitpid(pid, NULL, WNOHANG)`: Coleta assíncrona de processos filhos encerrados para evitar a formação de processos zumbis (`Z`).
- `pipe()`, `fcntl(FD_CLOEXEC)`, `dup2()`: Sincronização entre pai e filho para reportar erros caso o comando informado pelo usuário não exista ao chamar `execvp()`.

---

## 4. Detalhamento das Funções Implementadas

| Função | Assinatura | Descrição Técnica |
| :--- | :--- | :--- |
| `obter_info_processo` | `int obter_info_processo(pid_t pid, ProcessoInfo *info)` | Lê `/proc/[pid]`, `/proc/[pid]/stat` e `/proc/[pid]/cmdline`, preenchendo a struct `ProcessoInfo` com `PID`, `USER`, `NICE`, `STATUS`, `TIME` e linha de comando. |
| `listar_processos_usuario` | `void listar_processos_usuario(const char *nome_usuario)` | Converte o nome do usuário em `UID` via `getpwnam()`, percorre `/proc` e exibe uma tabela formatada com todos os processos daquele usuário. |
| `consultar_processo_pid` | `void consultar_processo_pid(pid_t pid)` | Exibe o painel detalhado de um processo específico (`PID`, `NICE`, `USER`, `TIME` em `HH:MM:SS.cc` e `STATUS` traduzido). |
| `bloquear_processo` | `int bloquear_processo(pid_t pid)` | Suspende a execução de um processo enviando o sinal `SIGSTOP` via `kill(pid, SIGSTOP)`, levando-o ao estado `T`. |
| `continuar_processo` | `int continuar_processo(pid_t pid)` | Verifica se o processo encontra-se bloqueado (`state == 'T'` ou `'t'`) e, em caso afirmativo, envia `SIGCONT` via `kill(pid, SIGCONT)`. |
| `executar_novo_processo` | `pid_t executar_novo_processo(const char *cmd, int def_nice, int val_nice)` | Faz o parsing dos argumentos, cria um processo filho via `fork()`, ajusta opcionalmente o `NICE` com `setpriority()` antes do `execvp()` e executa o comando em background. |
| `reiniciar_processo` | `int reiniciar_processo(pid_t pid)` | Lê `/proc/[pid]/cmdline`, `/proc/[pid]/exe`, `/proc/[pid]/cwd` e o `nice` atual; encerra o processo antigo com `SIGTERM` e cria uma nova instância idêntica com `fork()` + `execv()`. |
| `finalizar_processo` | `int finalizar_processo(pid_t pid, int forcar_sigkill)` | Encerra um processo enviando `SIGTERM` (15 - término gracioso) ou `SIGKILL` (9 - término imediato forçado). |
| `alterar_prioridade_em_execucao` | `int alterar_prioridade_em_execucao(pid_t pid, int novo_nice)` | Valida o intervalo `[-20, +19]` e aplica a nova prioridade ao processo em execução usando `setpriority(PRIO_PROCESS, pid, novo_nice)`. |

---

## 5. Recursos de Interface Utilizados

O software oferece **duas modalidades de interface**, proporcionando máxima flexibilidade:
1. **Menu Interativo no Terminal (TUI):** Ao executar `./gp` sem parâmetros, é apresentado um menu estruturado com submenus intuitivos, realce visual por códigos ANSI de cores, validação de entradas inválidas no `stdin` e mensagens claras de sucesso ou erro.
2. **Interface por Argumentos de Linha de Comando (CLI):** Permite automação e uso rápido diretamente pelo shell:
   - `./gp -u <usuario>` — Lista processos de um usuário.
   - `./gp -i <pid>` — Consulta informações detalhadas do PID.
   - `./gp -b <pid>` — Bloqueia (suspende) o processo.
   - `./gp -c <pid>` — Continua o processo bloqueado.
   - `./gp -e "<comando>"` — Executa novo processo.
   - `./gp -r <pid>` — Reinicia o processo.
   - `./gp -k <pid>` / `./gp -K <pid>` — Finaliza o processo (`SIGTERM` / `SIGKILL`).
   - `./gp -p <pid> <nice>` — Altera a prioridade de um processo em execução.
   - `./gp -n <nice> "<comando>"` — Executa um novo processo já com a prioridade `NICE` definida.

---

## 6. Mapeamento de Atendimento aos Requisitos

1. **a) Listar processos de um determinado usuário:** Atendido na função `listar_processos_usuario()`, que valida a existência do usuário via `getpwnam()`, varre `/proc` e filtra pelo `UID` proprietário do processo.
2. **b) Consultar informações sobre um determinado processo (`PID`, `NICE`, `USER`, `TIME`, `STATUS`):** Atendido na função `consultar_processo_pid()` em conjunto com `obter_info_processo()`, que extrai todos os 5 indicadores obrigatórios diretamente de `/proc/[pid]/stat` e dos metadados do diretório `/proc/[pid]`.
3. **c) Alterar o estado de um processo:**
   - **Bloquear:** Atendido via `kill(pid, SIGSTOP)` em `bloquear_processo()`.
   - **Continuar (caso esteja bloqueado):** Atendido em `continuar_processo()`, que inspeciona previamente se `info.state == 'T'` antes de enviar `kill(pid, SIGCONT)`.
   - **Executar:** Atendido em `executar_novo_processo()` usando `fork()` e `execvp()`.
   - **Reiniciar:** Atendido em `reiniciar_processo()`, que preserva a linha de comando, diretório de trabalho e prioridade `NICE` do processo original antes de encerrá-lo e recriá-lo.
   - **Finalizar:** Atendido em `finalizar_processo()` com suporte a `SIGTERM` e `SIGKILL`.
4. **d) Trocar prioridade de execução (`NICE`):**
   - **De um processo em execução:** Atendido em `alterar_prioridade_em_execucao()` via `setpriority(PRIO_PROCESS, pid, novo_nice)`.
   - **Antes de executar um processo:** Atendido em `executar_novo_processo(..., definir_nice=1, valor_nice)`, configurando a prioridade no processo filho após o `fork()` e imediatamente antes do `execvp()`.

---

## 7. Compilação e Roteiro de Testes Práticos no Linux

### 7.1. Compilação
```bash
gcc -Wall -Wextra -pedantic -O2 -o gp gerenciador_processos.c
```

### 7.2. Roteiro Rápido de Validação em Laboratório
1. **Iniciar o gerenciador:** `./gp` (ou testar via flags CLI).
2. **Executar um processo de teste com prioridade pré-definida (+10):**
   - Opção `4` -> Subopção `2` -> Comando: `sleep 300` -> NICE: `10`.
   - O programa exibirá o `PID` gerado (ex.: `4512`).
3. **Consultar o processo criado:**
   - Opção `2` -> Informe o PID `4512`. Verifique `PID`, `NICE (10)`, `USER`, `TIME` e `STATUS (S - Sleeping)`.
4. **Bloquear e Continuar o processo:**
   - Opção `3` -> Subopção `1` (Bloquear) -> PID `4512`.
   - Consulte novamente na Opção `2` e observe que o estado mudou para `T (Bloqueado / Suspenso por sinal - Stopped)`.
   - Opção `3` -> Subopção `2` (Continuar) -> PID `4512`. O estado retorna para `S`.
5. **Trocar prioridade em execução:**
   - Opção `4` -> Subopção `1` -> PID `4512` -> Novo NICE: `15` (ou `-5` executando com `sudo ./gp`).
6. **Reiniciar e Finalizar:**
   - Opção `3` -> Subopção `4` (Reiniciar) -> PID `4512`. Observe o encerramento do PID antigo e a criação do novo PID com o mesmo comando `sleep 300`.
   - Opção `3` -> Subopção `5` (Finalizar) -> Informe o novo PID para encerrá-lo.
