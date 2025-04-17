#!/bin/bash

# Definir cores para saída
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # Sem cor

echo -e "${GREEN}=== Script de Build e Diagnóstico da Camera Pipeline ===${NC}"

# Definir diretórios
REPO_DIR=$(pwd)
C_SRC_DIR="$REPO_DIR/c_src"
PY_SRC_DIR="$REPO_DIR/python_src"
LOG_FILE="$REPO_DIR/camera_debug.log"

# Limpar log anterior
rm -f "$LOG_FILE"
touch "$LOG_FILE"

# Função para registrar mensagens no log
log() {
    echo -e "$1" | tee -a "$LOG_FILE"
}

# === PARTE 1: Compilar a biblioteca C ===
log "${YELLOW}[1/3] Compilando biblioteca C...${NC}"

# Verificar se time.h está instalado
if ! [ -f "/usr/include/time.h" ]; then
    log "${RED}ERRO: time.h não encontrado. Instale pacotes de desenvolvimento.${NC}"
    log "Execute: sudo apt-get install libc6-dev"
    exit 1
fi

# Compilar com logs detalhados
cd "$C_SRC_DIR" || exit 1
log "Diretório de build: $C_SRC_DIR"

# Criar diretório de build se não existir
mkdir -p build
cd build || exit 1

# Adicionar flags para CLOCK_MONOTONIC e CLOCK_REALTIME
# -D_POSIX_C_SOURCE=200809L define constantes POSIX
cmake .. -DCMAKE_C_FLAGS="-D_POSIX_C_SOURCE=200809L -Wall -g -O0" 2>&1 | tee -a "$LOG_FILE"
make VERBOSE=1 2>&1 | tee -a "$LOG_FILE"

if [ $? -ne 0 ]; then
    log "${RED}Erro na compilação!${NC}"
    exit 1
else
    log "${GREEN}Biblioteca C compilada com sucesso!${NC}"
fi

# === PARTE 2: Adicionar Logs Temporários ao Python ===
log "${YELLOW}[2/3] Preparando ambiente Python...${NC}"

# Voltar ao diretório raiz
cd "$REPO_DIR" || exit 1

# Verificar se OpenCV e PySide6 estão instalados
python3 -c "import cv2, PySide6" 2>/dev/null
if [ $? -ne 0 ]; then
    log "${RED}ERRO: Bibliotecas Python necessárias não encontradas${NC}"
    log "Execute: pip install opencv-python PySide6 numpy"
    exit 1
fi

# === PARTE 3: Executar o programa com logs ===
log "${YELLOW}[3/3] Executando programa com monitoramento...${NC}"

# Executar o programa Python com timeout (impede que trave indefinidamente)
cd "$PY_SRC_DIR" || exit 1

# Definir timeout de 30 segundos (ajustar se necessário)
timeout 30s python3 -u main.py 2>&1 | tee -a "$LOG_FILE" &
PID=$!

# Monitorar uso de CPU e memória durante execução
log "Monitorando processo Python (PID: $PID)..."
sleep 2 # Espera inicializar

for i in {1..10}; do
    if ps -p $PID > /dev/null; then
        log "== DIAGNÓSTICO ($i) =="
        ps -p $PID -o %cpu,%mem,state,time | tee -a "$LOG_FILE"
        
        # Mostrar threads do processo
        log "Threads:"
        ps -T -p $PID | tee -a "$LOG_FILE"
        
        # Intervalo entre verificações
        sleep 2
    else
        break
    fi
done

# Aguardar conclusão ou timeout
wait $PID
EXIT_CODE=$?

if [ $EXIT_CODE -eq 124 ]; then
    log "${RED}Programa encerrado por timeout! (possível travamento)${NC}"
    # Capturar trace para análise
    log "== STACK TRACE =="
    gdb -p $PID -ex "thread apply all bt" -ex "quit" 2>&1 | tee -a "$LOG_FILE" || true
    kill -9 $PID 2>/dev/null || true
elif [ $EXIT_CODE -ne 0 ]; then
    log "${RED}Programa encerrado com erro (código $EXIT_CODE)${NC}"
else
    log "${GREEN}Programa concluído com sucesso${NC}"
fi

# === ANÁLISE FINAL ===
log "${YELLOW}=== ANÁLISE DOS LOGS ===${NC}"

# Verificar se conseguiu conectar à câmera
log "1. Status da conexão da câmera:"
grep -E "CONNECTING|CONNECTED|DISCONNECTED" "$LOG_FILE" | tail -5 | tee -a "$LOG_FILE"

# Verificar se frames foram decodificados
log "2. Frames decodificados:"
grep "Frame decoded" "$LOG_FILE" | wc -l | tee -a "$LOG_FILE"

# Verificar se frames chegaram à GUI
log "3. Frames processados pela GUI:"
grep "\[DEBUG\] Processados" "$LOG_FILE" | tail -1 | tee -a "$LOG_FILE"

log "${GREEN}Análise completa! Verifique o arquivo $LOG_FILE para detalhes${NC}"
