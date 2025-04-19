# Dockerfile para testar a instalação do camera-pipeline-processor

# Usar uma imagem base do Python
FROM python:3.10-slim

# Instalar dependências necessárias
RUN apt-get update && apt-get install -y \
    git \
    cmake \
    build-essential \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Diretório de trabalho
WORKDIR /app

RUN pip install --upgrade pip

# Clonar o repositório
RUN git clone https://github.com/leandroZanatta/camera-pipeline.git .

# Instalar o projeto
RUN pip install .

# Comando para testar a instalação
CMD ["python", "-c", "import camera_pipeline; print('Instalação bem-sucedida!')"] 