import ctypes
import ctypes.util
import logging
import os
import platform
import sys

logger = logging.getLogger(__name__)

# Constantes de nível de log (espelho das definições C)
LOG_LEVEL_QUIET = -8
LOG_LEVEL_PANIC = 0
LOG_LEVEL_FATAL = 8
LOG_LEVEL_ERROR = 16
LOG_LEVEL_WARNING = 24
LOG_LEVEL_INFO = 32
LOG_LEVEL_VERBOSE = 40
LOG_LEVEL_DEBUG = 48
LOG_LEVEL_TRACE = 56

# Constantes de formato de pixel (espelho das definições C)
AV_PIX_FMT_NONE = -1
AV_PIX_FMT_YUV420P = 0
AV_PIX_FMT_YUYV422 = 1
AV_PIX_FMT_RGB24 = 2
AV_PIX_FMT_BGR24 = 3
# Adicione outros formatos conforme necessário

# Constantes de status da câmera (espelho das definições C)
STATUS_STOPPED = 0
STATUS_CONNECTING = 1
STATUS_CONNECTED = 2
STATUS_DISCONNECTED = 3
STATUS_ERROR = 4
STATUS_RECONNECTING = 5
STATUS_BUFFERING = 6
STATUS_MAX_SLOTS = 7

# Variável global para armazenar a biblioteca carregada
C_LIBRARY = None
IS_INTERFACE_READY = False

def _find_and_load_library():
    """Tenta encontrar e carregar a biblioteca C compartilhada."""
    global C_LIBRARY, IS_INTERFACE_READY
    
    lib_names = ['camera_pipeline_c', 'libcamera_pipeline_c']
    system = platform.system()
    
    # Lista para armazenar todos os caminhos possíveis
    all_search_paths = []
    
    # Construir nomes de arquivo de biblioteca específicos do sistema
    for lib_name in lib_names:
        if system == "Windows":
            lib_filename = f"{lib_name}.dll"
        elif system == "Darwin": # macOS
            lib_filename = f"{lib_name}.dylib"
        else: # Linux e outros
            lib_filename = f"{lib_name}.so"
            
        # Caminhos onde procurar a biblioteca
        search_paths = [
            # Dentro do diretório do módulo
            os.path.join(os.path.dirname(__file__), lib_filename),
            # Dentro do subdiretório camera_processor
            os.path.join(os.path.dirname(__file__), 'camera_processor', lib_filename),
            # Caminho relativo para build local (útil durante desenvolvimento)
            os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'lib', lib_filename)),
            os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'camera_pipeline_c', 'build', lib_filename)),
            # Caminhos adicionais de build
            os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '_skbuild', f'linux-{platform.machine()}-{sys.version_info.major}.{sys.version_info.minor}', 'cmake-build', lib_filename)),
            os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '_skbuild', f'linux-{platform.machine()}-{sys.version_info.major}.{sys.version_info.minor}', 'cmake-install', lib_filename)),
        ]
        
        all_search_paths.extend(search_paths)

    # Adicionar busca padrão do sistema para os dois possíveis nomes
    for lib_name in lib_names:
        system_path = ctypes.util.find_library(lib_name)
        if system_path:
            all_search_paths.append(system_path)
            
    logger.debug(f"Procurando por biblioteca em: {all_search_paths}")
    
    # Tentar carregar a partir dos caminhos
    for path in all_search_paths:
        if os.path.exists(path):
            try:
                C_LIBRARY = ctypes.CDLL(path)
                logger.info(f"Biblioteca C carregada com sucesso de: {path}")
                IS_INTERFACE_READY = True
                return True
            except OSError as e:
                logger.warning(f"Falha ao carregar biblioteca de {path}: {e}")
        else:
            logger.debug(f"Biblioteca não encontrada em: {path}")

    logger.error(f"Biblioteca C não encontrada ou não pôde ser carregada.")
    return False

# --- Definição das Estruturas C --- 

class CallbackFrameData(ctypes.Structure):
    _fields_ = [
        ("camera_id", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("format", ctypes.c_int), # Usar enum AV_PIX_FMT_*
        ("pts", ctypes.c_int64),
        ("data", ctypes.POINTER(ctypes.c_uint8) * 4), # Dados do plano (até 4 planos)
        ("linesize", ctypes.c_int * 4)       # Linesize do plano (até 4 planos)
    ]

# --- Definição dos Tipos de Callback --- 

# void (*status_callback_t)(int camera_id, int status_code, const char* message, void* user_data);
STATUS_CALLBACK_FUNC_TYPE = ctypes.CFUNCTYPE(
    None,                   # Tipo de retorno (void)
    ctypes.c_int,           # camera_id
    ctypes.c_int,           # status_code
    ctypes.c_char_p,        # message
    ctypes.py_object        # user_data (passamos a instância do Processor)
)

# void (*frame_callback_t)(CallbackFrameData* frame_data, void* user_data);
FRAME_CALLBACK_FUNC_TYPE = ctypes.CFUNCTYPE(
    None,                                 # Tipo de retorno (void)
    ctypes.POINTER(CallbackFrameData),    # frame_data
    ctypes.py_object                      # user_data
)

# --- Configuração das Funções da Biblioteca --- 

def _define_c_functions():
    """Define argtypes e restype para as funções C."""
    if not C_LIBRARY:
        return

    try:
        # void logger_set_level(int level);
        C_LIBRARY.logger_set_level.argtypes = [ctypes.c_int]
        C_LIBRARY.logger_set_level.restype = None
        
        # int processor_initialize();
        C_LIBRARY.processor_initialize.argtypes = []
        C_LIBRARY.processor_initialize.restype = ctypes.c_int
        
        # int processor_add_camera(const char* url, status_callback_t status_cb, frame_callback_t frame_cb, void* status_user_data, void* frame_user_data, int target_fps);
        C_LIBRARY.processor_add_camera.argtypes = [
            ctypes.c_char_p,            # url
            STATUS_CALLBACK_FUNC_TYPE,  # status_cb
            FRAME_CALLBACK_FUNC_TYPE,   # frame_cb
            ctypes.py_object,           # status_user_data
            ctypes.py_object,           # frame_user_data
            ctypes.c_int                # target_fps
        ]
        C_LIBRARY.processor_add_camera.restype = ctypes.c_int
        
        # int processor_stop_camera(int camera_id);
        C_LIBRARY.processor_stop_camera.argtypes = [ctypes.c_int]
        C_LIBRARY.processor_stop_camera.restype = ctypes.c_int
        
        # int processor_shutdown();
        C_LIBRARY.processor_shutdown.argtypes = []
        C_LIBRARY.processor_shutdown.restype = ctypes.c_int
        
        # void callback_pool_return_data(CallbackFrameData* data);
        C_LIBRARY.callback_pool_return_data.argtypes = [ctypes.POINTER(CallbackFrameData)]
        C_LIBRARY.callback_pool_return_data.restype = None
        
        logger.info("Protótipos das funções C definidos com sucesso.")

    except AttributeError as e:
        global IS_INTERFACE_READY
        logger.error(f"Erro ao definir protótipo de função C: {e}. A biblioteca pode estar incompleta ou corrompida.")
        IS_INTERFACE_READY = False

# --- Inicialização --- 
# Tenta carregar a biblioteca e definir as funções ao importar o módulo
if _find_and_load_library():
    _define_c_functions()
else:
    logger.warning("Interface C não pôde ser inicializada. Funcionalidades dependentes não estarão disponíveis.")
