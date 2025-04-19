# setup.py para scikit-build e compilação da biblioteca C

import os
import shutil
import subprocess
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


# Os metadados, dependências e configuração de build são definidos em pyproject.toml
setup(
    package_data={
        "camera_pipeline.core.camera_processor": ["*.so"],
    },
    # Especificando explicitamente o diretório do CMakeLists.txt
    cmake_source_dir="c_src",
    cmake_install_dir="camera_pipeline/core/camera_processor",
    cmdclass={
        "post_install": PostInstallCommand,
    },
)
