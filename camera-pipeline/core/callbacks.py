import abc

class FrameCallback(abc.ABC):
    
    @abc.abstractmethod
    def process_frame(self, camera_id: int, frame) -> None:
        pass

class StatusCallback(abc.ABC):
    @abc.abstractmethod
    def update_status(self, camera_id: int, status_code: int, message: str) -> None:
        pass
