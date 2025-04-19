# Camera Pipeline Processor

Processador de Pipeline de Câmera com C/C++ e Python para processamento de streams de vídeo em tempo real.

## Requisitos do Sistema

- Python >= 3.10
- Bibliotecas do FFmpeg (libavcodec, libavformat, libavutil, libswscale, etc.)
- Compilador C/C++ (gcc/g++)
- CMake >= 3.10

## Instalação

### Instalação via GitHub

```bash
pip install git+https://github.com/leandroZanatta/camera-pipeline.git
```

### Instalação para Desenvolvimento

```bash
git clone https://github.com/leandroZanatta/camera-pipeline.git
cd camera-pipeline
pip install -e ".[dev]"
```

## Dependências

O pacote instala automaticamente:
- opencv-python==4.11.0.86 
- numpy>=1.26.4

## Uso Básico

```python
from camera_pipeline.core import CameraProcessor, SimpleFrameCallback, SimpleStatusCallback

# Callbacks para processar frames e atualizações de status
def process_frame(camera_id, frame):
    # O frame é um array NumPy
    print(f"Frame recebido da câmera {camera_id}, shape: {frame.shape}")

def update_status(camera_id, status_code, message):
    print(f"Status da câmera {camera_id}: {status_code} - {message}")

# Criar callbacks
frame_callback = SimpleFrameCallback(process_frame)
status_callback = SimpleStatusCallback(update_status)

# Inicializar o processador
processor = CameraProcessor()

# Registrar uma câmera (RTSP, arquivo de vídeo, etc.)
camera_id = processor.register_camera(
    url="rtsp://exemplo.com/stream",
    frame_callback=frame_callback,
    status_callback=status_callback
)

# Para parar uma câmera específica
processor.stop_camera(camera_id)

# Para desligar o processador quando terminar
processor.shutdown()
```

## Licença

Este projeto está licenciado sob a Licença MIT. 