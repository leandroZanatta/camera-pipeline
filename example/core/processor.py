import ctypes
import queue
import time
import threading
import logging
import numpy as np

# Importar definições da interface C
from .c_interface import (
    C_LIBRARY, IS_INTERFACE_READY, STATUS_CALLBACK_FUNC_TYPE, FRAME_CALLBACK_FUNC_TYPE,
    CallbackFrameData, AV_PIX_FMT_BGR24,
    LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARNING, LOG_LEVEL_ERROR, 
    STATUS_STOPPED, STATUS_CONNECTING, STATUS_CONNECTED, STATUS_DISCONNECTED # E outros estados se necessário
)

# Configurar logging para este módulo
# Use a configuração de logging do main.py ou de um módulo de config
logger = logging.getLogger(__name__) 
# logger.setLevel(logging.DEBUG) # Nível controlado externamente

class CameraProcessor:
    """
    Gerencia a interação com a biblioteca C para processar streams de múltiplas câmeras.
    """
    def __init__(self, c_log_level=LOG_LEVEL_INFO):
        if not IS_INTERFACE_READY:
            logger.critical("Biblioteca C não está carregada ou inicializada corretamente. Saindo.")
            raise ImportError("Falha ao carregar ou definir funções da biblioteca C.")
            
        self.c_lib = C_LIBRARY
        self._c_log_level = c_log_level
        # Remover frame_queue
        # self.frame_queue = queue.Queue(maxsize=300) 
        self.status_queue = queue.Queue(maxsize=100) # Fila para atualizações de status
        
        # Dicionário para armazenar o ÚLTIMO frame recebido por câmera
        self._latest_frames = {} 
        self._latest_frames_lock = threading.Lock()
        
        # Dicionário para armazenar informações sobre câmeras ativas:
        self._active_cameras = {} 
        self._processor_initialized = False
        # Usar um lock separado para o estado das câmeras/inicialização?
        # Ou usar o mesmo lock para _active_cameras e _latest_frames? 
        # Por simplicidade inicial, usar um único lock para ambos, mas pode ser separado se necessário.
        self._state_lock = threading.Lock() 

        # --- Callbacks Ctypes --- 
        self._c_status_callback_ref = STATUS_CALLBACK_FUNC_TYPE(self._c_status_callback)
        self._c_frame_callback_ref = FRAME_CALLBACK_FUNC_TYPE(self._c_frame_callback)

    def initialize_c_library(self):
        """Inicializa a biblioteca C globalmente (FFmpeg, etc.)."""
        with self._state_lock: # Usar o lock de estado
            if self._processor_initialized:
                logger.warning("Processador C já inicializado.")
                return True
            
            if not self.c_lib:
                logger.error("Biblioteca C não carregada.")
                return False
                
            try:
                logger.info(f"Configurando nível de log da C lib para: {self._c_log_level}")
                self.c_lib.logger_set_level(self._c_log_level)
                
                logger.info("Chamando processor_initialize...")
                ret = self.c_lib.processor_initialize()
                if ret == 0:
                    logger.info("Biblioteca C inicializada com sucesso.")
                    self._processor_initialized = True
                    return True
                else:
                    logger.error(f"Falha ao inicializar biblioteca C (processor_initialize retornou {ret}).")
                    self._processor_initialized = False # Garantir que está falso
                    return False
            except Exception as e:
                 logger.exception(f"Erro inesperado ao inicializar biblioteca C: {e}")
                 self._processor_initialized = False
                 return False

    # --- Callbacks (Executados pelas threads C) --- 

    def _c_status_callback(self, camera_id, status_code, message_ptr, user_data):
        """Callback C para status. Coloca na fila Python e atualiza estado interno."""
        try:
            message = message_ptr.decode('utf-8', 'ignore') if message_ptr else ""
            logger.debug(f"[Callback Status] Recebido: ID={camera_id}, Code={status_code}, Msg='{message}'")
            
            status_info = {
                'camera_id': camera_id,
                'status_code': status_code,
                'message': message
            }
            
            # Atualizar estado interno
            with self._state_lock: # Usar o lock de estado
                if camera_id in self._active_cameras:
                    self._active_cameras[camera_id]['status'] = status_code 

            self.status_queue.put_nowait(status_info)
            
        except queue.Full:
            logger.warning(f"Fila de status cheia para ID {camera_id}, descartando atualização.")
        except Exception as e:
            logger.exception(f"Erro inesperado no callback de status para ID {camera_id}: {e}")

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
            
            logger.debug(f"[Callback Frame] Recebido ID:{cam_id} {width}x{height} PTS:{pts} Linesize:{linesize} Format:{frame_data.format}")

            # Verificar se a câmera ainda está ativa antes de prosseguir
            # (Embora improvável dar problema, é uma segurança extra)
            with self._state_lock:
                if cam_id not in self._active_cameras:
                    logger.warning(f"[Callback Frame ID {cam_id}] Recebido frame para câmera inativa/removida. Descartando.")
                    if should_free_c_mem: self.c_lib.callback_pool_return_data(frame_data_ptr)
                    return

            # 2. Validar dados básicos
            if width <= 0 or height <= 0 or linesize <= 0 or not c_data_ptr:
                logger.error(f"[Callback Frame ID {cam_id}] Frame com dados/dims/linesize inválidos: {width}x{height}, L:{linesize}, Ptr:{c_data_ptr}")
                if should_free_c_mem: self.c_lib.callback_utils_free_data(frame_data_ptr)
                return
            if frame_data.format != AV_PIX_FMT_BGR24:
                logger.warning(f"[Callback Frame ID {cam_id}] Formato inesperado {frame_data.format}, esperado {AV_PIX_FMT_BGR24}.")
                if should_free_c_mem: self.c_lib.callback_utils_free_data(frame_data_ptr)
                return

            # 3. Calcular tamanho e obter ponteiro para dados do plano 0
            expected_bytes_per_pixel = 3
            buffer_size = height * linesize 
            if buffer_size <= 0:
                 logger.error(f"[Callback Frame ID {cam_id}] Tamanho de buffer inválido calculado: {buffer_size}")
                 if should_free_c_mem: self.c_lib.callback_utils_free_data(frame_data_ptr)
                 return
                 
            # 4. Criar buffer ctypes a partir do ponteiro C (APENAS para acesso)
            c_buffer = ctypes.cast(c_data_ptr, ctypes.POINTER(ctypes.c_uint8 * buffer_size)).contents

            # 5. Copiar dados para array NumPy
            frame_np = np.empty((height, width, expected_bytes_per_pixel), dtype=np.uint8)
            temp_view = np.frombuffer(c_buffer, dtype=np.uint8).reshape((height, linesize))
            # Copia os dados relevantes, ignorando o padding se houver
            frame_np[:, :, :] = temp_view[:, :width * expected_bytes_per_pixel].reshape((height, width, expected_bytes_per_pixel))
                 
            # 6. LIBERAR MEMÓRIA C IMEDIATAMENTE APÓS A CÓPIA!
            self.c_lib.callback_pool_return_data(frame_data_ptr)
            should_free_c_mem = False 
            frame_data_ptr = None 

            # 7. Criar dicionário Python com a cópia NumPy
            frame_info = {
                'frame': frame_np, 
                'pts': pts,
                'timestamp': time.time() 
            }
            
            # Armazenar no dicionário de último frame (SOBRESCREVER)
            with self._latest_frames_lock:
                self._latest_frames[cam_id] = frame_info
                # logger.debug(f"[Callback Frame ID {cam_id}] Último frame atualizado.") # Log verboso

        except queue.Full:
            logger.warning(f"[Callback Frame ID {cam_id_log}] Frame queue cheia. Frame descartado.")
            if should_free_c_mem:
                try: 
                    # Devolver ao pool mesmo em erro de fila
                    self.c_lib.callback_pool_return_data(frame_data_ptr)
                except Exception as free_err: logger.exception(f"Erro ao tentar liberar C frame após Queue Full (pré-cópia): {free_err}")
        except Exception as e:
            logger.exception(f"[Callback Frame ERROR ID {cam_id_log}] Exceção no callback de frame: {e}")
            if should_free_c_mem:
                try: 
                    # Devolver ao pool mesmo em erro de callback
                    self.c_lib.callback_pool_return_data(frame_data_ptr)
                except Exception as free_err: logger.exception(f"Erro ao tentar liberar C frame após exceção: {free_err}")

    # --- Métodos Públicos (Chamados pela thread principal Python) --- 

    def get_latest_frames(self) -> dict:
        """Retorna uma CÓPIA do dicionário contendo os últimos frames recebidos."""
        with self._latest_frames_lock:
            # Retorna uma cópia rasa, o suficiente pois os arrays NumPy já são cópias
            return dict(self._latest_frames)
            
    def start_camera(self, url: str, target_fps: int = 1) -> int:
        """
        Inicia o processamento de uma nova câmera.
        Retorna: O camera_id (>= 0) em sucesso, ou um código de erro C (< 0).
        """
        with self._state_lock: # Usar lock de estado
            if not self._processor_initialized:
                logger.error("Processador C não inicializado. Chame initialize_c_library() primeiro.")
                return -1 # Código de erro: não inicializado

            if not url:
                 logger.error("URL inválida fornecida para start_camera.")
                 return -3 # Código de erro: URL inválida
                 
            effective_target_fps = int(target_fps) if target_fps > 0 else 0
            logger.info(f"Solicitando adição da câmera: URL='{url}', TargetFPS={effective_target_fps}")
            
            try:
                c_url = url.encode('utf-8')
                # Usar as referências dos callbacks mantidas em self
                camera_id = self.c_lib.processor_add_camera(
                    c_url,
                    self._c_status_callback_ref, 
                    self._c_frame_callback_ref,
                    ctypes.py_object(self), # user_data para status_cb
                    ctypes.py_object(self), # user_data para frame_cb
                    ctypes.c_int(effective_target_fps)
                )

                if camera_id >= 0:
                    logger.info(f"Câmera adicionada via C com sucesso. ID: {camera_id}, URL: {url}")
                    # Armazenar informações básicas da câmera ativa
                    self._active_cameras[camera_id] = {
                        'url': url,
                        'target_fps': effective_target_fps,
                        'status': STATUS_CONNECTING # Estado inicial inferido
                    }
                    return camera_id
                else:
                    # Códigos de erro C: -1 (não inicializado), -2 (sem slots), -3 (URL inválida), -5 (erro thread)
                    logger.error(f"Falha ao adicionar câmera via C (Erro {camera_id}). URL: {url}")
                    return camera_id # Retorna o código de erro C

            except Exception as e:
                 logger.exception(f"Exceção Python ao chamar processor_add_camera para URL {url}: {e}")
                 return -99 # Código de erro genérico para exceção Python

    def stop_camera(self, camera_id: int) -> bool:
        """
        Solicita a parada de uma câmera específica via biblioteca C.
        Retorna: True se a C lib retornou 0 (sucesso), False caso contrário.
        """
        logger.info(f"Solicitando parada para câmera ID {camera_id}...")
        # Usar lock de estado para verificar se a câmera existe no rastreamento?
        with self._state_lock: 
             if camera_id not in self._active_cameras:
                  logger.warning(f"Tentando parar câmera ID {camera_id} que não está sendo rastreada pelo Python.")
                  # Não chamar C lib se não conhecemos? Ou chamar mesmo assim?
                  # return False # Mais seguro não chamar C lib para ID desconhecido

        # Se chegou aqui, a câmera existe (ou existia) no nosso rastreamento
        if not self._processor_initialized:
             logger.warning("Processador C não inicializado ao tentar parar câmera.")
             return False
        
        try:
             ret = self.c_lib.processor_stop_camera(camera_id)
             if ret == 0:
                  logger.info(f"Solicitação C de parada para ID {camera_id} enviada com sucesso.")
                  # O estado será atualizado pelo callback de status
                  return True
             else:
                  # Erros C: -1 (não inicializado), -2 (ID inválido/inativo)
                  logger.error(f"Falha ao solicitar parada C para ID {camera_id} (Erro {ret}).")
                  return False
        except Exception as e:
              logger.exception(f"Exceção Python ao chamar processor_stop_camera para ID {camera_id}: {e}")
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
            logger.warning("Processador C não estava inicializado ou C Lib não disponível.")

        # Limpar estado interno Python APÓS shutdown C
        with self._state_lock: 
            logger.info(f"Limpando estado interno Python ({len(self._active_cameras)} câmeras rastreadas).")
            self._active_cameras.clear()
            self._processor_initialized = False 
        
        # Limpar o buffer de últimos frames
        with self._latest_frames_lock:
            logger.info(f"Limpando buffer de últimos frames ({len(self._latest_frames)} câmeras).")
            self._latest_frames.clear()

        # Limpar APENAS a fila de status
        q_names = {'Status': self.status_queue}
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
                    break # Parar se houver erro
            logger.info(f"Fila '{name}' limpa ({cleared_count} itens removidos).")
            
        logger.info("Desligamento do CameraProcessor (Python) concluído.")
