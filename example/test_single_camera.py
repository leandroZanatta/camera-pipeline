import ctypes
import cv2
import numpy as np
import time
import sys
import os
import queue

# Adiciona o diretório pai (camera_pipeline/python_src) ao sys.path
# para encontrar os módulos core e config
script_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(script_dir)
if parent_dir not in sys.path:
    sys.path.insert(0, parent_dir)

# Importa após ajustar o path
try:
    from core.c_interface import (
        C_LIBRARY, IS_INTERFACE_READY, CallbackFrameData,
        STATUS_CALLBACK_FUNC_TYPE, LOG_LEVEL_TRACE, FRAME_CALLBACK_FUNC_TYPE
    )
except ImportError as e:
    print(f"Erro ao importar de core.c_interface: {e}")
    print("Certifique-se de que a biblioteca C está compilada e que o PYTHONPATH está correto.")
    sys.exit(1)

# --- Configuração --- 
# Substitua pela URL da câmera que você quer testar
# TEST_CAMERA_URL = "rtsp://admin:protaadmin@192.168.11.226:554/cam/realmonitor?channel=1&subtype=0"
TEST_CAMERA_URL = "https://connect-369.servicestream.io:8050/6859a2981be2.m3u8"
# TEST_CAMERA_URL = "https://vms.protenet.com.br/hls/500.m3u8"

CAMERA_ID = 0 # ID esperado para a primeira câmera
MAX_WIDTH_DISPLAY = 800 # Largura máxima da janela OpenCV

# --- Fila para Comunicação Thread C -> Thread Principal Python ---
frame_display_queue = queue.Queue(maxsize=10) # Limita o buffer Python

# --- Callbacks --- 
def simple_status_callback(camera_id, status_code, message_bytes, user_data):
    try:
        message = message_bytes.decode('utf-8', errors='replace')
        print(f"[Callback Status] Cam ID: {camera_id}, Status Code: {status_code}, Mensagem: '{message}'")
    except Exception as e:
        print(f"[Callback Status] Erro no callback: {e}")

status_callback_instance = STATUS_CALLBACK_FUNC_TYPE(simple_status_callback)

# Variável global para sinalizar saída (simples, para este teste)
g_exit_requested = False

def simple_frame_callback(frame_data_ptr, user_data):
    """Callback C->Python: Copia dados BGR para fila Python."""
    global g_exit_requested, frame_display_queue
    log_prefix = "[Frame Callback]"
    print(f"{log_prefix} Callback chamado com ponteiro: {frame_data_ptr}") # Log Entrada Callback
    
    if not frame_data_ptr or g_exit_requested:
        print(f"{log_prefix} Ignorando callback (Ponteiro Nulo ou Saída Solicitada)")
        if C_LIBRARY and frame_data_ptr: C_LIBRARY.processor_free_callback_data(frame_data_ptr)
        return
        
    frame_to_free = frame_data_ptr # Guardar ponteiro para liberar no finally
    try:
        frame_data = frame_data_ptr.contents 
        
        width = frame_data.width
        height = frame_data.height
        linesize = list(frame_data.linesize)
        pts = frame_data.pts
        bgr_ptr = frame_data.data[0] # Obtém o ponteiro ctypes diretamente
        bgr_stride = linesize[0]
        
        print(f"{log_prefix} Dados recebidos: CamID={cam_id}, W={width}, H={height}, Stride={bgr_stride}, PTS={pts}") # Log Dados Recebidos
        
        if width > 0 and height > 0 and bgr_ptr:
            # print(f"[Frame Callback] Recebido frame PTS {pts}. Copiando dados...")
            bgr_image_copy = None
            try:
                # Cria array NumPy temporário referenciando dados C
                temp_bgr_data_np = np.ctypeslib.as_array(
                    ctypes.cast(bgr_ptr, ctypes.POINTER(ctypes.c_uint8)), 
                    shape=(height, bgr_stride)
                )
                print(f"{log_prefix} Array NumPy temporário criado (shape: {temp_bgr_data_np.shape})") # Log Array Temporário
                
                # Faz cópia profunda e ajusta formato/padding
                if bgr_stride > width * 3:
                    bgr_image_copy = np.copy(temp_bgr_data_np[:, :width*3]).reshape((height, width, 3))
                elif bgr_stride == width * 3:
                    bgr_image_copy = np.copy(temp_bgr_data_np).reshape((height, width, 3))
                else: # Stride inválido
                    print(f"{log_prefix} ERRO: Stride BGR ({bgr_stride}) menor que width*3 ({width*3}). Descartando.")
            
                print(f"{log_prefix} Cópia NumPy criada: {'Sim' if bgr_image_copy is not None else 'Não'}") # Log Resultado Cópia
                
                # Coloca a CÓPIA NumPy na fila
                if bgr_image_copy is not None:
                    frame_info = {
                        'pts': pts,
                        'image_data': bgr_image_copy # Array NumPy copiado!
                    }
                    try:
                        frame_display_queue.put_nowait(frame_info)
                        print(f"{log_prefix} Cópia colocada na fila (PTS: {pts})") # Log Fila
                    except queue.Full:
                        print(f"{log_prefix} Fila de display cheia (PTS: {pts}). Frame descartado.") # Log Fila Cheia
                # else: (Erro no stride já logado)
                    
            except Exception as e_copy:
                 print(f"{log_prefix} Erro ao criar/copiar array NumPy: {e_copy}")
                 import traceback
                 traceback.print_exc()
        else:
             print(f"{log_prefix} Dados de frame inválidos recebidos: W={width}, H={height}, Ptr={bgr_ptr}")

        # Removido: Toda a lógica de conversão e exibição OpenCV daqui
        # bgr_image = ...
        # cv2.imshow(...)
        # cv2.waitKey(1)
        
    except Exception as e:
         print(f"{log_prefix} Erro INESPERADO no callback: {e}")
         import traceback
         traceback.print_exc()
         
    finally:
        print(f"{log_prefix} Bloco finally - Liberando dados C ref {frame_to_free}...") # Log Finally
        # *** OBRIGATÓRIO: Liberar os dados C copiados ***
        if C_LIBRARY and frame_to_free:
             C_LIBRARY.processor_free_callback_data(frame_to_free)

frame_callback_instance = FRAME_CALLBACK_FUNC_TYPE(simple_frame_callback)

def main():
    global g_exit_requested
    if not IS_INTERFACE_READY:
        print("ERRO: Interface da biblioteca C não está pronta. Saindo.")
        return

    c_lib = C_LIBRARY

    # --- Inicialização --- 
    print("Inicializando processador C com callbacks...")
    ret = c_lib.processor_initialize(1, status_callback_instance, 
                                     frame_callback_instance, None) # Passa o callback de frame
    if ret != 0:
        print(f"ERRO: Falha ao inicializar processador C (código: {ret})")
        return
    
    c_lib.processor_set_log_level(LOG_LEVEL_TRACE) # Log máximo da C
    print(f"Nível de log C definido para TRACE.")

    # --- Adicionar Câmera --- 
    print(f"Adicionando câmera: {TEST_CAMERA_URL}")
    # Passar None como user_data no callback C, já que não precisamos dele aqui
    added_camera_id = c_lib.processor_add_camera(TEST_CAMERA_URL.encode('utf-8'), None) 
    if added_camera_id < 0:
        print(f"ERRO: Falha ao adicionar câmera na biblioteca C (código: {added_camera_id})")
        c_lib.processor_shutdown()
        return
    
    if added_camera_id != CAMERA_ID:
        print(f"AVISO: ID da câmera esperado {CAMERA_ID}, mas recebeu {added_camera_id}")
        # Continuar usando o ID recebido
        current_camera_id = added_camera_id
    else:
        current_camera_id = CAMERA_ID
        
    print(f"Câmera adicionada com ID: {current_camera_id}. Aguardando callbacks...")

    # --- Loop Principal (agora processa a fila) --- 
    window_name = f"Camera Teste (ID: {current_camera_id})"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    print("Pressione 'q' na janela de vídeo para sair.")

    print("--- Iniciando loop principal de processamento da fila ---") 
    loop_count = 0
    processed_frames = 0
    while not g_exit_requested:
        loop_count += 1
        frame_to_display = None
        try:
            # Tenta pegar um item da fila sem bloquear indefinidamente
            frame_info = frame_display_queue.get(timeout=0.05) 
            processed_frames += 1
            print(f"[Main Loop {loop_count}] Item retirado da fila: {list(frame_info.keys())}") # Log Item Retirado
            
            # Obter a cópia do array NumPy da fila
            bgr_image = frame_info.get('image_data')
            pts = frame_info.get('pts', -1)
            
            # print(f"Loop {loop_count}: Processando frame PTS {pts}")
            
            # --- Processamento e Exibição na Thread Principal --- 
            if bgr_image is not None and isinstance(bgr_image, np.ndarray):
                try:
                    # Removido: Lógica de recriar ponteiro e array NumPy
                    # bgr_ptr_addr = frame_info['bgr_ptr_addr']
                    # ... (código antigo de acesso a ponteiro) ...
                    
                    # Trabalha diretamente com a cópia recebida da fila
                    if bgr_image.shape[0] > 0 and bgr_image.shape[1] > 0: # Verifica se a cópia é válida
                        # Redimensionar para exibição se for muito grande
                        display_h, display_w = bgr_image.shape[:2]
                        if display_w > MAX_WIDTH_DISPLAY:
                            scale = MAX_WIDTH_DISPLAY / display_w
                            display_h = int(display_h * scale)
                            display_w = MAX_WIDTH_DISPLAY
                            bgr_image_display = cv2.resize(bgr_image, (display_w, display_h), interpolation=cv2.INTER_LINEAR)
                        else:
                            bgr_image_display = bgr_image
                            
                        print(f"[Main Loop {loop_count}] Exibindo imagem PTS {pts} (Shape: {bgr_image_display.shape})") # Log Exibição
                        cv2.imshow(window_name, bgr_image_display)
                    else:
                         print(f"[Main Loop {loop_count}] Imagem BGR recebida da fila é inválida (shape: {bgr_image.shape}).")

                except Exception as e_proc:
                    print(f"[Main Loop {loop_count}] Erro processando frame da fila (PTS {pts}): {e_proc}")
                    import traceback
                    traceback.print_exc()
            else:
                print(f"[Main Loop {loop_count}] Item inválido recebido da fila: {frame_info}")

        except queue.Empty:
            # print(".", end="") # Sem frame novo, normal
            pass 
        
        # Processar eventos da GUI e checar tecla 'q'
        key = cv2.waitKey(10) # Espera curta para manter responsivo
        # print(f"[Main Loop {loop_count}] cv2.waitKey(10) retornou: {key}") # Log pode ser verboso
        if key == ord('q') or key == -1: 
            if key == -1: print("[Main Loop] Janela fechada detectada.")
            else: print("[Main Loop] Tecla 'q' pressionada.")
            g_exit_requested = True
            break

    print(f"--- Saindo do loop principal (Total frames processados: {processed_frames}) --- ")
    # --- Shutdown --- 
    print("Encerrando...")
    cv2.destroyAllWindows()
    
    print(f"Removendo câmera ID {current_camera_id}...")
    c_lib.processor_remove_camera(current_camera_id)
    
    print("Desligando processador C...")
    c_lib.processor_shutdown()
    
    print("Teste concluído.")

if __name__ == '__main__':
    main() 