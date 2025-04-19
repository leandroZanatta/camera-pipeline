#!/usr/bin/env python3
"""
Script para copiar a biblioteca .so compilada para o diretório correto no pacote Python.
Isso é necessário porque o scikit-build não está instalando corretamente a biblioteca.
"""

import os
import shutil
import glob
import sys

def copiar_biblioteca():
    # Diretório base do projeto
    base_dir = os.path.dirname(os.path.abspath(__file__))
    
    # Diretório do _skbuild
    skbuild_dir = os.path.join(base_dir, '_skbuild')
    
    if not os.path.exists(skbuild_dir):
        print(f"Diretório _skbuild não encontrado em {skbuild_dir}")
        return False
    
    # Procurar arquivos .so em _skbuild
    pattern = os.path.join(skbuild_dir, '**', '*.so')
    so_files = glob.glob(pattern, recursive=True)
    
    if not so_files:
        print(f"Nenhum arquivo .so encontrado em {skbuild_dir}")
        return False
    
    # Diretório de destino no pacote Python
    dest_dir = os.path.join(base_dir, 'camera_pipeline', 'core', 'camera_processor')
    
    # Criar diretório de destino se não existir
    os.makedirs(dest_dir, exist_ok=True)
    
    # Copiar cada arquivo .so encontrado
    for src_file in so_files:
        # Obter apenas o nome do arquivo, sem o caminho
        filename = os.path.basename(src_file)
        dst_file = os.path.join(dest_dir, filename)
        
        print(f"Copiando {src_file} para {dst_file}")
        shutil.copy2(src_file, dst_file)
    
    print(f"Biblioteca(s) copiada(s) com sucesso para {dest_dir}")
    return True

if __name__ == "__main__":
    if copiar_biblioteca():
        print("Operação concluída com sucesso!")
        sys.exit(0)
    else:
        print("Falha ao copiar biblioteca.")
        sys.exit(1) 