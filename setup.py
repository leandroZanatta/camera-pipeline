from skbuild import setup
from setuptools import find_packages

with open("README.md", "r", encoding="utf-8") as fh:
    long_description = fh.read()

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
        "core": ["*.so", "*.dll"],  # Incluir arquivos binários compilados
    },
    cmake_install_dir="core/camera_processor", # Onde o scikit-build deve instalar os artefatos CMake
    cmake_source_dir="c_src",        # Diretório contendo o CMakeLists.txt principal
    include_package_data=True,       # Importante para incluir arquivos não-python (como o .so)
    install_requires=[
        "numpy==2.2.4",
        "opencv-python==4.11.0.86",
        "PySide6==6.9.0",
    ],
    extras_require={
        "dev": ["pytest", "black", "flake8"],
        "test": ["pytest"],
    },
    python_requires=">=3.8",        # Versão mínima do Python
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: C",
    ],
) 