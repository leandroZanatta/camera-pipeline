import sys
import signal

from PySide6.QtWidgets import QApplication

from config import AppConfig, MAX_CAMERAS, C_LOG_LEVEL
from core.processor import CameraProcessor
from gui.main_window import MainWindow

# --- Variáveis Globais --- 
app = None
processor = None
main_window = None

def signal_handler(sig, frame):
    """Lida com sinais de interrupção (Ctrl+C) para desligamento limpo."""
    print(f"\nSinal {sig} recebido. Desligando...")
    if main_window:
        # Tenta fechar a janela da GUI primeiro, que sinalizará o processador
        main_window.close()
    elif processor:
        # Se a GUI não estiver ativa, desliga o processador diretamente
        processor.shutdown()
    if app:
        app.quit()
    sys.exit(0)

def main():
    global app, processor, main_window, config

    # Registrar signal handlers para Ctrl+C
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Carregar configuração usando AppConfig
    config = AppConfig()
    camera_urls = config.get_camera_urls() # Obter lista de URLs
    camera_data = config.get_camera_data() # Obter dados completos (inclui nomes, se houver)

    if not camera_urls:
        print("Nenhuma URL de câmera encontrada no servidor ou erro ao carregar. Saindo.")
        sys.exit(1)

    # Inicializar o processador da câmera
    try:
        processor = CameraProcessor(c_log_level=C_LOG_LEVEL)
        if not processor.initialize_c_library():
            print("Falha ao inicializar a biblioteca C. Saindo.")
            sys.exit(1)
    except Exception as e:
        print(f"Erro fatal ao inicializar CameraProcessor: {e}")
        sys.exit(1)

    # Inicializar aplicação Qt
    app = QApplication(sys.argv)

    # Adicionar câmeras e obter IDs
    initial_cameras_for_gui = []
    cameras_started_count = 0
    # Iterar sobre os dados completos para ter acesso ao nome e URL
    for cam_info in camera_data:
        cam_url = cam_info.get('url')
        # Tenta obter um nome, usa URL como fallback
        cam_name = cam_info.get('name', cam_url) 
        
        if cam_url:
            # Chamar start_camera e obter o ID real ou erro
            # Poderíamos definir um target_fps aqui se quiséssemos: target_fps=5
            camera_id = processor.start_camera(cam_url) 
            
            if camera_id >= 0:
                 # Adicionar à lista para a GUI usando o ID retornado
                 initial_cameras_for_gui.append({'id': camera_id, 'name': cam_name})
                 cameras_started_count += 1
                 print(f"INFO: Câmera '{cam_name}' iniciada via C. ID atribuído: {camera_id}")
                 # Não sair do loop, tentar iniciar todas
            else:
                 # Logar o erro retornado pela C lib
                 print(f"AVISO: Falha ao iniciar câmera '{cam_name}' ({cam_url}). Erro C: {camera_id}")
        else:
            print(f"AVISO: Ignorando câmera sem URL: {cam_info}")

    if cameras_started_count == 0: # Verificar se ALGUMA câmera iniciou
         print("ERRO: Nenhuma câmera foi iniciada com sucesso. Verifique as URLs e logs.")
         if processor:
              processor.shutdown()
         sys.exit(1)

    # Criar e mostrar a janela principal
    main_window = MainWindow(
        processor=processor, # Passar a instância do processor
        status_queue=processor.status_queue,
        # Não passar mais a c_lib para a GUI
        initial_cameras=initial_cameras_for_gui
    )
    
    # Conectar sinal de parada da janela ao desligamento do processador
    main_window.stop_signal.connect(processor.shutdown)
    
    main_window.show()

    # Executar o loop de eventos da aplicação
    print("Iniciando loop de eventos da aplicação...")
    exit_code = app.exec()
    print("Loop de eventos finalizado.")
    
    # Garantir desligamento final (caso a janela não tenha sido fechada corretamente)
    if processor:
         print("Garantindo desligamento final do processador...")
         processor.shutdown()

    sys.exit(exit_code)

if __name__ == '__main__':
    main() 