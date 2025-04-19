import numpy as np
import logging
from camera_pipeline.core import CameraProcessor, SimpleFrameCallback, SimpleStatusCallback

# Configurar logging
logging.basicConfig(level=logging.INFO, 
                   format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')

def main():
    print("Testando camera-pipeline com NumPy como dependência obrigatória")
    
    # Verificar versão do NumPy
    print(f"NumPy versão: {np.__version__}")
    
    # Função básica de callback para frames
    def frame_callback(camera_id, frame):
        print(f"Frame recebido da câmera {camera_id}, tipo: {type(frame)}")
        print(f"Shape do frame: {frame.shape}, dtype: {frame.dtype}")
    
    # Função básica de callback para status
    def status_callback(camera_id, status_code, message):
        print(f"Status da câmera {camera_id}: {status_code} - {message}")
    
    # Criar callbacks
    frame_cb = SimpleFrameCallback(frame_callback)
    status_cb = SimpleStatusCallback(status_callback)
    
    # Criar processador
    processor = CameraProcessor()
    
    # Verificar que o processador sempre tem NumPy disponível
    print(f"Processador tem NumPy: {processor.has_numpy}")
    
    # Desligar o processador
    processor.shutdown()
    print("Teste concluído com sucesso!")

if __name__ == "__main__":
    main() 