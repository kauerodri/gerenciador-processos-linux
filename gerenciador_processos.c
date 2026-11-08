/**
 * ============================================================================
 * INSTITUTO FEDERAL DE EDUCAÇÃO, CIÊNCIA E TECNOLOGIA DO CEARÁ - CAMPUS CRATO
 * Curso: Bacharelado em Sistemas de Informação
 * Disciplina: Sistemas Distribuídos / Sistemas Operacionais
 * Professor: Guilherme Esmeraldo
 * Autor: Kauê Rodrigues Grangeiro (kauerodrigues089@gmail.com)
 *
 * Projeto: Gerenciador de Processos (GP) para GNU/Linux
 * Arquivo: gerenciador_processos.c
 *
 * Descrição:
 *   Ferramenta em linguagem C para gerenciamento de processos no GNU/Linux,
 *   utilizando chamadas de sistema POSIX/Linux (fork, execvp, kill,
 *   getpriority, setpriority, nice, stat, opendir/readdir) e leitura direta
 *   do sistema de arquivos virtual /proc.
 *
 * Compilação:
 *   gcc -Wall -Wextra -pedantic -O2 -o gp gerenciador_processos.c
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>

/* Constantes de configuração e limites */
#define MAX_CMD_LEN     1024
#define MAX_PATH_LEN    512
#define MAX_ARGS        64
#define MAX_LINE_LEN    2048

/* Códigos de cores ANSI para interface no terminal */
#define COR_RESET       "\033[0m"
#define COR_NEGRITO     "\033[1m"
#define COR_VERDE       "\033[1;32m"
#define COR_AMARELO     "\033[1;33m"
#define COR_VERMELHO    "\033[1;31m"
#define COR_CIANO       "\033[1;36m"
#define COR_AZUL        "\033[1;34m"

/**
 * Estrutura que armazena as informações consolidada de um processo
 * extraídas de /proc/[pid]/stat, /proc/[pid]/cmdline e metadados do inode.
 */
typedef struct {
    pid_t pid;                      /* Identificador do processo (PID) */
    char comm[256];                 /* Nome do executável (campo comm) */
    char cmdline[MAX_CMD_LEN];      /* Linha de comando completa */
    char state;                     /* Caractere de estado (R, S, D, Z, T, etc.) */
    long nice;                      /* Prioridade de execução NICE (-20 a +19) */
    uid_t uid;                      /* UID numérico do proprietário */
    char user[128];                 /* Nome do usuário proprietário (USER) */
    unsigned long utime;            /* Tempo de CPU em modo usuário (clock ticks) */
    unsigned long stime;            /* Tempo de CPU em modo kernel (clock ticks) */
    double total_time_sec;          /* Tempo total de CPU em segundos */
} ProcessoInfo;

/* ============================================================================
 * PROTÓTIPOS DAS FUNÇÕES
 * ============================================================================ */

/* Funções utilitárias e de suporte */
int eh_numero(const char *str);
void limpar_buffer_entrada(void);
void pausar_tela(void);
const char* traduzir_estado_processo(char state);
void formatar_tempo_execucao(double segundos, char *buffer, size_t tam);
int dividir_argumentos(char *comando, char *argv[], int max_args);

/* Núcleo de leitura do sistema de arquivos /proc */
int obter_info_processo(pid_t pid, ProcessoInfo *info);
int recuperar_cmdline_processo(pid_t pid, char *argv_out[], char *buffer_bruto, size_t tam_buf);

/* Funcionalidades obrigatórias da especificação */
void listar_processos_usuario(const char *nome_usuario);
void consultar_processo_pid(pid_t pid);
int bloquear_processo(pid_t pid);
int continuar_processo(pid_t pid);
pid_t executar_novo_processo(const char *comando, int definir_nice, int valor_nice);
int reiniciar_processo(pid_t pid);
int finalizar_processo(pid_t pid, int forcar_sigkill);
int alterar_prioridade_em_execucao(pid_t pid, int novo_nice);

/* Interfaces com o usuário */
void exibir_banner(void);
void menu_alterar_estado(void);
void menu_alterar_prioridade(void);
void exibir_ajuda_cli(const char *nome_prog);

/* ============================================================================
 * IMPLEMENTAÇÃO DAS FUNÇÕES UTILITÁRIAS
 * ============================================================================ */

/**
 * Verifica se uma string contém exclusivamente dígitos numéricos.
 * Usado para filtrar os subdiretórios de /proc que representam PIDs.
 */
int eh_numero(const char *str) {
    if (str == NULL || *str == '\0') {
        return 0;
    }
    while (*str) {
        if (!isdigit((unsigned char)*str)) {
            return 0;
        }
        str++;
    }
    return 1;
}

/**
 * Descarta caracteres remanescentes no buffer padrão de entrada (stdin).
 */
void limpar_buffer_entrada(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

/**
 * Aguarda o usuário pressionar ENTER antes de retornar ao menu principal.
 */
void pausar_tela(void) {
    printf("\n" COR_CIANO "Pressione [ENTER] para continuar..." COR_RESET);
    fflush(stdout);
    limpar_buffer_entrada();
}

/**
 * Traduz a sigla de estado do kernel Linux (presente em /proc/[pid]/stat)
 * para uma descrição legível em português.
 */
const char* traduzir_estado_processo(char state) {
    switch (state) {
        case 'R': return "R (Em execucao / Executavel - Running)";
        case 'S': return "S (Dormindo / Aguardando evento - Sleeping)";
        case 'D': return "D (Espera ininterrupta de E/S - Disk Sleep)";
        case 'T': return "T (Bloqueado / Suspenso por sinal - Stopped)";
        case 't': return "t (Parado em rastreamento/depuracao - Traced)";
        case 'Z': return "Z (Processo Zumbi / Finalizado - Zombie)";
        case 'X':
        case 'x': return "X (Morto - Dead)";
        case 'I': return "I (Thread ociosa do Kernel - Idle)";
        default:  return "? (Estado desconhecido)";
    }
}

/**
 * Converte o tempo total em segundos para o formato padrão HH:MM:SS.cc
 */
void formatar_tempo_execucao(double segundos, char *buffer, size_t tam) {
    unsigned long total_centesimos = (unsigned long)(segundos * 100.0);
    unsigned long centesimos = total_centesimos % 100;
    unsigned long total_seg = total_centesimos / 100;
    unsigned long seg = total_seg % 60;
    unsigned long min = (total_seg / 60) % 60;
    unsigned long horas = total_seg / 3600;

    snprintf(buffer, tam, "%02lu:%02lu:%02lu.%02lu", horas, min, seg, centesimos);
}

/**
 * Divide uma linha de comando em um vetor de argumentos (argv) compatível
 * com a chamada de sistema execvp(). Suporta argumentos entre aspas simples ou duplas.
 */
int dividir_argumentos(char *comando, char *argv[], int max_args) {
    int argc = 0;
    char *p = comando;

    while (*p != '\0' && argc < max_args - 1) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (*p == '"' || *p == '\'') {
            char aspas = *p++;
            argv[argc++] = p;
            while (*p != '\0' && *p != aspas) {
                p++;
            }
            if (*p != '\0') {
                *p++ = '\0';
            }
        } else {
            argv[argc++] = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                p++;
            }
            if (*p != '\0') {
                *p++ = '\0';
            }
        }
    }
    argv[argc] = NULL;
    return argc;
}

/* ============================================================================
 * NÚCLEO DE LEITURA DO SISTEMA DE ARQUIVOS /proc
 * ============================================================================ */

/**
 * Coleta as informações de um processo a partir de /proc/[pid].
 *
 * Retorna:
 *   0 em caso de sucesso;
 *  -1 em caso de erro (ajustando errno adequadamente).
 */
int obter_info_processo(pid_t pid, ProcessoInfo *info) {
    char caminho[MAX_PATH_LEN];
    char linha[MAX_LINE_LEN];
    struct stat st;
    FILE *fp;

    if (pid <= 0 || info == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(info, 0, sizeof(ProcessoInfo));
    info->pid = pid;

    /* 1. Verifica existência do processo e obtém UID proprietário via stat() */
    snprintf(caminho, sizeof(caminho), "/proc/%d", pid);
    if (stat(caminho, &st) == -1) {
        return -1; /* errno conterá ENOENT se o processo não existir */
    }
    info->uid = st.st_uid;

    /* Converte UID numérico para nome de usuário usando getpwuid() */
    struct passwd *pw = getpwuid(info->uid);
    if (pw != NULL) {
        snprintf(info->user, sizeof(info->user), "%s", pw->pw_name);
    } else {
        snprintf(info->user, sizeof(info->user), "%u", (unsigned int)info->uid);
    }

    /* 2. Abre e analisa /proc/[pid]/stat */
    snprintf(caminho, sizeof(caminho), "/proc/%d/stat", pid);
    fp = fopen(caminho, "r");
    if (fp == NULL) {
        return -1;
    }

    if (fgets(linha, sizeof(linha), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /*
     * Nota técnica importante sobre /proc/[pid]/stat:
     * O 2º campo (comm) fica entre parênteses "(nome)" e pode conter espaços
     * ou até outros parênteses. Por isso, localizamos o primeiro '(' e o
     * último ')' usando strchr e strrchr antes de ler os demais campos.
     */
    char *abre_paren = strchr(linha, '(');
    char *fecha_paren = strrchr(linha, ')');
    if (abre_paren == NULL || fecha_paren == NULL || fecha_paren < abre_paren) {
        errno = EPROTO;
        return -1;
    }

    size_t tam_comm = (size_t)(fecha_paren - abre_paren - 1);
    if (tam_comm >= sizeof(info->comm)) {
        tam_comm = sizeof(info->comm) - 1;
    }
    memcpy(info->comm, abre_paren + 1, tam_comm);
    info->comm[tam_comm] = '\0';

    /*
     * Campos após ')':
     * (3) state  (4) ppid   (5) pgrp   (6) session (7) tty_nr (8) tpgid
     * (9) flags  (10) minflt (11) cminflt (12) majflt (13) cmajflt
     * (14) utime (15) stime (16) cutime (17) cstime (18) priority (19) nice
     */
    char *resto = fecha_paren + 2; /* Pula ") " */
    int ppid, pgrp, session, tty_nr, tpgid;
    unsigned int flags;
    unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
    long cutime, cstime, priority, nice_val;

    int lidos = sscanf(
        resto,
        "%c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld",
        &info->state,
        &ppid, &pgrp, &session, &tty_nr, &tpgid,
        &flags, &minflt, &cminflt, &majflt, &cmajflt,
        &utime, &stime, &cutime, &cstime, &priority, &nice_val
    );

    if (lidos < 17) {
        errno = EPROTO;
        return -1;
    }

    info->utime = utime;
    info->stime = stime;
    info->nice = nice_val;

    /* Calcula o tempo total de CPU em segundos: (utime + stime) / CLK_TCK */
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) {
        clk_tck = 100; /* Fallback padrão no Linux */
    }
    info->total_time_sec = (double)(info->utime + info->stime) / (double)clk_tck;

    /* 3. Lê a linha de comando completa em /proc/[pid]/cmdline (se disponível) */
    snprintf(caminho, sizeof(caminho), "/proc/%d/cmdline", pid);
    fp = fopen(caminho, "r");
    if (fp != NULL) {
        size_t n = fread(info->cmdline, 1, sizeof(info->cmdline) - 1, fp);
        fclose(fp);
        if (n > 0) {
            info->cmdline[n] = '\0';
            /* Substitui os separadores nulos '\0' entre argumentos por espaços */
            for (size_t i = 0; i + 1 < n; i++) {
                if (info->cmdline[i] == '\0') {
                    info->cmdline[i] = ' ';
                }
            }
        }
    }

    /* Se cmdline for vazio (ex: threads de kernel), usa [comm] */
    if (info->cmdline[0] == '\0') {
        snprintf(info->cmdline, sizeof(info->cmdline), "[%s]", info->comm);
    }

    return 0;
}

/**
 * Recupera o vetor de argumentos original (argv) de um processo lendo
 * /proc/[pid]/cmdline (onde cada argumento é separado por '\0').
 * Essencial para a funcionalidade de Reiniciar Processo.
 */
int recuperar_cmdline_processo(pid_t pid, char *argv_out[], char *buffer_bruto, size_t tam_buf) {
    char caminho[MAX_PATH_LEN];
    snprintf(caminho, sizeof(caminho), "/proc/%d/cmdline", pid);

    int fd = open(caminho, O_RDONLY);
    if (fd == -1) {
        return -1;
    }

    ssize_t bytes_lidos = read(fd, buffer_bruto, tam_buf - 1);
    close(fd);

    if (bytes_lidos <= 0) {
        return -1;
    }
    buffer_bruto[bytes_lidos] = '\0';

    int argc = 0;
    ssize_t i = 0;
    while (i < bytes_lidos && argc < MAX_ARGS - 1) {
        argv_out[argc++] = &buffer_bruto[i];
        while (i < bytes_lidos && buffer_bruto[i] != '\0') {
            i++;
        }
        i++; /* Pula o byte nulo '\0' */
    }
    argv_out[argc] = NULL;

    return argc;
}

/* ============================================================================
 * IMPLEMENTAÇÃO DOS REQUISITOS OBRIGATÓRIOS DO PROJETO
 * ============================================================================ */

/**
 * Requisito (a): Listar processos de um determinado usuário.
 */
void listar_processos_usuario(const char *nome_usuario) {
    if (nome_usuario == NULL || *nome_usuario == '\0') {
        fprintf(stderr, COR_VERMELHO "[ERRO] Nome de usuario invalido.\n" COR_RESET);
        return;
    }

    /* Obtém o registro do usuário no sistema (/etc/passwd) */
    struct passwd *pw = getpwnam(nome_usuario);
    if (pw == NULL) {
        fprintf(stderr, COR_VERMELHO "[ERRO] O usuario '%s' nao existe neste sistema.\n" COR_RESET, nome_usuario);
        return;
    }
    uid_t uid_alvo = pw->pw_uid;

    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        perror(COR_VERMELHO "[ERRO] Falha ao abrir o diretorio /proc" COR_RESET);
        return;
    }

    printf("\n" COR_NEGRITO "Processos pertencentes ao usuario: " COR_VERDE "%s (UID: %u)" COR_RESET "\n",
           nome_usuario, (unsigned int)uid_alvo);
    printf("----------------------------------------------------------------------------------------\n");
    printf(COR_NEGRITO "%-8s %-14s %-6s %-6s %-13s %-35s\n" COR_RESET,
           "PID", "USER", "STATUS", "NICE", "TIME", "COMANDO");
    printf("----------------------------------------------------------------------------------------\n");

    struct dirent *entrada;
    int contador = 0;

    while ((entrada = readdir(dir)) != NULL) {
        /* Ignora entradas em /proc que não sejam números (PIDs) */
        if (!eh_numero(entrada->d_name)) {
            continue;
        }

        pid_t pid = (pid_t)atoi(entrada->d_name);
        ProcessoInfo info;

        if (obter_info_processo(pid, &info) == 0) {
            if (info.uid == uid_alvo) {
                char tempo_str[32];
                formatar_tempo_execucao(info.total_time_sec, tempo_str, sizeof(tempo_str));

                printf("%-8d %-14.14s %-6c %-6ld %-13s %-35.35s\n",
                       info.pid,
                       info.user,
                       info.state,
                       info.nice,
                       tempo_str,
                       info.comm);
                contador++;
            }
        }
    }

    closedir(dir);
    printf("----------------------------------------------------------------------------------------\n");
    printf("Total de processos encontrados para '%s': " COR_NEGRITO "%d" COR_RESET "\n",
           nome_usuario, contador);
}

/**
 * Requisito (b): Permitir consulta de informações sobre um determinado processo:
 * - Identificador de Processo (PID)
 * - Prioridade de execução (NICE)
 * - Proprietário (USER)
 * - Tempo total de execução (TIME)
 * - Estado (STATUS)
 */
void consultar_processo_pid(pid_t pid) {
    if (pid <= 0) {
        fprintf(stderr, COR_VERMELHO "[ERRO] PID invalido (%d). Informe um inteiro positivo.\n" COR_RESET, pid);
        return;
    }

    ProcessoInfo info;
    if (obter_info_processo(pid, &info) == -1) {
        if (errno == ENOENT) {
            fprintf(stderr, COR_VERMELHO "[ERRO] Processo com PID %d nao encontrado (nao existe).\n" COR_RESET, pid);
        } else if (errno == EACCES) {
            fprintf(stderr, COR_VERMELHO "[ERRO] Permissao negada ao consultar o PID %d.\n" COR_RESET, pid);
        } else {
            fprintf(stderr, COR_VERMELHO "[ERRO] Falha ao ler dados do PID %d: %s\n" COR_RESET, pid, strerror(errno));
        }
        return;
    }

    char tempo_str[32];
    formatar_tempo_execucao(info.total_time_sec, tempo_str, sizeof(tempo_str));

    printf("\n" COR_CIANO "============================================================" COR_RESET "\n");
    printf(COR_NEGRITO "           INFORMACOES DETALHADAS DO PROCESSO               " COR_RESET "\n");
    printf(COR_CIANO "============================================================" COR_RESET "\n");
    printf("  %-30s : " COR_NEGRITO "%d" COR_RESET "\n", "Identificador (PID)", info.pid);
    printf("  %-30s : %s\n", "Nome do Executavel", info.comm);
    printf("  %-30s : %s (UID: %u)\n", "Proprietario (USER)", info.user, (unsigned int)info.uid);
    printf("  %-30s : %ld\n", "Prioridade (NICE)", info.nice);
    printf("  %-30s : %s (%.2f s)\n", "Tempo Total de Execucao (TIME)", tempo_str, info.total_time_sec);
    printf("  %-30s : %s\n", "Estado (STATUS)", traduzir_estado_processo(info.state));
    printf("  %-30s : %s\n", "Linha de Comando", info.cmdline);
    printf(COR_CIANO "============================================================" COR_RESET "\n");
}

/**
 * Requisito (c.1): Alterar o estado de um processo -> Bloquear (suspender).
 * Envia o sinal SIGSTOP ao processo especificado.
 */
int bloquear_processo(pid_t pid) {
    ProcessoInfo info;
    if (obter_info_processo(pid, &info) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Processo PID %d nao encontrado: %s\n" COR_RESET, pid, strerror(errno));
        return -1;
    }

    if (info.state == 'T') {
        printf(COR_AMARELO "[AVISO] O processo %d (%s) ja se encontra bloqueado (estado T).\n" COR_RESET,
               pid, info.comm);
        return 0;
    }

    if (kill(pid, SIGSTOP) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Nao foi possivel bloquear o processo %d: %s\n" COR_RESET,
                pid, strerror(errno));
        return -1;
    }

    /* Aguarda brevemente o kernel atualizar o estado em /proc */
    usleep(50000);
    printf(COR_VERDE "[SUCESSO] Sinal SIGSTOP enviado! Processo %d (%s) foi BLOQUEADO.\n" COR_RESET,
           pid, info.comm);
    return 0;
}

/**
 * Requisito (c.2): Alterar o estado de um processo -> Continuar (caso esteja bloqueado).
 * Verifica se o processo está em estado 'T' ou 't' antes de enviar SIGCONT.
 */
int continuar_processo(pid_t pid) {
    ProcessoInfo info;
    if (obter_info_processo(pid, &info) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Processo PID %d nao encontrado: %s\n" COR_RESET, pid, strerror(errno));
        return -1;
    }

    if (info.state != 'T' && info.state != 't') {
        printf(COR_AMARELO "[AVISO] O processo %d (%s) NAO esta bloqueado (Estado atual: %c). "
               "A operacao 'Continuar' aplica-se apenas a processos bloqueados.\n" COR_RESET,
               pid, info.comm, info.state);
        return -1;
    }

    if (kill(pid, SIGCONT) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Falha ao enviar SIGCONT ao processo %d: %s\n" COR_RESET,
                pid, strerror(errno));
        return -1;
    }

    usleep(50000);
    printf(COR_VERDE "[SUCESSO] Sinal SIGCONT enviado! Processo %d (%s) retomou a execucao.\n" COR_RESET,
           pid, info.comm);
    return 0;
}

/**
 * Requisito (c.3) e (d.2): Executar um novo processo (com opção de definir
 * a prioridade NICE antes da execução).
 *
 * Parâmetros:
 *   comando      - String com o comando e argumentos (ex: "sleep 60")
 *   definir_nice - Flag booleana (1 = configurar nice antes do exec, 0 = manter padrão)
 *   valor_nice   - Valor de prioridade NICE desejado (-20 a +19)
 */
pid_t executar_novo_processo(const char *comando, int definir_nice, int valor_nice) {
    if (comando == NULL || *comando == '\0') {
        fprintf(stderr, COR_VERMELHO "[ERRO] Comando vazio.\n" COR_RESET);
        return -1;
    }

    if (definir_nice && (valor_nice < -20 || valor_nice > 19)) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Valor NICE invalido (%d). O intervalo permitido no Linux e de -20 a +19.\n" COR_RESET,
                valor_nice);
        return -1;
    }

    char copia_cmd[MAX_CMD_LEN];
    snprintf(copia_cmd, sizeof(copia_cmd), "%s", comando);

    char *argv[MAX_ARGS];
    int argc = dividir_argumentos(copia_cmd, argv, MAX_ARGS);
    if (argc == 0) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Nenhum comando valido informado.\n" COR_RESET);
        return -1;
    }

    /* Pipe com close-on-exec para detectar falha imediata no execvp do filho */
    int pipe_erro[2];
    if (pipe(pipe_erro) == -1) {
        perror(COR_VERMELHO "[ERRO] Falha ao criar pipe de sincronizacao" COR_RESET);
        return -1;
    }
    fcntl(pipe_erro[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        perror(COR_VERMELHO "[ERRO] Falha na chamada de sistema fork()" COR_RESET);
        close(pipe_erro[0]);
        close(pipe_erro[1]);
        return -1;
    }

    if (pid == 0) {
        /* ==================== PROCESSO FILHO ==================== */
        close(pipe_erro[0]);

        /* Coloca o processo em seu próprio grupo para não conflitar com o menu */
        setpgid(0, 0);

        /*
         * Requisito (d.2): Trocar prioridade de execução ANTES de executar o processo.
         * Utiliza setpriority() / nice() no processo filho antes do execvp().
         */
        if (definir_nice) {
            errno = 0;
            if (setpriority(PRIO_PROCESS, 0, valor_nice) == -1) {
                int err = errno;
                fprintf(stderr, COR_AMARELO "[AVISO FILHO] Nao foi possivel aplicar NICE %d (%s). "
                        "Executando com prioridade padrao.\n" COR_RESET,
                        valor_nice, strerror(err));
            }
        }

        /* Redireciona entrada padrão de /dev/null para rodar em background de forma segura */
        int fd_null = open("/dev/null", O_RDONLY);
        if (fd_null != -1) {
            dup2(fd_null, STDIN_FILENO);
            close(fd_null);
        }

        /* Substitui a imagem do processo filho pelo programa solicitado */
        execvp(argv[0], argv);

        /* Se execvp retornar, houve erro: envia errno ao processo pai */
        int err = errno;
        if (write(pipe_erro[1], &err, sizeof(err)) < 0) {
            /* Ignora erro de escrita no encerramento */
        }
        close(pipe_erro[1]);
        _exit(EXIT_FAILURE);
    }

    /* ==================== PROCESSO PAI ==================== */
    close(pipe_erro[1]);

    int erro_filho = 0;
    ssize_t lidos = read(pipe_erro[0], &erro_filho, sizeof(erro_filho));
    close(pipe_erro[0]);

    if (lidos > 0) {
        /* O filho falhou no execvp */
        waitpid(pid, NULL, 0);
        fprintf(stderr, COR_VERMELHO "[ERRO] Falha ao executar '%s': %s\n" COR_RESET,
                argv[0], strerror(erro_filho));
        return -1;
    }

    /* Reap de eventuais processos filhos já terminados sem bloquear */
    waitpid(pid, NULL, WNOHANG);

    errno = 0;
    int nice_atual = getpriority(PRIO_PROCESS, (id_t)pid);
    printf(COR_VERDE "[SUCESSO] Processo iniciado em background!\n" COR_RESET);
    printf("          -> Comando : %s\n", comando);
    printf("          -> PID     : " COR_NEGRITO "%d" COR_RESET "\n", pid);
    if (errno == 0) {
        printf("          -> NICE    : %d\n", nice_atual);
    }

    return pid;
}

/**
 * Requisito (c.4): Alterar o estado de um processo -> Reiniciar.
 *
 * Captura a linha de comando original (/proc/[pid]/cmdline), diretório de
 * trabalho (/proc/[pid]/cwd) e prioridade (NICE) do processo em execução,
 * encerra a instância atual e inicia uma nova instância idêntica.
 */
int reiniciar_processo(pid_t pid) {
    ProcessoInfo info;
    if (obter_info_processo(pid, &info) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Processo PID %d nao encontrado: %s\n" COR_RESET, pid, strerror(errno));
        return -1;
    }

    char buffer_cmd[MAX_CMD_LEN];
    char *argv_original[MAX_ARGS];
    int argc = recuperar_cmdline_processo(pid, argv_original, buffer_cmd, sizeof(buffer_cmd));
    if (argc <= 0 || argv_original[0] == NULL || argv_original[0][0] == '\0') {
        fprintf(stderr, COR_VERMELHO "[ERRO] Nao foi possivel recuperar a linha de comando do PID %d "
                "(pode ser uma thread de kernel ou sem permissao).\n" COR_RESET, pid);
        return -1;
    }

    /* Tenta recuperar o caminho real do executável (/proc/[pid]/exe) e diretório (/proc/[pid]/cwd) */
    char caminho_proc[MAX_PATH_LEN];
    char exe_path[MAX_PATH_LEN] = {0};
    char cwd_path[MAX_PATH_LEN] = {0};

    snprintf(caminho_proc, sizeof(caminho_proc), "/proc/%d/exe", pid);
    ssize_t len_exe = readlink(caminho_proc, exe_path, sizeof(exe_path) - 1);
    if (len_exe > 0) {
        exe_path[len_exe] = '\0';
    }

    snprintf(caminho_proc, sizeof(caminho_proc), "/proc/%d/cwd", pid);
    ssize_t len_cwd = readlink(caminho_proc, cwd_path, sizeof(cwd_path) - 1);
    if (len_cwd > 0) {
        cwd_path[len_cwd] = '\0';
    }

    long nice_antigo = info.nice;

    printf("[INFO] Encerrando instancia atual do processo %d (%s)...\n", pid, info.comm);

    /* Se o processo estiver bloqueado (SIGSTOP), envia SIGCONT junto com SIGTERM */
    if (kill(pid, SIGTERM) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Nao foi possivel finalizar o processo %d para reinicio: %s\n" COR_RESET,
                pid, strerror(errno));
        return -1;
    }
    kill(pid, SIGCONT);

    /* Aguarda até 1 segundo pelo encerramento gracioso; caso contrário, força com SIGKILL */
    for (int tentativa = 0; tentativa < 10; tentativa++) {
        usleep(100000);
        if (kill(pid, 0) == -1 && errno == ESRCH) {
            break;
        }
        /* Caso seja processo filho direto deste GP, coleta o status */
        waitpid(pid, NULL, WNOHANG);
    }
    if (kill(pid, 0) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, WNOHANG);
        usleep(50000);
    }

    /* Cria a nova instância do processo */
    pid_t novo_pid = fork();
    if (novo_pid < 0) {
        perror(COR_VERMELHO "[ERRO] Falha ao criar novo processo no reinicio" COR_RESET);
        return -1;
    }

    if (novo_pid == 0) {
        setpgid(0, 0);
        if (cwd_path[0] != '\0') {
            if (chdir(cwd_path) < 0) {
                /* Mantém diretório atual caso não consiga mudar */
            }
        }
        setpriority(PRIO_PROCESS, 0, (int)nice_antigo);

        int fd_null = open("/dev/null", O_RDONLY);
        if (fd_null != -1) {
            dup2(fd_null, STDIN_FILENO);
            close(fd_null);
        }

        if (exe_path[0] != '\0') {
            execv(exe_path, argv_original);
        }
        execvp(argv_original[0], argv_original);
        _exit(EXIT_FAILURE);
    }

    usleep(50000);
    printf(COR_VERDE "[SUCESSO] Processo reiniciado com sucesso!\n" COR_RESET);
    printf("          -> PID Antigo : %d (finalizado)\n", pid);
    printf("          -> PID Novo   : " COR_NEGRITO "%d" COR_RESET "\n", novo_pid);
    printf("          -> Comando    : %s\n", info.cmdline);

    return 0;
}

/**
 * Requisito (c.5): Alterar o estado de um processo -> Finalizar.
 * Permite encerrar o processo com SIGTERM (15) ou SIGKILL (9).
 */
int finalizar_processo(pid_t pid, int forcar_sigkill) {
    ProcessoInfo info;
    if (obter_info_processo(pid, &info) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Processo PID %d nao encontrado: %s\n" COR_RESET, pid, strerror(errno));
        return -1;
    }

    int sinal = forcar_sigkill ? SIGKILL : SIGTERM;
    const char *nome_sinal = forcar_sigkill ? "SIGKILL (9)" : "SIGTERM (15)";

    if (kill(pid, sinal) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Falha ao enviar %s para o PID %d: %s\n" COR_RESET,
                nome_sinal, pid, strerror(errno));
        return -1;
    }

    /* Se estava suspenso e recebeu SIGTERM, envia SIGCONT para que possa processar o término */
    if (!forcar_sigkill && info.state == 'T') {
        kill(pid, SIGCONT);
    }

    /* Se o processo morto era filho direto do gerenciador, recolhe o zombie com waitpid */
    usleep(50000);
    waitpid(pid, NULL, WNOHANG);

    printf(COR_VERDE "[SUCESSO] Sinal %s enviado ao processo %d (%s). Processo finalizado!\n" COR_RESET,
           nome_sinal, pid, info.comm);
    return 0;
}

/**
 * Requisito (d.1): Trocar prioridade de execução de um processo já em execução.
 * Utiliza a chamada de sistema setpriority(PRIO_PROCESS, pid, novo_nice).
 */
int alterar_prioridade_em_execucao(pid_t pid, int novo_nice) {
    if (novo_nice < -20 || novo_nice > 19) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Valor NICE (%d) fora do intervalo permitido (-20 a +19).\n" COR_RESET,
                novo_nice);
        return -1;
    }

    ProcessoInfo info;
    if (obter_info_processo(pid, &info) == -1) {
        fprintf(stderr, COR_VERMELHO "[ERRO] Processo PID %d nao encontrado: %s\n" COR_RESET, pid, strerror(errno));
        return -1;
    }

    long nice_anterior = info.nice;

    if (setpriority(PRIO_PROCESS, (id_t)pid, novo_nice) == -1) {
        if (errno == EACCES || errno == EPERM) {
            fprintf(stderr, COR_VERMELHO "[ERRO] Permissao negada ao alterar prioridade do PID %d para %d.\n"
                    "       Dica: No Linux, diminuir o valor de NICE (aumentar prioridade) ou alterar\n"
                    "       processos de outros usuarios requer privilegios de superusuario (sudo).\n" COR_RESET,
                    pid, novo_nice);
        } else {
            fprintf(stderr, COR_VERMELHO "[ERRO] Falha em setpriority() para o PID %d: %s\n" COR_RESET,
                    pid, strerror(errno));
        }
        return -1;
    }

    printf(COR_VERDE "[SUCESSO] Prioridade (NICE) do processo %d (%s) alterada de %ld para %d!\n" COR_RESET,
           pid, info.comm, nice_anterior, novo_nice);
    return 0;
}

/* ============================================================================
 * MENUS INTERATIVOS E INTERFACE DE LINHA DE COMANDO (CLI)
 * ============================================================================ */

void exibir_banner(void) {
    printf("\n" COR_AZUL "======================================================================" COR_RESET "\n");
    printf(COR_NEGRITO "          GERENCIADOR DE PROCESSOS (GP) - GNU/LINUX                   " COR_RESET "\n");
    printf("     IFCE Campus Crato - Bacharelado em Sistemas de Informacao        \n");
    printf(COR_AZUL "======================================================================" COR_RESET "\n");
}

void menu_alterar_estado(void) {
    int sub_opcao = -1;
    pid_t pid;
    char comando[MAX_CMD_LEN];

    printf("\n" COR_CIANO "--- [3] ALTERAR ESTADO DE UM PROCESSO ---" COR_RESET "\n");
    printf("  1. Bloquear (Suspender processo - SIGSTOP)\n");
    printf("  2. Continuar (Retomar processo bloqueado - SIGCONT)\n");
    printf("  3. Executar (Iniciar um novo processo)\n");
    printf("  4. Reiniciar (Encerrar e recriar processo existente)\n");
    printf("  5. Finalizar (Encerrar processo - SIGTERM / SIGKILL)\n");
    printf("  0. Voltar ao menu principal\n");
    printf("Escolha uma opcao: ");

    if (scanf("%d", &sub_opcao) != 1) {
        limpar_buffer_entrada();
        fprintf(stderr, COR_VERMELHO "[ERRO] Entrada invalida.\n" COR_RESET);
        return;
    }
    limpar_buffer_entrada();

    switch (sub_opcao) {
        case 1:
            printf("Informe o PID do processo que deseja BLOQUEAR: ");
            if (scanf("%d", &pid) == 1) {
                limpar_buffer_entrada();
                bloquear_processo(pid);
            } else {
                limpar_buffer_entrada();
                fprintf(stderr, COR_VERMELHO "[ERRO] PID invalido.\n" COR_RESET);
            }
            break;

        case 2:
            printf("Informe o PID do processo bloqueado que deseja CONTINUAR: ");
            if (scanf("%d", &pid) == 1) {
                limpar_buffer_entrada();
                continuar_processo(pid);
            } else {
                limpar_buffer_entrada();
                fprintf(stderr, COR_VERMELHO "[ERRO] PID invalido.\n" COR_RESET);
            }
            break;

        case 3:
            printf("Informe o comando a ser executado (ex: sleep 120): ");
            if (fgets(comando, sizeof(comando), stdin) != NULL) {
                comando[strcspn(comando, "\n")] = '\0';
                executar_novo_processo(comando, 0, 0);
            }
            break;

        case 4:
            printf("Informe o PID do processo que deseja REINICIAR: ");
            if (scanf("%d", &pid) == 1) {
                limpar_buffer_entrada();
                reiniciar_processo(pid);
            } else {
                limpar_buffer_entrada();
                fprintf(stderr, COR_VERMELHO "[ERRO] PID invalido.\n" COR_RESET);
            }
            break;

        case 5: {
            int modo = 1;
            printf("Informe o PID do processo que deseja FINALIZAR: ");
            if (scanf("%d", &pid) != 1) {
                limpar_buffer_entrada();
                fprintf(stderr, COR_VERMELHO "[ERRO] PID invalido.\n" COR_RESET);
                break;
            }
            printf("Tipo de encerramento [1 = Normal (SIGTERM) | 2 = Forcado (SIGKILL)]: ");
            if (scanf("%d", &modo) != 1) {
                modo = 1;
            }
            limpar_buffer_entrada();
            finalizar_processo(pid, (modo == 2));
            break;
        }

        case 0:
            return;

        default:
            fprintf(stderr, COR_VERMELHO "[ERRO] Opcao invalida.\n" COR_RESET);
            break;
    }
}

void menu_alterar_prioridade(void) {
    int sub_opcao = -1;
    pid_t pid;
    int novo_nice;
    char comando[MAX_CMD_LEN];

    printf("\n" COR_CIANO "--- [4] TROCAR PRIORIDADE DE EXECUCAO (NICE) ---" COR_RESET "\n");
    printf("  1. Alterar prioridade de um processo JA EM EXECUCAO (renice)\n");
    printf("  2. Definir prioridade ANTES DE EXECUTAR um novo processo (nice)\n");
    printf("  0. Voltar ao menu principal\n");
    printf("Escolha uma opcao: ");

    if (scanf("%d", &sub_opcao) != 1) {
        limpar_buffer_entrada();
        fprintf(stderr, COR_VERMELHO "[ERRO] Entrada invalida.\n" COR_RESET);
        return;
    }
    limpar_buffer_entrada();

    switch (sub_opcao) {
        case 1:
            printf("Informe o PID do processo em execucao: ");
            if (scanf("%d", &pid) != 1) {
                limpar_buffer_entrada();
                fprintf(stderr, COR_VERMELHO "[ERRO] PID invalido.\n" COR_RESET);
                break;
            }
            printf("Informe a nova prioridade NICE (-20 = maxima prioridade a +19 = minima): ");
            if (scanf("%d", &novo_nice) != 1) {
                limpar_buffer_entrada();
                fprintf(stderr, COR_VERMELHO "[ERRO] Valor NICE invalido.\n" COR_RESET);
                break;
            }
            limpar_buffer_entrada();
            alterar_prioridade_em_execucao(pid, novo_nice);
            break;

        case 2:
            printf("Informe o comando para o novo processo (ex: sleep 180): ");
            if (fgets(comando, sizeof(comando), stdin) == NULL) {
                break;
            }
            comando[strcspn(comando, "\n")] = '\0';

            printf("Informe a prioridade NICE inicial (-20 a +19): ");
            if (scanf("%d", &novo_nice) != 1) {
                limpar_buffer_entrada();
                fprintf(stderr, COR_VERMELHO "[ERRO] Valor NICE invalido.\n" COR_RESET);
                break;
            }
            limpar_buffer_entrada();
            executar_novo_processo(comando, 1, novo_nice);
            break;

        case 0:
            return;

        default:
            fprintf(stderr, COR_VERMELHO "[ERRO] Opcao invalida.\n" COR_RESET);
            break;
    }
}

void exibir_ajuda_cli(const char *nome_prog) {
    printf("Uso: %s [OPCAO] [ARGUMENTOS...]\n\n", nome_prog);
    printf("Se executado sem argumentos, abre o MENU INTERATIVO.\n\n");
    printf("Opcoes via linha de comando:\n");
    printf("  -u <usuario>          Lista todos os processos do usuario informado\n");
    printf("  -i <pid>              Consulta informacoes detalhadas do processo (PID)\n");
    printf("  -b <pid>              Bloqueia (suspende) o processo com SIGSTOP\n");
    printf("  -c <pid>              Continua um processo bloqueado com SIGCONT\n");
    printf("  -e \"<comando>\"        Executa um novo processo em background\n");
    printf("  -r <pid>              Reinicia o processo especificado\n");
    printf("  -k <pid>              Finaliza o processo (SIGTERM)\n");
    printf("  -K <pid>              Finaliza forcadamente o processo (SIGKILL)\n");
    printf("  -p <pid> <nice>       Altera a prioridade (NICE) de um processo em execucao\n");
    printf("  -n <nice> \"<comando>\" Executa um novo processo ja com a prioridade NICE definida\n");
    printf("  -h                    Exibe esta mensagem de ajuda\n");
}

/* ============================================================================
 * FUNÇÃO PRINCIPAL (main)
 * ============================================================================ */

int main(int argc, char *argv[]) {
    /* Modo 1: Execução direta via argumentos de linha de comando (CLI) */
    if (argc > 1) {
        if (strcmp(argv[1], "-u") == 0 && argc >= 3) {
            listar_processos_usuario(argv[2]);
        } else if (strcmp(argv[1], "-i") == 0 && argc >= 3) {
            consultar_processo_pid((pid_t)atoi(argv[2]));
        } else if (strcmp(argv[1], "-b") == 0 && argc >= 3) {
            return bloquear_processo((pid_t)atoi(argv[2])) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        } else if (strcmp(argv[1], "-c") == 0 && argc >= 3) {
            return continuar_processo((pid_t)atoi(argv[2])) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        } else if (strcmp(argv[1], "-e") == 0 && argc >= 3) {
            return executar_novo_processo(argv[2], 0, 0) > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        } else if (strcmp(argv[1], "-r") == 0 && argc >= 3) {
            return reiniciar_processo((pid_t)atoi(argv[2])) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        } else if (strcmp(argv[1], "-k") == 0 && argc >= 3) {
            return finalizar_processo((pid_t)atoi(argv[2]), 0) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        } else if (strcmp(argv[1], "-K") == 0 && argc >= 3) {
            return finalizar_processo((pid_t)atoi(argv[2]), 1) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        } else if (strcmp(argv[1], "-p") == 0 && argc >= 4) {
            return alterar_prioridade_em_execucao((pid_t)atoi(argv[2]), atoi(argv[3])) == 0
                   ? EXIT_SUCCESS : EXIT_FAILURE;
        } else if (strcmp(argv[1], "-n") == 0 && argc >= 4) {
            return executar_novo_processo(argv[3], 1, atoi(argv[2])) > 0
                   ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            exibir_ajuda_cli(argv[0]);
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    /* Modo 2: Menu Interativo */
    int opcao = -1;
    char usuario[128];
    pid_t pid;

    while (1) {
        /* Recolhe eventuais processos filhos em segundo plano que já finalizaram */
        while (waitpid(-1, NULL, WNOHANG) > 0);

        exibir_banner();
        printf("  " COR_NEGRITO "1." COR_RESET " Listar processos de um determinado usuario\n");
        printf("  " COR_NEGRITO "2." COR_RESET " Consultar informacoes de um processo (pelo PID)\n");
        printf("  " COR_NEGRITO "3." COR_RESET " Alterar o estado de um processo (Bloquear/Continuar/Executar/Reiniciar/Finalizar)\n");
        printf("  " COR_NEGRITO "4." COR_RESET " Trocar prioridade de execucao (NICE)\n");
        printf("  " COR_NEGRITO "0." COR_RESET " Sair do Gerenciador de Processos\n");
        printf("----------------------------------------------------------------------\n");
        printf("Selecione uma opcao: ");

        if (scanf("%d", &opcao) != 1) {
            limpar_buffer_entrada();
            fprintf(stderr, COR_VERMELHO "\n[ERRO] Entrada invalida! Digite um numero de 0 a 4.\n" COR_RESET);
            pausar_tela();
            continue;
        }
        limpar_buffer_entrada();

        switch (opcao) {
            case 1:
                printf("\nInforme o nome do usuario (ex: root, ubuntu): ");
                if (fgets(usuario, sizeof(usuario), stdin) != NULL) {
                    usuario[strcspn(usuario, "\n")] = '\0';
                    listar_processos_usuario(usuario);
                }
                pausar_tela();
                break;

            case 2:
                printf("\nInforme o PID do processo que deseja consultar: ");
                if (scanf("%d", &pid) == 1) {
                    limpar_buffer_entrada();
                    consultar_processo_pid(pid);
                } else {
                    limpar_buffer_entrada();
                    fprintf(stderr, COR_VERMELHO "[ERRO] PID invalido.\n" COR_RESET);
                }
                pausar_tela();
                break;

            case 3:
                menu_alterar_estado();
                pausar_tela();
                break;

            case 4:
                menu_alterar_prioridade();
                pausar_tela();
                break;

            case 0:
                printf(COR_VERDE "\nEncerrando o Gerenciador de Processos. Ate logo!\n" COR_RESET);
                return EXIT_SUCCESS;

            default:
                fprintf(stderr, COR_VERMELHO "\n[ERRO] Opcao inexistente! Escolha entre 0 e 4.\n" COR_RESET);
                pausar_tela();
                break;
        }
    }

    return EXIT_SUCCESS;
}
