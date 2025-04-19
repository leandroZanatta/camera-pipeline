#!/usr/bin/env python
"""
Verifica se as bibliotecas da camera-pipeline estão instaladas corretamente.
Esse script tenta carregar explicitamente cada biblioteca para identificar problemas.
"""

import os
import sys
import platform
import ctypes
import logging

# Configurar logging
logging.basicConfig(level=logging.INFO, 
                   format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger("verify_libs")

def check_environment():
    """Verifica variáveis de ambiente relevantes"""
    logger.info(f"Python version: {sys.version}")
    logger.info(f"Platform: {platform.platform()}")
    
    if platform.system() == "Linux":
        ld_path = os.environ.get('LD_LIBRARY_PATH', '')
        logger.info(f"LD_LIBRARY_PATH: {ld_path}")
    elif platform.system() == "Darwin":
        dyld_path = os.environ.get('DYLD_LIBRARY_PATH', '')
        logger.info(f"DYLD_LIBRARY_PATH: {dyld_path}")
    elif platform.system() == "Windows":
        path = os.environ.get('PATH', '')
        logger.info(f"PATH: {path}")

def get_package_path():
    """Encontra o caminho para o pacote camera_pipeline"""
    try:
        import camera_pipeline
        return os.path.dirname(camera_pipeline.__file__)
    except ImportError:
        logger.error("Pacote camera_pipeline não encontrado!")
        return None

def check_libraries():
    """Tenta carregar cada uma das bibliotecas necessárias"""
    package_path = get_package_path()
    if not package_path:
        return
    
    logger.info(f"Pacote camera_pipeline encontrado em: {package_path}")
    
    # Verificar a existência do diretório lib
    lib_dir = os.path.join(package_path, 'core', 'camera_processor', 'lib')
    if os.path.exists(lib_dir):
        logger.info(f"Diretório de bibliotecas encontrado: {lib_dir}")
        logger.info(f"Conteúdo do diretório: {os.listdir(lib_dir)}")
    else:
        logger.error(f"Diretório de bibliotecas NÃO encontrado: {lib_dir}")
    
    # Verificar se a biblioteca principal existe
    main_lib = os.path.join(package_path, 'core', 'camera_processor', 'camera_pipeline_c.so')
    if os.path.exists(main_lib):
        logger.info(f"Biblioteca principal encontrada: {main_lib}")
    else:
        logger.error(f"Biblioteca principal NÃO encontrada: {main_lib}")
    
    # Tentar carregar cada biblioteca individualmete
    if os.path.exists(lib_dir):
        for lib_file in sorted(os.listdir(lib_dir)):
            if lib_file.endswith('.so') or lib_file.endswith('.dll') or lib_file.endswith('.dylib'):
                lib_path = os.path.join(lib_dir, lib_file)
                try:
                    ctypes.CDLL(lib_path)
                    logger.info(f"✓ Biblioteca carregada com sucesso: {lib_file}")
                except Exception as e:
                    logger.error(f"✗ Falha ao carregar biblioteca {lib_file}: {e}")
    
    # Tentar carregar a biblioteca principal
    if os.path.exists(main_lib):
        try:
            lib = ctypes.CDLL(main_lib)
            logger.info(f"✓ Biblioteca principal carregada com sucesso!")
            # Verificar algumas funções
            if hasattr(lib, 'processor_initialize'):
                logger.info(f"✓ Função processor_initialize encontrada!")
            else:
                logger.error(f"✗ Função processor_initialize NÃO encontrada!")
        except Exception as e:
            logger.error(f"✗ Falha ao carregar biblioteca principal: {e}")
            
def test_import():
    """Tenta importar o módulo e verificar se funciona"""
    try:
        from camera_pipeline.core import CameraProcessor
        processor = CameraProcessor()
        logger.info("✓ Módulo importado e inicializado com sucesso!")
        processor.shutdown()
        logger.info("✓ Desligamento do processador bem-sucedido!")
        return True
    except Exception as e:
        logger.error(f"✗ Falha ao importar/inicializar módulo: {e}")
        return False

if __name__ == "__main__":
    logger.info("=== Verificação de bibliotecas da camera-pipeline ===")
    check_environment()
    check_libraries()
    test_import()
    logger.info("=== Verificação concluída ===") 