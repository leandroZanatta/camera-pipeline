import ctypes
import queue
import time
import threading
import logging
import numpy as np
from typing import Callable, Dict, Any, Optional, Union

# Importar definições da interface C
from .c_interface import (
    C_LIBRARY,
    IS_INTERFACE_READY,
    STATUS_CALLBACK_FUNC_TYPE,
    FRAME_CALLBACK_FUNC_TYPE,
    CallbackFrameData,
    AV_PIX_FMT_BGR24,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
    STATUS_STOPPED,
    STATUS_CONNECTING,
    STATUS_CONNECTED,
    STATUS_DISCONNECTED,  # E outros estados se necessário
)

# Importar interfaces de callback
from .callbacks import (
    FrameCallback,
    StatusCallback,
    SimpleFrameCallback,
    SimpleStatusCallback,
    FrameType,
)

# Configurar logging para este módulo
# Use a configuração de logging do main.py ou de um módulo de config
logger = logging.getLogger(__name__)
# logger.setLevel(logging.DEBUG) # Nível controlado externamente

# Para compatibilidade com código existente
LegacyFrameCallbackFunc = Callable[[int, Any], None]
LegacyStatusCallbackFunc = Callable[[int, int, str], None]

# Tipos de callback
# Callback de frame: recebe (camera_id: int, frame: np.ndarray)
FrameCallbackType = Callable[[int, np.ndarray], None]

# Callback de status: recebe (camera_id: int, status_code: int, message: str)
StatusCallbackType = Callable[[int, int, str], None]


class CameraProcessor:
    """
    Gerencia a interação com a biblioteca C para processar streams de múltiplas câmeras.
    """

    def __init__(self, c_log_level=LOG_LEVEL_INFO):
        if not IS_INTERFACE_READY:
            logger.critical(
                "Biblioteca C não está carregada ou inicializada corretamente. Saindo."
            )
            raise ImportError("Falha ao carregar ou definir funções da biblioteca C.")

        self.c_lib = C_LIBRARY
        self._c_log_level = c_log_level
        self.status_queue = queue.Queue(maxsize=100)  # Fila para atualizações de status

        # Dicionário para armazenar o ÚLTIMO frame recebido por câmera
        self._latest_frames = {}
        self._latest_frames_lock = threading.Lock()

        # Dicionários para armazenar informações e callbacks
        self._active_cameras = {}
        self._frame_callbacks = {}
        self._status_callbacks = {}
        self._processor_initialized = False
        self._state_lock = threading.Lock()

        # --- Callbacks Ctypes ---
        self._c_status_callback_ref = STATUS_CALLBACK_FUNC_TYPE(self._c_status_callback)
        self._c_frame_callback_ref = FRAME_CALLBACK_FUNC_TYPE(self._c_frame_callback)

        # Inicializar a biblioteca C
        self.initialize_c_library()

    # Propriedade para compatibilidade com código anterior
    @property
    def has_numpy(self):
        """NumPy está sempre disponível agora como dependência obrigatória."""
        return True

    def initialize_c_library(self):
        """Inicializa a biblioteca C globalmente (FFmpeg, etc.)."""
        with self._state_lock:  # Usar o lock de estado
            if self._processor_initialized:
                logger.warning("Processador C já inicializado.")
                return True

            if not self.c_lib:
                logger.error("Biblioteca C não carregada.")
                return False

            try:
                logger.info(
                    f"Configurando nível de log da C lib para: {self._c_log_level}"
                )
                self.c_lib.logger_set_level(self._c_log_level)

                logger.info("Chamando processor_initialize...")
                ret = self.c_lib.processor_initialize()
                if ret == 0:
                    logger.info("Biblioteca C inicializada com sucesso.")
                    self._processor_initialized = True
                    return True
                else:
                    logger.error(
                        f"Falha ao inicializar biblioteca C (processor_initialize retornou {ret})."
                    )
                    self._processor_initialized = False  # Garantir que está falso
                    return False
            except Exception as e:
                logger.exception(f"Erro inesperado ao inicializar biblioteca C: {e}")
                self._processor_initialized = False
                return False

    def _c_status_callback(self, camera_id, status_code, message_ptr, user_data):
        """Callback C para status. Coloca na fila Python e atualiza estado interno."""
        try:
            message = message_ptr.decode("utf-8", "ignore") if message_ptr else ""
            logger.debug(
                f"[Callback Status] Recebido: ID={camera_id}, Code={status_code}, Msg='{message}'"
            )

            status_info = {
                "camera_id": camera_id,
                "status_code": status_code,
                "message": message,
            }

            # Atualizar estado interno
            with self._state_lock:
                if camera_id in self._active_cameras:
                    self._active_cameras[camera_id]["status"] = status_code

                # Chamar o callback de status registrado para esta câmera, se existir
                if (
                    camera_id in self._status_callbacks
                    and self._status_callbacks[camera_id] is not None
                ):
                    try:
                        self._status_callbacks[camera_id].update_status(
                            camera_id, status_code, message
                        )
                    except Exception as callback_error:
                        logger.error(
                            f"Erro ao executar callback de status para câmera ID {camera_id}: {callback_error}"
                        )

            self.status_queue.put_nowait(status_info)

        except queue.Full:
            logger.warning(
                f"Fila de status cheia para ID {camera_id}, descartando atualização."
            )
        except Exception as e:
            logger.exception(
                f"Erro inesperado no callback de status para ID {camera_id}: {e}"
            )

    def _c_frame_callback(self, frame_data_ptr, user_data):
        """
        Callback C para frames. Copia dados, libera memória C, armazena no buffer de último frame.
        """
        should_free_c_mem = bool(frame_data_ptr)
        cam_id_log = -1

        try:
            if not frame_data_ptr:
                logger.warning("[Callback Frame] Ponteiro de frame NULO recebido.")
                return

            frame_data = frame_data_ptr.contents
            cam_id = frame_data.camera_id
            cam_id_log = cam_id
            width = frame_data.width
            height = frame_data.height
            pts = frame_data.pts
            linesize = frame_data.linesize[0]
            c_data_ptr = frame_data.data[0]

            logger.debug(
                f"[Callback Frame] Recebido ID:{cam_id} {width}x{height} PTS:{pts} Linesize:{linesize} Format:{frame_data.format}"
            )

            # Verificar se a câmera ainda está ativa antes de prosseguir
            with self._state_lock:
                if cam_id not in self._active_cameras:
                    logger.warning(
                        f"[Callback Frame ID {cam_id}] Recebido frame para câmera inativa/removida. Descartando."
                    )
                    if should_free_c_mem:
                        self.c_lib.callback_pool_return_data(frame_data_ptr)
                    return

            # 2. Validar dados básicos
            if width <= 0 or height <= 0 or linesize <= 0 or not c_data_ptr:
                logger.error(
                    f"[Callback Frame ID {cam_id}] Frame com dados/dims/linesize inválidos: {width}x{height}, L:{linesize}, Ptr:{c_data_ptr}"
                )
                if should_free_c_mem:
                    self.c_lib.callback_pool_return_data(frame_data_ptr)
                return
            if frame_data.format != AV_PIX_FMT_BGR24:
                logger.warning(
                    f"[Callback Frame ID {cam_id}] Formato inesperado {frame_data.format}, esperado {AV_PIX_FMT_BGR24}."
                )
                if should_free_c_mem:
                    self.c_lib.callback_pool_return_data(frame_data_ptr)
                return

            # 3. Calcular tamanho e obter ponteiro para dados do plano 0
            expected_bytes_per_pixel = 3
            buffer_size = height * linesize
            if buffer_size <= 0:
                logger.error(
                    f"[Callback Frame ID {cam_id}] Tamanho de buffer inválido calculado: {buffer_size}"
                )
                if should_free_c_mem:
                    self.c_lib.callback_pool_return_data(frame_data_ptr)
                return

            # Criar array NumPy (NumPy é dependência obrigatória agora)
            # 4. Criar buffer ctypes a partir do ponteiro C (APENAS para acesso)
            c_buffer = ctypes.cast(
                c_data_ptr, ctypes.POINTER(ctypes.c_uint8 * buffer_size)
            ).contents

            # 5. Copiar dados para array NumPy
            frame_np = np.empty(
                (height, width, expected_bytes_per_pixel), dtype=np.uint8
            )
            temp_view = np.frombuffer(c_buffer, dtype=np.uint8).reshape(
                (height, linesize)
            )
            # Copia os dados relevantes, ignorando o padding se houver
            frame_np[:, :, :] = temp_view[
                :, : width * expected_bytes_per_pixel
            ].reshape((height, width, expected_bytes_per_pixel))
            frame_data_obj = frame_np

            # 6. LIBERAR MEMÓRIA C IMEDIATAMENTE APÓS A CÓPIA!
            self.c_lib.callback_pool_return_data(frame_data_ptr)
            should_free_c_mem = False
            frame_data_ptr = None

            # 7. Criar dicionário Python com a cópia dos dados
            frame_info = {
                "frame": frame_data_obj,
                "pts": pts,
                "timestamp": time.time(),
                "width": width,
                "height": height,
            }

            # Armazenar no dicionário de último frame (SOBRESCREVER)
            with self._latest_frames_lock:
                self._latest_frames[cam_id] = frame_info

            # Chamar o callback registrado para esta câmera - OBRIGATÓRIO TER UM CALLBACK
            with self._state_lock:
                if (
                    cam_id in self._frame_callbacks
                    and self._frame_callbacks[cam_id] is not None
                ):
                    try:
                        # Executar o callback com o frame
                        self._frame_callbacks[cam_id].process_frame(
                            cam_id, frame_data_obj
                        )
                    except Exception as callback_error:
                        logger.error(
                            f"Erro ao executar callback de frame para câmera ID {cam_id}: {callback_error}"
                        )
                else:
                    logger.warning(
                        f"Nenhum callback de frame registrado para câmera ID {cam_id}"
                    )

        except queue.Full:
            logger.warning(
                f"[Callback Frame ID {cam_id_log}] Frame queue cheia. Frame descartado."
            )
            if should_free_c_mem:
                try:
                    # Devolver ao pool mesmo em erro de fila
                    self.c_lib.callback_pool_return_data(frame_data_ptr)
                except Exception as free_err:
                    logger.exception(
                        f"Erro ao tentar liberar C frame após Queue Full (pré-cópia): {free_err}"
                    )
        except Exception as e:
            logger.exception(
                f"[Callback Frame ERROR ID {cam_id_log}] Exceção no callback de frame: {e}"
            )
            if should_free_c_mem:
                try:
                    # Devolver ao pool mesmo em erro de callback
                    self.c_lib.callback_pool_return_data(frame_data_ptr)
                except Exception as free_err:
                    logger.exception(
                        f"Erro ao tentar liberar C frame após exceção: {free_err}"
                    )

    def _adapt_frame_callback(self, callback):
        """Converte uma função de callback em um objeto FrameCallback."""
        if isinstance(callback, FrameCallback):
            return callback
        elif callable(callback):
            return SimpleFrameCallback(callback)
        else:
            return None

    def _adapt_status_callback(self, callback):
        """Converte uma função de callback em um objeto StatusCallback."""
        if callback is None:
            return None
        elif isinstance(callback, StatusCallback):
            return callback
        elif callable(callback):
            return SimpleStatusCallback(callback)
        else:
            return None

    def register_camera(
        self,
        camera_id: int,  # ID fornecido pelo Python
        url: str,
        frame_callback: Union[FrameCallback, LegacyFrameCallbackFunc],
        status_callback: Optional[
            Union[StatusCallback, LegacyStatusCallbackFunc]
        ] = None,
        target_fps: int = 1,
    ) -> int:
        """
        Registra uma nova câmera usando um ID fornecido externamente.

        Args:
            camera_id: O ID a ser usado para esta câmera (definido pelo chamador).
            url: URL da câmera (RTMP, HLS, RTSP, etc.)
            frame_callback: Interface FrameCallback ou função para frames (OBRIGATÓRIO).
            status_callback: Interface StatusCallback ou função para status (OPCIONAL).
            target_fps: Taxa de quadros alvo (0 para máxima)

        Retorna:
            0 em sucesso, ou um código de erro C (< 0).
        """
        # Validar que frame_callback foi fornecido
        if frame_callback is None:
            logger.error("Callback de frame é obrigatório")
            return -3

        adapted_frame_callback = self._adapt_frame_callback(frame_callback)
        if adapted_frame_callback is None:
            logger.error(
                "O callback de frame deve ser uma instância de FrameCallback ou uma função"
            )
            return -3

        adapted_status_callback = self._adapt_status_callback(status_callback)
        if status_callback is not None and adapted_status_callback is None:
            logger.error(
                "O callback de status deve ser uma instância de StatusCallback, uma função ou None"
            )
            return -3

        with self._state_lock:
            if not self._processor_initialized:
                logger.error(
                    "Processador C não inicializado. Chame initialize_c_library() primeiro."
                )
                return -1

            if not url:
                logger.error("URL inválida fornecida para register_camera.")
                return -3
            
            # Verificar se o ID já está em uso
            if camera_id in self._active_cameras:
                logger.error(f"Tentativa de registrar câmera com ID {camera_id} que já está ativo.")
                return -4 # Código de erro para ID duplicado

            effective_target_fps = int(target_fps) if target_fps > 0 else 0
            logger.info(
                f"Solicitando adição da câmera: ID={camera_id}, URL='{url}', TargetFPS={effective_target_fps}"
            )

            try:
                c_url = url.encode("utf-8")
                # Chamar a função C passando o camera_id
                ret = self.c_lib.processor_add_camera(
                    ctypes.c_int(camera_id), # Passa o ID fornecido
                    c_url,
                    self._c_status_callback_ref,
                    self._c_frame_callback_ref,
                    ctypes.py_object(self),  # user_data para status_cb
                    ctypes.py_object(self),  # user_data para frame_cb
                    ctypes.c_int(effective_target_fps),
                )

                if ret == 0:
                    logger.info(
                        f"Câmera ID {camera_id} adicionada via C com sucesso. URL: {url}"
                    )
                    # Armazenar informações usando o ID fornecido
                    self._active_cameras[camera_id] = {
                        "url": url,
                        "target_fps": effective_target_fps,
                        "status": STATUS_CONNECTING,
                    }
                    self._frame_callbacks[camera_id] = adapted_frame_callback
                    if adapted_status_callback is not None:
                        self._status_callbacks[camera_id] = adapted_status_callback
                    return 0 # Retorna 0 para sucesso
                else:
                    # Erros C: -1 (init), -3 (url), -4(id), -5 (thread), outros...
                    logger.error(
                        f"Falha ao adicionar câmera ID {camera_id} via C (Erro {ret}). URL: {url}"
                    )
                    return ret  # Retorna o código de erro C

            except Exception as e:
                logger.exception(
                    f"Exceção Python ao chamar processor_add_camera para ID {camera_id}, URL {url}: {e}"
                )
                return -99

    def stop_camera(self, camera_id: int) -> bool:
        """
        Solicita a parada de uma câmera específica via biblioteca C.
        IMPORTANTE: Esta função agora aguarda a thread finalizar COM TIMEOUT DE SEGURANÇA (3s)
        para evitar travamentos. Retorna True quando a câmera foi removida ou timeout atingido.
        Retorna: True se a C lib retornou 0 (sucesso), False caso contrário.
        """
        logger.info(f"Solicitando parada para câmera ID {camera_id} (com timeout de segurança)...")
        with self._state_lock:
            if camera_id not in self._active_cameras:
                logger.warning(
                    f"Tentando parar câmera ID {camera_id} que não está sendo rastreada pelo Python."
                )

        # Se chegou aqui, a câmera existe (ou existia) no nosso rastreamento
        if not self._processor_initialized:
            logger.warning("Processador C não inicializado ao tentar parar câmera.")
            return False

        try:
            logger.debug(f"Chamando processor_stop_camera (com timeout) para ID {camera_id}...")
            ret = self.c_lib.processor_stop_camera(camera_id)
            if ret == 0:
                logger.info(
                    f"Câmera ID {camera_id} removida com sucesso (thread finalizada ou timeout de segurança)."
                )
                # O estado será atualizado pelo callback de status

                # Remover os callbacks registrados E a entrada da câmera ativa
                with self._state_lock:
                    removed_items = []
                    if camera_id in self._active_cameras: # Verifica se realmente existe antes de tentar deletar
                        del self._active_cameras[camera_id]
                        removed_items.append("active_cameras")
                    if camera_id in self._frame_callbacks:
                        del self._frame_callbacks[camera_id]
                        removed_items.append("frame_callbacks")
                    if camera_id in self._status_callbacks:
                        del self._status_callbacks[camera_id]
                        removed_items.append("status_callbacks")
                    
                    if removed_items:
                        logger.debug(f"Estado Python limpo para ID {camera_id}: {', '.join(removed_items)}")

                return True
            elif ret == -1:
                logger.error(f"Falha ao parar câmera ID {camera_id}: Processador C não inicializado.")
                return False
            elif ret == -2:
                logger.warning(f"Falha ao parar câmera ID {camera_id}: ID inválido ou câmera já inativa.")
                # Limpar estado Python mesmo assim, caso esteja inconsistente
                with self._state_lock:
                    if camera_id in self._active_cameras:
                        del self._active_cameras[camera_id]
                        logger.debug(f"Estado Python limpo para ID {camera_id} (ID inválido no C)")
                    if camera_id in self._frame_callbacks:
                        del self._frame_callbacks[camera_id]
                    if camera_id in self._status_callbacks:
                        del self._status_callbacks[camera_id]
                return False
            elif ret == -3:
                logger.error(f"Falha ao parar câmera ID {camera_id}: Erro na finalização da thread.")
                return False
            else:
                # Erros C: outros códigos
                logger.error(
                    f"Falha ao parar câmera ID {camera_id}: Código de erro desconhecido ({ret})."
                )
                return False
        except Exception as e:
            logger.exception(
                f"Exceção Python ao chamar processor_stop_camera para ID {camera_id}: {e}"
            )
            return False

    def shutdown(self):
        """Desliga o processador C e limpa recursos Python."""
        logger.info("Iniciando desligamento do CameraProcessor (Python)...")
        # Chamar shutdown C primeiro
        if self._processor_initialized and self.c_lib:
            logger.info("Chamando processor_shutdown na biblioteca C...")
            try:
                ret = self.c_lib.processor_shutdown()
                if ret == 0:
                    logger.info("processor_shutdown C concluído com sucesso.")
                else:
                    logger.error(f"processor_shutdown C retornou um erro: {ret}")
            except Exception as e:
                logger.exception(f"Exceção Python ao chamar processor_shutdown C: {e}")
        else:
            logger.warning(
                "Processador C não estava inicializado ou C Lib não disponível."
            )

        # Limpar estado interno Python APÓS shutdown C
        with self._state_lock:
            logger.info(
                f"Limpando estado interno Python ({len(self._active_cameras)} câmeras rastreadas)."
            )
            self._active_cameras.clear()
            self._frame_callbacks.clear()
            self._status_callbacks.clear()
            self._processor_initialized = False

        # Limpar o buffer de últimos frames
        with self._latest_frames_lock:
            logger.info(
                f"Limpando buffer de últimos frames ({len(self._latest_frames)} câmeras)."
            )
            self._latest_frames.clear()

        # Limpar APENAS a fila de status
        q_names = {"Status": self.status_queue}
        for name, q in q_names.items():
            cleared_count = 0
            while not q.empty():
                try:
                    q.get_nowait()
                    cleared_count += 1
                except queue.Empty:
                    break
                except Exception as e_q:
                    logger.error(f"Erro ao limpar fila {name}: {e_q}")
                    break  # Parar se houver erro
            logger.info(f"Fila '{name}' limpa ({cleared_count} itens removidos).")

        logger.info("Desligamento do CameraProcessor (Python) concluído.")
