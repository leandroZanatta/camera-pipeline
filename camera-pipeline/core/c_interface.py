import ctypes
import os
import platform
import sys

# --- Constantes C (espelhadas do C) ---
# Níveis de Log (devem corresponder a common_utils.h)
LOG_LEVEL_ERROR = 0
LOG_LEVEL_WARNING = 1
LOG_LEVEL_INFO = 2
LOG_LEVEL_DEBUG = 3
LOG_LEVEL_TRACE = 4

# Status codes (devem corresponder ao mapeamento em camera_thread.c)
STATUS_STOPPED = 0
STATUS_CONNECTING = 1
STATUS_CONNECTED = 2
STATUS_DISCONNECTED = 3
STATUS_WAITING_RECONNECT = 4
STATUS_RECONNECTING = 5
STATUS_UNKNOWN = -1
# STATUS_ERROR não é um estado direto, mas um código de erro geral que pode
# acompanhar STATUS_DISCONNECTED ou outro estado problemático.

# Formato de Pixel (Valor numérico de AV_PIX_FMT_BGR24 esperado do C)
# AV_PIX_FMT_BGR24 era 3 na maioria das versões recentes do FFmpeg.
# AV_PIX_FMT_RGB24 costuma ser 2.
AV_PIX_FMT_BGR24 = 3 # Corrigido de 2 para 3
# Formato de Pixel (Valor numérico de AV_PIX_FMT_YUV420P)
# É importante que a biblioteca C garanta a conversão para este formato.
# AV_PIX_FMT_YUV420P = 0 # Conferir este valor se houver problemas

# Define MAX_CAMERAS (deve ser consistente com C)
MAX_CAMERAS = 64
MAX_URL_LENGTH = 1024

# --- Tipos Ctypes --- 

# Definição do callback de status (Python -> C)
# void (*status_callback_t)(int camera_id, int status_code, const char* message, void* user_data);
STATUS_CALLBACK_FUNC_TYPE = ctypes.CFUNCTYPE(
    None,           # Tipo de retorno: void
    ctypes.c_int,   # camera_id
    ctypes.c_int,   # status_code
    ctypes.c_char_p,# message
    ctypes.py_object # user_data (passado como objeto Python)
)

# Nova estrutura callback_frame_data_t (Python <-> C)
# Deve corresponder EXATAMENTE a callback_utils.h
class CallbackFrameData(ctypes.Structure):
    _fields_ = [
        # Ordem EXATA como em callback_utils.h
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("format", ctypes.c_int), # Esperado ser AV_PIX_FMT_BGR24
        ("pts", ctypes.c_int64),
        ("camera_id", ctypes.c_int), # <<< ADICIONADO
        ("ref_count", ctypes.c_int), # <<< ADICIONADO (usado internamente no C)
        ("data", ctypes.POINTER(ctypes.c_uint8) * 4), # uint8_t* data[4];
        ("linesize", ctypes.c_int * 4),             # int linesize[4];
        ("data_buffer_size", ctypes.c_size_t * 4), # size_t data_buffer_size[4]; <<< ADICIONADO
    ]

# Definição do callback de frame (Python -> C)
# void (*frame_callback_t)(callback_frame_data_t* frame_data, void* user_data);
FRAME_CALLBACK_FUNC_TYPE = ctypes.CFUNCTYPE(
    None,                                # Tipo de retorno: void
    ctypes.POINTER(CallbackFrameData),   # frame_data (ponteiro para a estrutura)
    ctypes.py_object                     # user_data (passado como objeto Python)
)

# --- Carregamento da Biblioteca C --- 

def _find_library_path():
    """Tenta localizar a biblioteca C compilada."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    
    # Lista de possíveis locais para a biblioteca
    possible_locations = [
        # 1. Diretório camera_processor no pacote instalado
        os.path.join(script_dir, 'camera_processor'),
        
        # 2. No mesmo diretório do script (core/)
        script_dir,
        
        # 3. Diretório de build do C (para desenvolvimento)
        os.path.join(os.path.dirname(script_dir), 'c_src', 'build')
    ]
    
    if platform.system() == "Windows":
        lib_name = "camera_processor.dll"
    elif platform.system() == "Darwin": # macOS
        lib_name = "libcamera_processor.dylib"
    else: # Linux e outros
        lib_name = "libcamera_processor.so"
    
    # Tenta cada possível localização
    for location in possible_locations:
        full_path = os.path.join(location, lib_name)
        if os.path.exists(full_path):
            print(f"[C Interface] Biblioteca encontrada em: {full_path}")
            return full_path
    
    # Se não encontrou, retorna o caminho padrão (que mostrará mensagem de erro)
    default_path = os.path.join(possible_locations[0], lib_name)
    print(f"[C Interface] Biblioteca não encontrada. Tentando usar: {default_path}")
    return default_path

def _load_c_library(lib_path):
    """Carrega a DLL/SO e retorna a instância ou None em caso de falha."""
    if not os.path.exists(lib_path):
        print(f"ERRO FATAL: Biblioteca C não encontrada em {lib_path}."
              f" Compile a biblioteca C primeiro (cd ../c_src/build && make)", file=sys.stderr)
        return None
    try:
        c_lib = ctypes.CDLL(lib_path)
        print("[C Interface] Biblioteca C carregada com sucesso.")
        return c_lib
    except Exception as e:
        print(f"ERRO FATAL: Falha ao carregar a biblioteca C: {e}", file=sys.stderr)
        return None

def _define_c_functions(c_lib):
    """Define os argtypes e restype das funções C na instância carregada."""
    if not c_lib:
        return False
    try:
        # processor_initialize
        c_lib.processor_initialize.argtypes = [] # Sem argumentos
        c_lib.processor_initialize.restype = ctypes.c_int

        # processor_add_camera 
        c_lib.processor_add_camera.argtypes = [
            ctypes.c_char_p,            # url
            STATUS_CALLBACK_FUNC_TYPE,  # status_cb
            FRAME_CALLBACK_FUNC_TYPE,   # frame_cb
            ctypes.py_object,           # status_cb_user_data
            ctypes.py_object,           # frame_cb_user_data
            ctypes.c_int                # target_fps
        ]
        c_lib.processor_add_camera.restype = ctypes.c_int

        # processor_stop_camera 
        c_lib.processor_stop_camera.argtypes = [ctypes.c_int]
        c_lib.processor_stop_camera.restype = ctypes.c_int
        
        # processor_shutdown
        c_lib.processor_shutdown.argtypes = []
        c_lib.processor_shutdown.restype = ctypes.c_int
        
        # Adicionar nova função de retorno ao pool
        c_lib.callback_pool_return_data.argtypes = [ctypes.POINTER(CallbackFrameData)]
        c_lib.callback_pool_return_data.restype = None # void
        
        # logger_set_level 
        c_lib.logger_set_level.argtypes = [ctypes.c_int]

        print("[C Interface] Assinaturas das funções C definidas.") # Mensagem simplificada
        return True

    except AttributeError as e:
        print(f"ERRO FATAL: Função não encontrada na biblioteca C: {e}. "
              f"Verifique a compilação e exportação da biblioteca.", file=sys.stderr)
        return False
    except Exception as e:
         print(f"ERRO FATAL inesperado ao definir funções C: {e}", file=sys.stderr)
         return False

# --- Inicialização do Módulo --- 

# Carrega a biblioteca ao importar o módulo
_lib_path = _find_library_path()
C_LIBRARY = _load_c_library(_lib_path)
_functions_defined = False
if C_LIBRARY:
    _functions_defined = _define_c_functions(C_LIBRARY)

# Flag global para verificar se a interface está pronta
IS_INTERFACE_READY = C_LIBRARY is not None and _functions_defined

if not IS_INTERFACE_READY:
    print("AVISO: Interface da biblioteca C não está pronta. Algumas funcionalidades podem falhar.", file=sys.stderr) 

# Remover a função setup_c_functions duplicada/antiga se existir
# A definição agora é feita diretamente em _define_c_functions 