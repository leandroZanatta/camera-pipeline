# Dockerfile para testar a instalação do camera-pipeline-processor

# Usar uma imagem base do Python
FROM python:3.10-slim

# Instalar dependências necessárias
RUN apt-get update && apt-get install -y \
    git \
    cmake \
    build-essential \
    pkg-config \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --upgrade pip

# Diretório de trabalho
WORKDIR /app

# Instalar o projeto diretamente do GitHub
RUN pip install git+https://github.com/leandroZanatta/camera-pipeline.git

# Comando para testar a instalação
CMD ["python", "-c", "import camera_pipeline; print('Instalação bem-sucedida!')"] 