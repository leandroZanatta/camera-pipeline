# Sistema de Logging com Detecção de Paradas

## Visão Geral

O sistema de logging foi aprimorado para incluir:
- **Logging em disco** com rotação automática de arquivos
- **Tracking de performance** em tempo real
- **Detecção automática de paradas** de processamento
- **Heartbeats** para monitoramento de atividade
- **Estatísticas detalhadas** de processamento

## Funcionalidades Principais

### 1. Logging em Disco
- Logs são salvos em arquivos separados por câmera: `camera_pipeline_{camera_id}.log`
- Rotação automática quando o arquivo atinge o tamanho máximo (padrão: 100MB)
- Timestamps com microssegundos para precisão
- Thread-safe para múltiplas câmeras

### 2. Tracking de Performance
- Medição de tempo de processamento para cada operação
- Contagem de frames processados
- Detecção de erros e warnings consecutivos
- Estatísticas de FPS real vs. estimado

### 3. Detecção de Paradas
- Monitoramento contínuo da atividade de cada câmera
- Detecção automática de paradas com timeout configurável (padrão: 30s)
- Alertas em tempo real para paradas críticas
- Logs específicos para recuperação de paradas

### 4. Heartbeats
- Sinais periódicos de atividade de cada componente
- Monitoramento de saúde do sistema
- Detecção de componentes travados

## Estrutura dos Logs

### Formato das Mensagens
```
2024-01-15 14:30:25.123456 [INFO  ] [Thread ID 1] Iniciada para URL: rtsp://...
2024-01-15 14:30:25.234567 [DEBUG ] [Heartbeat ID 1] stream_processor ativo
2024-01-15 14:30:25.345678 [INFO  ] [FPS Real ID 1] FPS de Entrada Decodificado (últimos 5.0s): 30.15
```

### Tipos de Mensagens Importantes

#### Paradas Detectadas
```
[Stall Detection ID 1] PARADA DETECTADA! Última atividade: 35.2s atrás, Último frame: 35.2s atrás
```

#### Erros Consecutivos
```
[Performance ID 1] 3 erros consecutivos detectados
```

#### Heartbeats
```
[Heartbeat ID 1] stream_processor ativo
```

#### Medições de FPS
```
[FPS Real ID 1] FPS de Entrada Decodificado (últimos 5.0s): 30.15
[FPS Real ID 1] FPS de Saída Calculado (últimos 5.0s): 29.87
```

## Configuração

### Inicialização do Logger
```c
// Inicializar com arquivo de log, tamanho máximo e tracking de performance
bool logger_init(const char* log_file_path, size_t max_file_size_mb, bool enable_performance_tracking);
```

### Exemplo de Uso
```c
// Inicializar logger para câmera 1
char log_filename[256];
snprintf(log_filename, sizeof(log_filename), "camera_pipeline_%d.log", camera_id);
logger_init(log_filename, 100, true); // 100MB max, performance tracking habilitado
```

## APIs Principais

### Logging de Atividade
```c
void log_activity(int camera_id, const char* activity_type, double processing_time_ms);
```

### Verificação de Paradas
```c
bool check_processing_stall(int camera_id, int timeout_seconds);
```

### Heartbeats
```c
void log_heartbeat(int camera_id, const char* component);
```

### Estatísticas de Performance
```c
bool get_performance_stats(int camera_id, performance_stats_t* stats);
```

## Macros de Conveniência

```c
// Log de atividade
LOG_ACTIVITY(camera_id, "frame", processing_time_ms);

// Heartbeat
LOG_HEARTBEAT(camera_id, "decoder");
```

## Monitoramento em Tempo Real

### Script Python de Monitoramento
```python
python test_logging_performance.py
```

### Análise de Logs
```python
python test_logging_performance.py analyze camera_pipeline_1.log
```

## Pontos de Integração no Código

### 1. Inicialização
- `run_camera_loop()`: Inicializa o logger para cada câmera

### 2. Tracking de Performance
- `process_stream()`: Logs de leitura, decodificação e dispatch
- `convert_and_dispatch_frame()`: Medição de tempo de processamento
- Heartbeats periódicos no loop principal

### 3. Detecção de Paradas
- Verificação automática a cada iteração do loop de processamento
- Timeout configurável (padrão: 30 segundos)
- Logs específicos para recuperação

## Análise de Logs

### Padrões de Parada
1. **Paradas Detectadas**: Mensagens com "PARADA DETECTADA"
2. **Erros Consecutivos**: 3+ erros em sequência
3. **Warnings Consecutivos**: 5+ warnings em sequência
4. **Gaps de Atividade**: Períodos sem logs > 10 segundos

### Métricas de Performance
- **FPS Real**: Medições a cada 5 segundos
- **Tempo de Processamento**: Média e máximo por operação
- **Taxa de Erro**: Erros por minuto
- **Uptime**: Tempo de atividade contínua

## Troubleshooting

### Problemas Comuns

1. **Logs não aparecem**
   - Verificar permissões de escrita no diretório
   - Confirmar inicialização do logger

2. **Paradas não detectadas**
   - Verificar timeout configurado
   - Confirmar chamadas de `log_activity()`

3. **Performance degradada**
   - Verificar tamanho do arquivo de log
   - Considerar reduzir nível de log

### Debug
```bash
# Verificar logs em tempo real
tail -f camera_pipeline_1.log

# Analisar estatísticas
python test_logging_performance.py analyze camera_pipeline_1.log

# Monitorar múltiplas câmeras
for i in {1..5}; do tail -f camera_pipeline_$i.log & done
```

## Configurações Recomendadas

### Para Produção
- Tamanho máximo de log: 100MB
- Timeout de parada: 30 segundos
- Nível de log: INFO
- Performance tracking: HABILITADO

### Para Desenvolvimento
- Tamanho máximo de log: 50MB
- Timeout de parada: 15 segundos
- Nível de log: DEBUG
- Performance tracking: HABILITADO

### Para Debug
- Tamanho máximo de log: 10MB
- Timeout de parada: 5 segundos
- Nível de log: TRACE
- Performance tracking: HABILITADO 