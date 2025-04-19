import abc
from typing import Any, Optional, Union, Callable
import numpy as np

# NumPy agora é uma dependência obrigatória
FrameType = np.ndarray

class FrameCallback(abc.ABC):
    """
    Interface para callbacks de processamento de frames.
    """
    
    @abc.abstractmethod
    def process_frame(self, camera_id: int, frame: FrameType) -> None:
        """
        Processa um frame recebido de uma câmera.
        
        Args:
            camera_id: ID da câmera que gerou o frame.
            frame: Dados do frame (np.ndarray).
        """
        pass

class StatusCallback(abc.ABC):
    """
    Interface para callbacks de atualização de status.
    """
    
    @abc.abstractmethod
    def update_status(self, camera_id: int, status_code: int, message: str) -> None:
        """
        Processa uma atualização de status recebida de uma câmera.
        
        Args:
            camera_id: ID da câmera que gerou a atualização.
            status_code: Código de status.
            message: Mensagem de status.
        """
        pass

# Classes de implementação simples para uso direto

class SimpleFrameCallback(FrameCallback):
    """
    Implementação simples de FrameCallback que delega para uma função.
    """
    def __init__(self, callback_func: Callable[[int, FrameType], None]):
        self._callback = callback_func
        
    def process_frame(self, camera_id: int, frame: FrameType) -> None:
        self._callback(camera_id, frame)

class SimpleStatusCallback(StatusCallback):
    """
    Implementação simples de StatusCallback que delega para uma função.
    """
    def __init__(self, callback_func: Callable[[int, int, str], None]):
        self._callback = callback_func
        
    def update_status(self, camera_id: int, status_code: int, message: str) -> None:
        self._callback(camera_id, status_code, message)
