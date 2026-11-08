# ============================================================================
# Makefile - Gerenciador de Processos (GP) para GNU/Linux
# IFCE Campus Crato - Sistemas Distribuídos / Sistemas Operacionais
# ============================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -O2
TARGET  = gp
SRC     = gerenciador_processos.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
