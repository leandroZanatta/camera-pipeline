import os
import sys
from skbuild import setup
from setuptools import find_packages

# Forçar a compilação (não usar cache)
os.environ['SKBUILD_ARGS'] = '--fresh'

with open("README.md", "r", encoding="utf-8") as fh:
    long_description = fh.read()

# Renomear diretório camera-pipeline para camera_pipeline (formato de pacote Python válido)
if os.path.exists('camera-pipeline'):
    # Verificar se já existe o diretório camera_pipeline
    if not os.path.exists('camera_pipeline'):
        # Criar link simbólico ou copy diretório - apenas para desenvolvimento local
        try:
            # Em sistemas Unix, tenta criar um link simbólico
            os.symlink('camera-pipeline', 'camera_pipeline')
            print("Link simbólico camera_pipeline criado.")
        except:
            # Em caso de falha ou Windows, copia os arquivos
            import shutil
            shutil.copytree('camera-pipeline', 'camera_pipeline')
            print("Diretório camera_pipeline copiado.")

# Garantir que o diretório camera_processor existe
camera_processor_dir = os.path.join('camera_pipeline', 'core', 'camera_processor')
os.makedirs(camera_processor_dir, exist_ok=True)

setup(
    name="camera-pipeline-processor",  # Nome do seu pacote para instalação via pip
    version="0.1.0",                 # Versão inicial
    description="Processador de Pipeline de Câmera com C/C++ e Python",
    long_description=long_description,
    long_description_content_type="text/markdown",
    author="Leandro",              # Seu nome
    author_email="leandrozanatta27@gmail.com",  # Seu email
    url="https://github.com/leandroZanatta/camera-pipeline",  # URL do repositório
    license="MIT",                   # Ou outra licença que preferir
    packages=find_packages(),        # Encontra automaticamente os pacotes Python
    package_data={
        "camera_pipeline.core": ["*.so", "*.dll", "camera_processor/*.so", "camera_processor/*.dll"],
    },
    cmake_install_dir="camera_pipeline/core/camera_processor", # Onde o scikit-build deve instalar os artefatos CMake
    cmake_source_dir="c_src",        # Diretório contendo o CMakeLists.txt principal
    cmake_args=[
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_VERBOSE_MAKEFILE=ON',
    ],
    include_package_data=True,       # Importante para incluir arquivos não-python (como o .so)
    install_requires=[
        "opencv-python==4.11.0.86",
    ],
    extras_require={
        "numpy": ["numpy>=1.26.4"],
        "ui": ["PySide6"],
        "dev": ["pytest", "black", "flake8"],
        "test": ["pytest"],
        "full": ["numpy>=1.26.4", "PySide6"],
    },
    python_requires=">=3.8",        # Versão mínima do Python
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: C",
    ],
) 