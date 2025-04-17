import sys
import time
import queue
import math

from PySide6.QtCore import Qt, QTimer, QSize, Signal, Slot
from PySide6.QtGui import QColor, QFont, QPixmap, QPainter # Adicionar QPixmap, QPainter
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QGridLayout, 
    QScrollArea, QPushButton, QDialog, QListWidget, QListWidgetItem, 
    QHBoxLayout, QMessageBox, QLabel # Adicionar QLabel
)

from .video_widget import VideoWidget
from core.c_interface import (
    STATUS_STOPPED, STATUS_CONNECTING, STATUS_CONNECTED, STATUS_DISCONNECTED, 
    STATUS_WAITING_RECONNECT, STATUS_RECONNECTING, STATUS_UNKNOWN, 
    CallbackFrameData # Usar o nome correto
)

class MainWindow(QMainWindow):
    """Janela principal que exibe os vídeos das câmeras em um grid dinâmico."""

    # Sinais para comunicação externa (se necessário, mas não usados atualmente)
    # add_camera_signal = Signal(str, str) # name, url
    # remove_camera_signal = Signal(int)   # camera_id
    stop_signal = Signal() # Sinaliza para o main.py que a janela está fechando

    def __init__(self, processor, status_queue, initial_cameras=[], parent=None):
        super().__init__(parent)
        self.setWindowTitle("Camera Pipeline Viewer")
        self.setGeometry(100, 100, 1280, 720)

        # self._frame_queue = frame_queue # Não precisa mais da fila
        self._processor = processor # Armazenar instância do processor
        self._status_queue = status_queue

        # Widgets de vídeo ativos (camera_id -> VideoWidget)
        self._video_widgets = {}
        self._widget_order = [] # Mantém a ordem para layout
        self._camera_status = {} # Mantém último estado conhecido (para botão de falha)
        
        # --- Layout Principal --- 
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)
        main_layout.setContentsMargins(5, 5, 5, 5)
        main_layout.setSpacing(5)

        # Botão de Visualizar Falhas
        self._failed_cameras_btn = QPushButton(" Visualizar Câmeras com Falha")
        # Adicionar um ícone (opcional, requer um arquivo de ícone)
        # self._failed_cameras_btn.setIcon(QIcon("path/to/error_icon.png")) 
        self._failed_cameras_btn.setStyleSheet("padding: 6px; text-align: left;")
        self._failed_cameras_btn.clicked.connect(self._show_failed_cameras_dialog)
        main_layout.addWidget(self._failed_cameras_btn)

        # Área de Scroll para o Grid de Vídeos
        self._scroll_area = QScrollArea()
        self._scroll_area.setWidgetResizable(True)
        self._scroll_area.setStyleSheet("background-color: #282828; border: 1px solid #444;") # Cor de fundo escura
        main_layout.addWidget(self._scroll_area, 1) # Ocupa espaço restante

        # Widget para conter o grid dentro do scroll
        self._grid_container = QWidget()
        self._grid_container.setStyleSheet("background-color: transparent;") # Fundo transparente
        self._grid_layout = QGridLayout(self._grid_container)
        self._grid_layout.setSpacing(5) # Espaçamento entre widgets
        self._grid_layout.setContentsMargins(5, 5, 5, 5)
        self._scroll_area.setWidget(self._grid_container)
        
        # --- Timers --- 
        # Renomear timer e conectar à nova função
        self._update_timer = QTimer(self)
        self._update_timer.timeout.connect(self._update_gui_frames) 
        self._update_timer.start(50) # Atualizar ~20 FPS (ajuste conforme necessário)
        
        # Timer para processar a fila de status (menos frequente)
        self._status_timer = QTimer(self)
        self._status_timer.timeout.connect(self._process_status_queue)
        self._status_timer.start(200) # Checa status 5x por segundo

        # Timer para monitorar saúde (checa se widgets receberam frames)
        self._health_timer = QTimer(self)
        self._health_timer.timeout.connect(self._check_camera_health)
        self._health_timer.start(5000) # Checa a cada 5 segundos

        # Adicionar câmeras iniciais
        for cam in initial_cameras:
            # Adiciona o widget visualmente
            self.add_camera_widget(cam['id'], cam['name']) 
            # Define um status inicial (a C lib pode atualizar logo em seguida)
            self._update_camera_status(cam['id'], STATUS_CONNECTING, "Iniciando...") 

        self._update_grid_layout() # Organiza o layout inicial
        self.statusBar().showMessage("Pronto.")
        print("Interface gráfica inicializada.")

    # Esta função é chamada pelo main.py ou outra lógica para criar o widget
    # Não é um Slot direto, mas um método público.
    def add_camera_widget(self, camera_id, name):
        """Adiciona um widget de vídeo para uma nova câmera na GUI."""
        if camera_id in self._video_widgets:
            print(f"GUI: Widget para camera ID {camera_id} já existe.")
            return

        print(f"GUI: Adicionando widget para camera ID {camera_id} ({name})")
        # Cria o widget, passando o container do grid como pai
        widget = VideoWidget(self._grid_container, camera_id=camera_id, camera_name=name)
        self._video_widgets[camera_id] = widget
        if camera_id not in self._widget_order: # Evita duplicatas se chamado novamente
            self._widget_order.append(camera_id) 
        if camera_id not in self._camera_status: # Garante entrada de status inicial
             self._camera_status[camera_id] = {'status': STATUS_CONNECTING, 'message': 'Adicionando...'} 
        self._update_grid_layout()

    # Função interna para remover o widget da GUI (chamada por ex: closeEvent)
    def _remove_camera_widget(self, camera_id):
        """Remove o widget de vídeo da interface e agenda para deleção."""
        if camera_id in self._video_widgets:
            print(f"GUI: Removendo widget visual para camera ID {camera_id}.")
            widget = self._video_widgets.pop(camera_id)
            if camera_id in self._widget_order:
                 self._widget_order.remove(camera_id)
            self._camera_status.pop(camera_id, None)
            widget.setParent(None) # Remove do layout
            widget.deleteLater() # Agenda para deleção segura pelo Qt
            self._update_grid_layout() # Reorganiza o grid
        else:
            print(f"GUI: Widget visual para camera ID {camera_id} não encontrado para remoção.")
            
    def _update_grid_layout(self):
        """Reorganiza os widgets de vídeo no grid dinamicamente."""
        # Limpar layout antigo completamente
        for i in reversed(range(self._grid_layout.count())):
            widget_item = self._grid_layout.itemAt(i)
            if widget_item is not None:
                widget = widget_item.widget()
                if widget:
                    widget.setParent(None) # Essencial para remover do layout
                # self._grid_layout.removeItem(widget_item)
                # A linha acima pode não ser necessária se setParent(None) funcionar
                # Mas é mais seguro remover explicitamente após setParent(None)
                self._grid_layout.takeAt(i)
                 
        num_widgets = len(self._widget_order)
        if num_widgets == 0:
            # Adicionar um QLabel indicando que não há câmeras
            no_cam_label = QLabel("Nenhuma câmera adicionada.")
            no_cam_label.setAlignment(Qt.AlignCenter)
            no_cam_label.setStyleSheet("color: gray; font-size: 16px;")
            self._grid_layout.addWidget(no_cam_label, 0, 0)
            return

        # Calcular número de colunas (tenta fazer um grid mais agradável)
        cols = math.isqrt(num_widgets) # Raiz quadrada inteira
        if cols * cols < num_widgets: cols += 1
        if cols == 0: cols = 1
        
        # Adicionar widgets ao grid na ordem definida
        print(f"Atualizando grid: {num_widgets} widgets, {cols} colunas.")
        for i, cam_id in enumerate(self._widget_order):
            row = i // cols
            col = i % cols
            if cam_id in self._video_widgets:
                self._grid_layout.addWidget(self._video_widgets[cam_id], row, col)
            else:
                 print(f"AVISO: Cam ID {cam_id} na ordem mas sem widget correspondente.")
                 
        # Forçar atualização do layout container
        self._grid_container.updateGeometry()
        
    @Slot()
    def _update_gui_frames(self): # Renomeada de _process_frame_queue
        """Busca os frames mais recentes do processor e atualiza os widgets."""
        try:
            # 1. Obter a cópia dos últimos frames do processor
            latest_frames = self._processor.get_latest_frames()
            
            if not latest_frames:
                # logger.debug("Nenhum frame recente para exibir.") # Pode ser verboso
                return

            # 2. Iterar sobre os frames recebidos e atualizar widgets
            for cam_id, frame_info in latest_frames.items():
                if cam_id in self._video_widgets:
                    # Passa o dicionário frame_info (que contém o frame NumPy)
                    self._video_widgets[cam_id].update_frame(frame_info) 
                # else: Log se receber frame para widget desconhecido?

        except Exception as e:
            # Usar logger aqui seria melhor
            print(f"ERRO CRÍTICO no loop _update_gui_frames: {e}")
            import traceback
            traceback.print_exc()

    @Slot()
    def _process_status_queue(self):
        """Processa atualizações de status da fila."""
        try:
            while not self._status_queue.empty():
                status_info = self._status_queue.get_nowait()
                self._update_camera_status(
                    status_info.get('camera_id', -1),
                    status_info.get('status_code', STATUS_UNKNOWN),
                    status_info.get('message', '')
                )
        except queue.Empty:
            pass
        except Exception as e:
            print(f"ERRO ao processar status queue: {e}")
            import traceback
            traceback.print_exc()
            
    # Método interno chamado por _process_status_queue
    def _update_camera_status(self, camera_id, status_code, message):
        """Atualiza o estado interno e o widget visual da câmera."""
        # Atualiza estado interno para o botão de falhas
        if camera_id >= 0:
             self._camera_status[camera_id] = {'status': status_code, 'message': message}
             
        # Atualiza o widget correspondente se ele existir
        if camera_id in self._video_widgets:
            self._video_widgets[camera_id].update_status(status_code, message)
        # else:
            # Se o widget não existe (ex: falha ao adicionar), talvez logar?
            # if status_code != STATUS_CONNECTED and status_code != STATUS_STOPPED:
            #     print(f"[GUI Status] Status {status_code} recebido para camera ID {camera_id} sem widget.")
            pass 

    @Slot()
    def _check_camera_health(self):
        """Verifica periodicamente se as câmeras conectadas estão recebendo frames."""
        now = time.time()
        timeout_threshold = 30 # Segundos sem frame para considerar erro
        
        for cam_id, widget in self._video_widgets.items():
            last_update = widget.get_last_update_time()
            status = widget.get_status()
            # Considerar falha apenas se estava CONECTADA e parou de receber frames
            if status == STATUS_CONNECTED and last_update > 0 and (now - last_update > timeout_threshold):
                print(f"[Health Check] Câmera {cam_id} conectada mas sem frames por >{timeout_threshold}s. Marcando como desconectada.")
                # Atualizar status para refletir o problema
                self._update_camera_status(cam_id, STATUS_DISCONNECTED, f"Timeout: Sem frames por >{timeout_threshold}s")
                # Poderíamos tentar reiniciar a câmera aqui emitindo um sinal para o processor? 
                # Ex: self.restart_camera_signal.emit(cam_id)
            
            # Poderia adicionar lógica para remover widgets fantasmas (sem status por muito tempo)?

    @Slot()
    def _show_failed_cameras_dialog(self):
        """Mostra um diálogo listando câmeras que não estão no estado CONECTED."""
        failed_cameras = []
        for cam_id, status_data in self._camera_status.items():
            # Considera falha se não estiver conectada (e não for um estado inicial/parada normal)
            current_status = status_data.get('status')
            if current_status != STATUS_CONNECTED and current_status != STATUS_STOPPED:
                 # Tenta obter o nome do widget se ele ainda existir
                 name = f"ID {cam_id}" # Default
                 if cam_id in self._video_widgets:
                     name = self._video_widgets[cam_id]._camera_name
                 
                 failed_cameras.append({
                     'id': cam_id,
                     'name': name,
                     'status': current_status,
                     'message': status_data.get('message', 'N/A')
                 })
                 
        if not failed_cameras:
             QMessageBox.information(self, "Status das Câmeras", "Todas as câmeras adicionadas estão conectadas ou paradas.")
             return

        dialog = QDialog(self)
        dialog.setWindowTitle("Câmeras Não Conectadas")
        dialog.setMinimumWidth(450)
        layout = QVBoxLayout(dialog)
        
        label = QLabel("As seguintes câmeras não estão no estado 'Conectada':")
        layout.addWidget(label)
        
        list_widget = QListWidget()
        list_widget.setStyleSheet("QListWidget { background-color: #444; color: white; }" 
                                "QListWidget::item { padding: 5px; }" 
                                "QListWidget::item:alternate { background-color: #555; }")
                                
        status_map = { # Mapeamento para nomes mais amigáveis
             STATUS_DISCONNECTED: "Desconectada",
             STATUS_CONNECTING: "Conectando",
             STATUS_WAITING_RECONNECT: "Aguardando Reconexão",
             STATUS_RECONNECTING: "Reconectando",
             STATUS_UNKNOWN: "Desconhecido"
             # Não incluir STOPPED aqui intencionalmente
        }

        for cam in failed_cameras:
            status_name = status_map.get(cam['status'], f"Código {cam['status']}")
            item_text = f"{cam['name']} (ID: {cam['id']})\n  Status: {status_name}\n  Mensagem: {cam['message']}"
            list_item = QListWidgetItem(item_text)
            
            # Cor de fundo baseada no status
            if cam['status'] == STATUS_DISCONNECTED:
                 list_item.setBackground(QColor("#550000")) # Vermelho escuro
            elif cam['status'] in [STATUS_WAITING_RECONNECT, STATUS_RECONNECTING]:
                 list_item.setBackground(QColor("#553300")) # Laranja escuro
            elif cam['status'] == STATUS_CONNECTING:
                 list_item.setBackground(QColor("#555500")) # Amarelo escuro
            else:
                 list_item.setBackground(QColor("#550055")) # Magenta escuro
                 
            list_item.setForeground(QColor("white"))
            list_widget.addItem(list_item)
            
        layout.addWidget(list_widget)
        
        # Botão Fechar
        button_box = QHBoxLayout()
        close_button = QPushButton("Fechar")
        close_button.clicked.connect(dialog.accept)
        button_box.addStretch()
        button_box.addWidget(close_button)
        button_box.addStretch()
        layout.addLayout(button_box)
        
        dialog.exec()

    # Sobrescreve o evento de fechamento da janela
    def closeEvent(self, event):
        """Chamado quando o usuário tenta fechar a janela."""
        print("MainWindow: Close event recebido. Solicitando parada...")
        
        # Parar todos os timers da GUI
        self._update_timer.stop() # Parar o novo timer
        self._status_timer.stop()
        self._health_timer.stop()
        print("Timers da GUI parados.")
        
        # Apenas remover os widgets visuais
        print(f"Removendo {len(self._video_widgets)} widgets da GUI...")
        for cam_id in list(self._video_widgets.keys()):
            self._remove_camera_widget(cam_id)

        # Emitir sinal para o main.py, que chamará processor.shutdown()
        print("Emitindo sinal de parada para o processador...")
        self.stop_signal.emit()
        
        print("Aceitando evento de fechamento da janela.")
        event.accept() # Permite que a janela feche 