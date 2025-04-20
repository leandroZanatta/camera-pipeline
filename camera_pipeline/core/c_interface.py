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

def _load_c_library():
    """Carrega a biblioteca C principal a partir do diretório do pacote."""
    global C_LIBRARY, IS_INTERFACE_READY

    if C_LIBRARY is not None:
        return True # Já carregada

    lib_filename = ""
    system = platform.system()
    # Usar o nome base sem 'lib' pois o CMakeLists remove o prefixo no Linux
    lib_base_name = 'camera_pipeline_c' 

    if system == "Windows":
        lib_filename = f"{lib_base_name}.dll"
    elif system == "Darwin": # macOS
        lib_filename = f"{lib_base_name}.dylib"
    else: # Linux e outros
        lib_filename = f"{lib_base_name}.so"

    # Caminho esperado da biblioteca dentro do pacote instalado
    # __file__ é o caminho para c_interface.py
    # os.path.dirname(__file__) é o diretório camera_pipeline/core/
    expected_path = os.path.join(os.path.dirname(__file__), lib_filename)

    logger.debug(f"Tentando carregar a biblioteca C de: {expected_path}")

    try:
        if os.path.exists(expected_path):
            # Carregar a biblioteca. O dynamic linker do SO cuidará das dependências (FFmpeg).
            # Remover RTLD_GLOBAL para teste
            C_LIBRARY = ctypes.CDLL(expected_path)
            logger.info(f"Biblioteca C carregada com sucesso de: {expected_path}")
            IS_INTERFACE_READY = True
            return True
        else:
            logger.error(f"Arquivo da biblioteca C não encontrado em: {expected_path}")
            return False
    except OSError as e:
        logger.error(f"Falha ao carregar biblioteca C de {expected_path}: {e}", exc_info=True)
        # Tentar obter informações sobre dependências ausentes (útil para debug)
        if system == "Linux":
            try:
                logger.error("Verificando dependências ausentes com ldd...")
                result = os.popen(f"ldd {expected_path}").read()
                logger.error(f"Resultado do ldd para {expected_path}:\n{result}")
            except Exception as ldd_e:
                 logger.error(f"Falha ao executar ldd: {ldd_e}")
        return False

# --- Tentar carregar a biblioteca na inicialização do módulo ---
if not _load_c_library():
    logger.critical("Interface C não pôde ser inicializada. Funcionalidades dependentes não estarão disponíveis.")
    # Você pode querer levantar uma exceção aqui se a biblioteca for absolutamente essencial
    # raise ImportError("Falha crítica ao inicializar a interface C.")

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
        
        # int processor_add_camera(int camera_id, const char* url, status_callback_t status_cb, frame_callback_t frame_cb, void* status_user_data, void* frame_user_data, int target_fps);
        C_LIBRARY.processor_add_camera.argtypes = [
            ctypes.c_int,               # camera_id (NOVO PRIMEIRO ARGUMENTO)
            ctypes.c_char_p,            # url
            STATUS_CALLBACK_FUNC_TYPE,  # status_cb
            FRAME_CALLBACK_FUNC_TYPE,   # frame_cb
            ctypes.py_object,           # status_user_data
            ctypes.py_object,           # frame_user_data
            ctypes.c_int                # target_fps
        ]
        C_LIBRARY.processor_add_camera.restype = ctypes.c_int # Deve retornar 0 em sucesso, < 0 em erro
        
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
if _load_c_library():
    _define_c_functions()
else:
    logger.warning("Interface C não pôde ser inicializada. Funcionalidades dependentes não estarão disponíveis.")
