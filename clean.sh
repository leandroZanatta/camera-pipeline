#!/bin/bash
# Script para limpar artefatos de build e caches do projeto Python

# Parar em caso de erro
set -e

# Diretório raiz do projeto (onde o script está localizado)
PROJECT_DIR=$(dirname "${BASH_SOURCE[0]}")
cd "$PROJECT_DIR"

# Informar o que está sendo feito
echo "Limpando artefatos de build e caches..."

# Remover diretórios de build/distribuição
echo "Removendo build/, dist/, *.egg-info/ ..."
rm -rf build/
rm -rf dist/
rm -rf .eggs/
find . -maxdepth 1 -name '*.egg-info' -exec rm -rf {} +

# Remover caches do Python (__pycache__)
echo "Removendo caches __pycache__ ..."
find . -type d -name '__pycache__' -exec rm -rf {} +

# Remover caches de testes
echo "Removendo .pytest_cache/, .coverage ..."
rm -rf .pytest_cache/
rm -f .coverage
rm -f .coverage.*

# Remover arquivos C/C++ compilados (.so) no diretório raiz (se houver)
echo "Removendo *.so da raiz (se existirem) ..."
find . -maxdepth 1 -name '*.so' -delete
find . -maxdepth 1 -name '*.pyd' -delete # Para Windows

# Informar sobre ambientes virtuais (não remover automaticamente)
echo "Atenção: Ambientes virtuais (.venv, venv, etc.) não foram removidos."

echo "Limpeza concluída." 