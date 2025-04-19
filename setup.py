# setup.py para scikit-build e compilação da biblioteca C

import os
import shutil
import subprocess
import glob
from skbuild import setup


# Para lidar com a compilação manual da biblioteca quando necessário
class PostInstallCommand:
    def __init__(self):
        self.name = "post_install"

    def run(self):
        # Verificar se a biblioteca foi compilada e copiada pelo scikit-build
        target_dir = os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "camera_pipeline",
            "core",
            "camera_processor",
        )
        
        target_lib = os.path.join(target_dir, "camera_pipeline_c.so")
        
        # Se a biblioteca não existe, compilar manualmente
        if not os.path.exists(target_lib):
            print("Biblioteca não encontrada. Tentando compilação manual...")
            
            # Criar diretório de build se não existir
            build_dir = os.path.join(
                os.path.dirname(os.path.abspath(__file__)),
                "c_src",
                "build"
            )
            os.makedirs(build_dir, exist_ok=True)
            
            # Mudar para o diretório de build e executar cmake
            cwd = os.getcwd()
            os.chdir(build_dir)
            
            try:
                # Executar cmake e make
                print("Executando cmake...")
                cmake_cmd = ["cmake", ".."]
                subprocess.run(cmake_cmd, check=True)
                
                print("Executando make...")
                make_cmd = ["make"]
                subprocess.run(make_cmd, check=True)
                
                # Verificar se a biblioteca foi gerada
                lib_path = os.path.join(build_dir, "camera_pipeline_c.so")
                if os.path.exists(lib_path):
                    # Garantir que o diretório de destino existe
                    os.makedirs(target_dir, exist_ok=True)
                    
                    # Copiar a biblioteca para o diretório do pacote
                    print(f"Copiando biblioteca de {lib_path} para {target_lib}")
                    shutil.copy2(lib_path, target_lib)
                    print("Biblioteca copiada com sucesso!")
                else:
                    print("ERRO: Biblioteca não foi gerada pelo make")
            except Exception as e:
                print(f"ERRO durante compilação manual: {e}")
            finally:
                # Voltar ao diretório original
                os.chdir(cwd)
        else:
            print(f"Biblioteca já existe em {target_lib}")
            
        # Verificar e copiar as bibliotecas do FFmpeg
        lib_dir = os.path.join(target_dir, "lib")
        if not os.path.exists(lib_dir) or not os.listdir(lib_dir):
            print("Copiando bibliotecas do FFmpeg...")
            os.makedirs(lib_dir, exist_ok=True)
            
            # Lista de bibliotecas que precisamos incluir
            ffmpeg_libs = [
                "libavformat.so",
                "libavcodec.so",
                "libavutil.so",
                "libswscale.so"
            ]
            
            system_lib_dir = "/usr/lib/x86_64-linux-gnu"
            
            for lib in ffmpeg_libs:
                # Encontrar todas as versões da biblioteca (incluindo links simbólicos)
                for file in os.listdir(system_lib_dir):
                    if file.startswith(lib):
                        src = os.path.join(system_lib_dir, file)
                        dst = os.path.join(lib_dir, file)
                        print(f"Copiando {src} para {dst}")
                        shutil.copy2(src, dst)


# Encontrar todas as bibliotecas .so no diretório lib
lib_files = []
lib_dir = os.path.join("camera_pipeline", "core", "camera_processor", "lib")
if os.path.exists(lib_dir):
    lib_files = glob.glob(os.path.join(lib_dir, "*.so*"))
    print(f"Encontradas {len(lib_files)} bibliotecas para incluir: {lib_files}")

# Os metadados, dependências e configuração de build são definidos em pyproject.toml
setup(
    package_data={
        "camera_pipeline.core.camera_processor": ["*.so", "lib/*.so*"],
    },
    data_files=[
        ("camera_pipeline/core/camera_processor/lib", lib_files),
    ],
    include_package_data=True,
    # Especificando explicitamente o diretório do CMakeLists.txt
    cmake_source_dir="c_src",
    cmake_install_dir="camera_pipeline/core/camera_processor",
    cmdclass={
        "post_install": PostInstallCommand,
    },
)
