# Camera Pipeline (C + Python)

Este projeto implementa um pipeline para processar múltiplos streams de vídeo de câmeras (HLS/RTMP) usando uma biblioteca C de alta performance controlada por Python.

## Instalação

### Via pip (recomendado)

```bash
pip install camera-pipeline-processor
```

### Diretamente do GitHub

```bash
pip install git+https://github.com/leandroZanatta/camera-pipeline.git
```

### Instalação para Desenvolvimento

```bash
git clone https://github.com/leandroZanatta/camera-pipeline.git
cd camera-pipeline
pip install -e .
```

## Arquitetura

-   **`c_src/`**: Contém o código fonte da biblioteca compartilhada C (`libcamera_processor.so` / `.dll`) responsável por:
    -   Gerenciar conexões HLS/RTMP (usando bibliotecas como libcurl, librtmp - **NÃO USA LIBAVFORMAT**).
    -   Demuxar streams (ex: MPEG-TS - **NÃO USA LIBAVFORMAT**).
    -   Decodificar vídeo usando bibliotecas de codec diretas (ex: OpenH264 - **NÃO USA LIBAVCODEC**).
    -   Gerenciar threads (um por câmera).
    -   Implementar buffer de frames e lógica de reconexão.
    -   Enviar frames decodificados e status para Python via callbacks.
-   **`core/`**: Contém o código Python que:
    -   Usa `ctypes` para carregar e interagir com a biblioteca C.
    -   Fornece URLs das câmeras para a biblioteca C.
    -   Define funções de callback Python para receber frames (YUV) e status.
    -   (Opcional) Processa os frames recebidos usando NumPy/OpenCV.

## Como Usar

```python
from core.processor import CameraProcessor

# Inicializar o processador
processor = CameraProcessor()

# Adicionar câmeras
camera_id = processor.add_camera("rtmp://exemplo.com/stream")

# Definir callback para receber frames
def on_frame(camera_id, frame):
    # Processar o frame aqui
    pass

processor.set_frame_callback(on_frame)

# Iniciar o processamento
processor.start()

# Para parar
processor.stop()
```

## Como Construir e Executar Manualmente

1.  **Instalar Dependências C:**
    -   Instale as bibliotecas C necessárias para rede e decodificação (ex: `libcurl-dev`, `librtmp-dev`, `libopenh264-dev`). Ajuste de acordo com as bibliotecas escolhidas.
    -   Certifique-se de ter `cmake` e um compilador C (como `gcc`) instalados.

2.  **Compilar a Biblioteca C:**
    ```bash
    cd c_src
    mkdir build
    cd build
    cmake .. 
    make 
    cd ../.. 
    ```
    (Ajuste `CMakeLists.txt` para encontrar suas dependências C corretamente!)

3.  **Configurar Python:**
    -   (Opcional) Crie um ambiente virtual: `python -m venv venv && source venv/bin/activate`
    -   Instale dependências Python: `pip install -r requirements.txt`

4.  **Executar:**
    -   Execute seu código Python que importa e usa a biblioteca.

## TODOs Principais (Implementação C)

-   Implementar conexão HLS (curl + m3u8 parsing).
-   Implementar conexão RTMP (librtmp?).
-   Implementar demuxer TS.
-   Implementar demuxer RTMP/FLV.
-   Integrar decodificador(es) de vídeo (ex: OpenH264).
-   Implementar buffer de frames com sincronização (ring buffer, mutex, condition variables).
-   Refinar lógica de reconexão com backoff exponencial real.
-   Implementar tratamento de erro robusto.
-   Implementar conversão de cor (YUV->RGB) se necessário (em C ou Python).
