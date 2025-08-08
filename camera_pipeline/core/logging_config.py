import logging
from logging.handlers import RotatingFileHandler
import os
from typing import Optional

def setup_logging(
    log_dir: str = "logs",
    log_level: int = logging.INFO,
    log_to_console: bool = True,
    per_camera: bool = False,
    camera_id: Optional[int] = None,
    max_bytes: int = 100 * 1024 * 1024,  # 100MB
    backup_count: int = 5,
):
    """
    Configura o sistema de logging global do Python.
    Se per_camera=True e camera_id for fornecido, cria um arquivo de log separado por câmera.
    Caso contrário, usa um arquivo geral.
    """
    os.makedirs(log_dir, exist_ok=True)
    if per_camera and camera_id is not None:
        log_file = os.path.join(log_dir, f"camera_pipeline_{camera_id}.log")
    else:
        log_file = os.path.join(log_dir, "camera_pipeline.log")

    root_logger = logging.getLogger()
    root_logger.setLevel(log_level)

    # Remover handlers antigos para evitar duplicidade
    if root_logger.hasHandlers():
        root_logger.handlers.clear()

    formatter = logging.Formatter(
        fmt="%(asctime)s [%(levelname)8s] [%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    file_handler = RotatingFileHandler(
        log_file, maxBytes=max_bytes, backupCount=backup_count, encoding="utf-8"
    )
    file_handler.setFormatter(formatter)
    root_logger.addHandler(file_handler)

    if log_to_console:
        console_handler = logging.StreamHandler()
        console_handler.setFormatter(formatter)
        root_logger.addHandler(console_handler)

    logging.info(f"Logging configurado. Arquivo: {log_file} | Nível: {logging.getLevelName(log_level)}") 