import abc
from typing import Any, Optional, Union

try:
    import numpy as np
    HAS_NUMPY = True
    FrameType = Union[np.ndarray, bytearray]
except ImportError:
    HAS_NUMPY = False
    FrameType = bytearray

class FrameCallback(abc.ABC):
    """
    Interface para callbacks de processamento de frames.
    Implementações desta interface devem fornecer o método process_frame
    para processar os frames recebidos da câmera.
    """
    
    @abc.abstractmethod
    def process_frame(self, camera_id: int, frame: FrameType) -> None:
        """
        Processa um frame recebido de uma câmera.
        
        Args:
            camera_id: ID da câmera que gerou o frame
            frame: Dados do frame. Será numpy.ndarray se NumPy estiver disponível, 
                  ou bytearray caso contrário.
        """
        pass

class StatusCallback(abc.ABC):
    """
    Interface para callbacks de atualização de status.
    Implementações desta interface devem fornecer o método update_status
    para processar atualizações de status das câmeras.
    """
    
    @abc.abstractmethod
    def update_status(self, camera_id: int, status_code: int, message: str) -> None:
        """
        Processa uma atualização de status recebida de uma câmera.
        
        Args:
            camera_id: ID da câmera que gerou a atualização
            status_code: Código de status (conforme definido em c_interface)
            message: Mensagem de status ou descrição adicional
        """
        pass

# Classes de implementação simples para uso direto

class SimpleFrameCallback(FrameCallback):
    """
    Implementação simples de FrameCallback que delega para uma função.
    """
    
    def __init__(self, callback_func):
        """
        Inicializa com uma função de callback.
        
        Args:
            callback_func: Função que aceita (camera_id, frame) como parâmetros
        """
        self._callback = callback_func
        
    def process_frame(self, camera_id: int, frame: FrameType) -> None:
        """Chama a função de callback com os parâmetros recebidos."""
        self._callback(camera_id, frame)

class SimpleStatusCallback(StatusCallback):
    """
    Implementação simples de StatusCallback que delega para uma função.
    """
    
    def __init__(self, callback_func):
        """
        Inicializa com uma função de callback.
        
        Args:
            callback_func: Função que aceita (camera_id, status_code, message) como parâmetros
        """
        self._callback = callback_func
        
    def update_status(self, camera_id: int, status_code: int, message: str) -> None:
        """Chama a função de callback com os parâmetros recebidos."""
        self._callback(camera_id, status_code, message)
