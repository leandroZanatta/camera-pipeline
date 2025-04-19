# Este arquivo torna o diretório core/camera_processor um pacote Python
# Isso permite importar as bibliotecas C compiladas
import os
import sys

# Adiciona o diretório atual ao PATH para encontrar as bibliotecas .so
__file_dir__ = os.path.dirname(os.path.abspath(__file__))
if __file_dir__ not in sys.path:
    sys.path.append(__file_dir__) 