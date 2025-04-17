import json
import urllib.request
import logging
import sys
import os

# --- Configurações Globais ---

# Nível de log para a biblioteca C (0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=TRACE)
# Ajuste conforme necessário para depuração
C_LOG_LEVEL = 2 # Reduzido para INFO para operação normal

# Número máximo de câmeras que a biblioteca C pode gerenciar simultaneamente
# Deve ser >= ao número de câmeras retornadas pelo servidor
MAX_CAMERAS = 60 # Aumentado para suportar mais câmeras

# Timeout para buscar frames da biblioteca C (em milissegundos)
GET_FRAME_TIMEOUT_MS = 1000 # Aumentado um pouco para dar mais margem em caso de rede lenta

# URL do servidor para buscar a lista de câmeras
CAMERA_API_URL = "https://vms.protenet.com.br/api/v1/camera/objetos/get?tipgat=all&codser=196"


class AppConfig:
    """Carrega e mantém as configurações da aplicação, incluindo as URLs das câmeras."""

    def __init__(self, logger_name='AppConfig'):
        """Inicializa a configuração, buscando as URLs do servidor."""
        self.logger = logging.getLogger(logger_name)
        self.prefix = f"[{logger_name}]"
        self.camera_urls = []
        self.camera_data = [] # Armazena os dados completos das câmeras

        self.load_config()

    def load_config(self):
        """Carrega as configurações buscando os dados da API."""
        self.logger.info(f"{self.prefix} Carregando configurações da API...")
        
        # Chamar o método para buscar da API
        self.camera_data = self._buscar_cameras()
        
        if not self.camera_data:
             self.logger.error(f"{self.prefix} Não foi possível buscar dados de câmera da API. Verifique a URL e a conexão.")
             self.camera_urls = []
             return
        
        # Extrair as URLs dos dados recebidos
        self.camera_urls = [cam.get('url') for cam in self.camera_data if cam and isinstance(cam.get('url'), str) and cam.get('url')]
        self.logger.info(f"{self.prefix} {len(self.camera_urls)} URLs válidas carregadas a partir de {len(self.camera_data)} objetos recebidos da API.")

    def _buscar_cameras(self):
        """Busca a lista de câmeras (como dicionários) do servidor configurado."""
        self.logger.info(f"{self.prefix} Buscando lista de câmeras de {CAMERA_API_URL}...")
        try:
            with urllib.request.urlopen(CAMERA_API_URL, timeout=10) as response: # Adicionado timeout
                if response.status == 200:
                    content = response.read()
                    cameras = json.loads(content)
                    if isinstance(cameras, list):
                        self.logger.info(f"{self.prefix} Recebido {len(cameras)} objetos de câmera do servidor.")
                        # Validação básica de cada câmera (pode ser expandida)
                        valid_cameras = [cam for cam in cameras if isinstance(cam, dict) and 'url' in cam]
                        self.logger.info(f"{self.prefix} {len(valid_cameras)} câmeras válidas encontradas.")
                        return valid_cameras
                    else:
                        self.logger.error(f"{self.prefix} Resposta do servidor não é uma lista JSON válida.")
                        return []
                else:
                     self.logger.error(f"{self.prefix} Falha na requisição ao servidor. Status: {response.status}")
                     return []
        except urllib.error.URLError as e:
             self.logger.error(f'{self.prefix} Erro de URL ao buscar câmeras: {e.reason}', exc_info=False)
             return []
        except json.JSONDecodeError as e:
             self.logger.error(f'{self.prefix} Erro ao decodificar JSON da resposta do servidor: {e}', exc_info=False)
             # Tentar logar o início do conteúdo para depuração
             try:
                 with urllib.request.urlopen(CAMERA_API_URL, timeout=5) as resp:
                     preview = resp.read(200).decode('utf-8', errors='ignore')
                     self.logger.debug(f"{self.prefix} Prévia da resposta inválida: {preview}...")
             except Exception:
                 pass # Ignorar erros ao tentar obter prévia
             return []
        except Exception as e:
            self.logger.error(f'{self.prefix} Falha inesperada na recuperação de câmeras do servidor', exc_info=True)
            return []

    def get_camera_urls(self):
        """Retorna a lista de URLs das câmeras carregadas."""
        return self.camera_urls

    def get_camera_data(self):
        """Retorna a lista completa de dados das câmeras carregadas."""
        return self.camera_data

# --- Constantes Derivadas (Exemplo de uso) ---
# Para usar fora da classe, você instanciaria AppConfig
# Exemplo (não descomentar aqui, apenas ilustrativo):
# if __name__ == '__main__':
#     logging.basicConfig(level=logging.INFO)
#     config = AppConfig()
#     print("URLs:", config.get_camera_urls())
#     # print("Dados Completos:", config.get_camera_data())

# --- Configurações Estáticas Adicionais (se necessário) ---
# ... outras constantes globais ...

# Garantir que o diretório pai (onde core, gui estão) esteja no sys.path
# Isso é geralmente gerenciado pelo executor (main.py), mas pode adicionar aqui por segurança
# project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# if project_root not in sys.path:
#     sys.path.insert(0, project_root) 