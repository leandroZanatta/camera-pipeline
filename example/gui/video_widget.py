import time
import numpy as np
import cv2 # Para conversão YUV->BGR
import ctypes

from PySide6.QtCore import Qt, QSize, QPoint, Signal, Slot
from PySide6.QtGui import QImage, QPixmap, QPainter, QColor, QFont, QPainterPath, QPen
from PySide6.QtWidgets import (QWidget, QLabel, QVBoxLayout, QSizePolicy, QFrame)

# Importar definições da interface C e constantes
from core.c_interface import CallbackFrameData, C_LIBRARY, IS_INTERFACE_READY
from core.c_interface import (
    STATUS_STOPPED, STATUS_CONNECTING, STATUS_CONNECTED, STATUS_DISCONNECTED, 
    STATUS_WAITING_RECONNECT, STATUS_RECONNECTING, STATUS_UNKNOWN
)

class VideoWidget(QWidget):
    """Widget para exibir o vídeo e o status de uma única câmera."""

    status_changed_signal = Signal(int, int, str) # camera_id, status_code, message

    def __init__(self, parent=None, camera_id=-1, camera_name=""):
        super().__init__(parent)
        self._camera_id = camera_id
        self._camera_name = camera_name or f"Camera {camera_id}"
        # self._c_lib = c_lib_interface # REMOVIDO COMPLETAMENTE
        self.last_frame_time = 0 # Timestamp do último frame recebido
        self._last_update = 0
        self._status = STATUS_STOPPED
        self._status_message = ""
        self._has_error = False
        self._pixmap = QPixmap() # Pixmap vazio inicial

        self.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Ignored)
        self.setMinimumSize(160, 120) # Tamanho mínimo razoável

        # Layout principal
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        # Label para exibir o vídeo
        self._video_label = QLabel("Aguardando vídeo...")
        self._video_label.setAlignment(Qt.AlignCenter)
        self._video_label.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self._video_label.setStyleSheet("background-color: black; color: gray;")
        layout.addWidget(self._video_label, 1) # Ocupa a maior parte do espaço

        # Frame para o status (sobreposto)
        self._status_frame = QFrame(self)
        self._status_frame.setFrameShape(QFrame.StyledPanel)
        self._status_frame.setStyleSheet("background-color: rgba(0, 0, 0, 150); border-radius: 5px;")
        self._status_frame.setSizePolicy(QSizePolicy.Minimum, QSizePolicy.Fixed)
        status_layout = QVBoxLayout(self._status_frame)
        status_layout.setContentsMargins(5, 3, 5, 3)

        # Label para o status
        self._status_label = QLabel(self._camera_name)
        self._status_label.setStyleSheet("color: white; background-color: transparent;")
        self._status_label.setWordWrap(True)
        status_layout.addWidget(self._status_label)
        
        self._status_frame.adjustSize() # Ajusta o tamanho ao conteúdo inicial
        self._status_frame.hide() # Começa escondido, mostra quando há status

        # Inicializar display de status
        self._update_status_display()

    def resizeEvent(self, event):
        """Reposiciona o status frame no canto inferior esquerdo ao redimensionar."""
        super().resizeEvent(event)
        if self._status_frame.isVisible():
            margin = 5
            status_size = self._status_frame.sizeHint() 
            # Limita a largura máxima do status
            max_width = self.width() - 2 * margin
            if status_size.width() > max_width:
                 status_size.setWidth(max_width)
                 self._status_frame.setFixedWidth(max_width)
            else:
                 self._status_frame.setFixedWidth(status_size.width())
                 
            self._status_frame.move(margin, self.height() - status_size.height() - margin)
            self._status_frame.raise_() # Garante que fique sobre o vídeo
            
        # Redimensiona o pixmap atual se existir
        self.update_pixmap_display()

    @Slot(dict)
    def update_frame(self, frame_info):
        """Atualiza o widget com um novo frame recebido da fila."""
        # print(f"[VidWidget {self._camera_id}] Recebeu frame info: {list(frame_info.keys())}") # Debug
        try:
            frame_np = frame_info.get('frame')
            
            # Validar se recebemos um array NumPy válido
            if isinstance(frame_np, np.ndarray) and frame_np.ndim == 3 and frame_np.shape[2] == 3:
                self.last_frame_time = time.time() # Atualiza timestamp do último frame válido
                h, w, ch = frame_np.shape
                
                # --- Criação da QImage (Assumindo BGR do C/Processor) ---
                bytes_per_line = ch * w
                q_image = QImage(frame_np.data, w, h, bytes_per_line, QImage.Format_BGR888)
                
                # Redimensionar para caber no widget, mantendo aspect ratio
                pixmap = QPixmap.fromImage(q_image)
                scaled_pixmap = pixmap.scaled(self.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation)
                self._video_label.setPixmap(scaled_pixmap)
                
                # Se estava mostrando mensagem de erro/status, limpar
                if self._status_frame.isVisible():
                     self._status_frame.hide()
                     self._status_label.setText("")
            else:
                # Logar se o frame não for o esperado
                print(f"[VidWidget {self._camera_id}] Frame inválido recebido ou formato inesperado: {type(frame_np)}")
                # Não liberar referência C aqui, pois ela não existe mais neste ponto

        except Exception as e:
            print(f"[VidWidget {self._camera_id}] Erro ao atualizar frame: {e}")
            import traceback
            traceback.print_exc()

    def update_pixmap_display(self):
        """Atualiza o QLabel com o pixmap atual, escalado para o tamanho do label."""
        if not self._pixmap.isNull():
            self._video_label.setPixmap(self._pixmap.scaled(
                self._video_label.size(), 
                Qt.KeepAspectRatio, 
                Qt.SmoothTransformation
            ))
        else:
             # Se não há pixmap válido, mostrar mensagem padrão ou de erro
             self._update_status_display()

    @Slot(int, str)
    def update_status(self, status_code, message=""):
        """Atualiza o status do widget e sua aparência."""
        # print(f"[VidWidget {self._camera_id}] Recebido Status Update: Code={status_code}, Msg='{message}'")
        old_status = self._status
        self._status = status_code
        self._status_message = message or ""
        self._has_error = status_code in [STATUS_DISCONNECTED, STATUS_UNKNOWN] or status_code < 0
        
        # Atualizar interface apenas se o status mudou
        # Sempre atualizar se for erro para garantir que a msg apareça
        if old_status != status_code or self._has_error:
            self._update_status_display()
    
    def _update_status_display(self):
        """Atualiza a aparência visual baseada no status atual."""
        # Define cores e textos baseados no status
        status_text = self._camera_name
        border_color = "#555555" # Cinza padrão
        bg_color = "black"
        status_bg_color = "rgba(0, 0, 0, 150)"
        status_color = "white"
        font_weight = "normal"
        show_status = True
        
        if self._status == STATUS_CONNECTED:
            status_text += " (Conectada)"
            border_color = "#00aa00"  # Verde
            show_status = False # Esconde status overlay quando conectado
        elif self._status == STATUS_CONNECTING:
            status_text += " (Conectando...)"
            border_color = "#ffaa00"  # Amarelo
            status_bg_color = "rgba(255, 170, 0, 180)"
            status_color = "black"
        elif self._status == STATUS_DISCONNECTED:
            status_text += " (DESCONECTADA)"
            border_color = "#cc0000"  # Vermelho
            bg_color = "#110000"
            status_bg_color = "rgba(204, 0, 0, 180)"
            font_weight = "bold"
        elif self._status == STATUS_WAITING_RECONNECT:
            status_text += " (Aguardando Reconexão...)"
            border_color = "#ff7700"  # Laranja
            status_bg_color = "rgba(255, 119, 0, 180)"
        elif self._status == STATUS_RECONNECTING:
             status_text += " (Reconectando...)"
             border_color = "#ff7700"
             status_bg_color = "rgba(255, 119, 0, 180)"
        elif self._status == STATUS_STOPPED:
             status_text += " (Parada)"
             border_color = "#888888"
             status_bg_color = "rgba(100, 100, 100, 180)"
        else:  # STATUS_UNKNOWN ou outro
            status_text += " (Status Desconhecido)"
            border_color = "#ff00ff"  # Magenta
            status_bg_color = "rgba(200, 0, 200, 180)"
        
        # Adiciona mensagem de erro detalhada se houver
        if self._status_message and self._has_error:
             status_text += f"\nErro: {self._status_message}"
        elif self._status_message and self._status == STATUS_WAITING_RECONNECT:
             status_text += f"\n{self._status_message}" # Mostrar msg de tempo
             
        # Atualiza a aparência do widget
        self.setStyleSheet(f"background-color: {bg_color}; border: 2px solid {border_color}; border-radius: 3px;")
        self._status_label.setText(status_text)
        self._status_label.setStyleSheet(f"color: {status_color}; font-weight: {font_weight}; background-color: transparent;")
        self._status_frame.setStyleSheet(f"background-color: {status_bg_color}; border-radius: 5px;")
        
        # Mostrar/Esconder overlay de status
        if show_status:
             self._status_frame.adjustSize()
             self._status_frame.show()
        else:
             self._status_frame.hide()

        # Se há erro e não temos imagem válida, mostrar placeholder de erro
        if self._has_error and self._pixmap.isNull():
            error_pixmap = QPixmap(self.size() or QSize(160,120)) # Usa tamanho atual ou mínimo
            error_pixmap.fill(QColor(bg_color)) # Usa cor de fundo do erro
            painter = QPainter(error_pixmap)
            painter.setPen(QColor(180, 180, 180)) # Cor do texto de erro
            painter.setFont(QFont("Arial", 10))
            text_rect = error_pixmap.rect().adjusted(5, 5, -5, -5)
            painter.drawText(text_rect, Qt.AlignCenter | Qt.TextWordWrap, status_text)
            painter.end()
            self._video_label.setPixmap(error_pixmap)
        elif not self._has_error and self._pixmap.isNull():
             # Se não há erro mas ainda não recebeu frame, mostrar "Aguardando"
             self._video_label.setStyleSheet("background-color: black; color: gray;")
             self._video_label.setText("Aguardando vídeo...")
             self._video_label.setPixmap(QPixmap()) # Limpa pixmap anterior

    def get_last_update_time(self):
        """Retorna o timestamp da última atualização de frame."""
        return self._last_update

    def get_status(self):
         """Retorna o código de status atual."""
         return self._status
         
    def get_camera_id(self):
         return self._camera_id

    def closeEvent(self, event):
        """Garante a liberação da última referência C ao fechar."""
        print(f"[VidWidget {self._camera_id}] Close event chamado.")
        super().closeEvent(event) 