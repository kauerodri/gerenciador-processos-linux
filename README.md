# Gerenciador de Processos (GP) — GNU/Linux

**Instituição:** Instituto Federal de Educação, Ciência e Tecnologia do Ceará (*Campus* Crato)  
**Curso:** Bacharelado em Sistemas de Informação  
**Disciplina:** Sistemas Distribuídos  
**Professor:** Guilherme Esmeraldo  
**Autor:** Kauê Rodrigues Grangeiro   

---

## Estrutura dos Arquivos Entregues

- `gerenciador_processos.c` — Código-fonte completo e comentado em linguagem C para GNU/Linux.
- `Makefile` — Script de automação para compilação com `gcc`.
- `Relatorio_Tecnico_Gerenciador_de_Processos.md` — Relatório técnico em formato Markdown.
- `Relatorio_Tecnico_Gerenciador_de_Processos.doc` — Relatório técnico formatado compatível com Microsoft Word / Google Docs / LibreOffice Writer (basta abrir, preencher os nomes dos autores na tabela inicial e salvar como PDF).
- `README.md` — Guia rápido de compilação e execução.

---

## Como Compilar no GNU/Linux

Utilizando o `Makefile`:
```bash
make
```

Ou compilando diretamente com o `gcc`:
```bash
gcc -Wall -Wextra -pedantic -O2 -o gp gerenciador_processos.c
```

---

## Como Executar

### 1. Modo Menu Interativo
```bash
./gp
```
*(Dica: Para diminuir valores de `NICE` — aumentando a prioridade — ou gerenciar processos de outros usuários, execute com `sudo ./gp`)*.

### 2. Modo Linha de Comando (Argumentos Diretos)
```bash
./gp -u <usuario>          # a) Listar processos de um determinado usuário
./gp -i <pid>              # b) Consultar informações detalhadas de um processo (PID, NICE, USER, TIME, STATUS)
./gp -b <pid>              # c.1) Bloquear (suspender) processo com SIGSTOP
./gp -c <pid>              # c.2) Continuar processo bloqueado com SIGCONT
./gp -e "<comando>"        # c.3) Executar um novo processo em background
./gp -r <pid>              # c.4) Reiniciar um processo existente
./gp -k <pid>              # c.5) Finalizar um processo (SIGTERM)
./gp -K <pid>              # c.5) Finalizar forçadamente um processo (SIGKILL)
./gp -p <pid> <nice>       # d.1) Trocar prioridade (NICE) de um processo em execução
./gp -n <nice> "<comando>" # d.2) Trocar prioridade (NICE) antes de executar um novo processo
```
