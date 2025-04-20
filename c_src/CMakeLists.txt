cmake_minimum_required(VERSION 3.10)
project(camera_pipeline_c C)

# Configuração: usar FFmpeg (CMake padrão deve encontrar no Conda)
option(USE_SYSTEM_FFMPEG "Usar FFmpeg do sistema" ON) # Pode remover esta opção agora

# Adicionar flags de otimização para builds de Release
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3" CACHE STRING "C flags for Release builds" FORCE)
# Para debug, geralmente queremos símbolos de depuração e talvez otimizações leves
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g" CACHE STRING "C flags for Debug builds" FORCE)

# Encontrar pthreads (necessário para Linux/macOS)
find_package(Threads REQUIRED)

# --- Adicionar FFmpeg (Esperado do ambiente Conda agora) --- 
# Remover pkg-config e usar find_package diretamente
# find_package(PkgConfig REQUIRED) 
# pkg_check_modules(AVFORMAT REQUIRED libavformat)
# pkg_check_modules(AVCODEC REQUIRED libavcodec)
# pkg_check_modules(AVUTIL REQUIRED libavutil)
# pkg_check_modules(SWSCALE REQUIRED libswscale)

# Encontrar FFmpeg (deve encontrar a versão do Conda)
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale)

# Definir os arquivos fonte
set(SOURCE_FILES
    src/camera_processor.c
    src/logger.c
    src/callback_utils.c
    src/camera_thread.c
)

# Criar a biblioteca compartilhada
add_library(camera_pipeline_c SHARED ${SOURCE_FILES})

# Adicionar diretório de includes
target_include_directories(camera_pipeline_c PRIVATE 
    include # Nosso diretório de includes
    ${FFMPEG_INCLUDE_DIRS} # Usar variável definida por find_package(FFmpeg)
)

# No Linux precisamos garantir que a biblioteca tenha o nome sem o prefixo "lib"
# para corresponder ao que o Python está procurando
if(UNIX AND NOT APPLE)
    set_target_properties(camera_pipeline_c PROPERTIES
        PREFIX "" # Remove o prefixo "lib"
    )
    if(DEFINED ENV{CONDA_PREFIX})
        set(CMAKE_INSTALL_RPATH "$ENV{CONDA_PREFIX}/lib")
        message(STATUS "Setting RPATH for Conda environment to: ${CMAKE_INSTALL_RPATH}")
    else()
        message(WARNING "CONDA_PREFIX environment variable not found. RPATH might not be set correctly for Conda libs.")
        # Definir um fallback relativo pode ser perigoso, melhor avisar.
        set(CMAKE_INSTALL_RPATH "$ORIGIN") # Apenas o diretório da própria lib como fallback mínimo
    endif()
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE) # Importante: adiciona os caminhos das bibliotecas lincadas (como FFmpeg) ao RPATH
    message(STATUS "Final RPATH setting: ${CMAKE_INSTALL_RPATH}")
endif()

# Lincar dependências (FFmpeg e Pthreads)
target_link_libraries(camera_pipeline_c PRIVATE 
    Threads::Threads
    # Usar targets importados fornecidos por find_package(FFmpeg)
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
)

# Instalar a biblioteca no subdiretório correto relativo à raiz da instalação CMake
# Como wheel.install-dir="" no pyproject.toml, este caminho será preservado na wheel.
install(TARGETS camera_pipeline_c
        LIBRARY DESTINATION camera_pipeline/core 
        ARCHIVE DESTINATION camera_pipeline/core
       )
