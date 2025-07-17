cmake_minimum_required(VERSION 3.10)
project(camera_pipeline_c C)

# Adicionar nosso diretório cmake ao path de módulos do CMake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Configuração: usar FFmpeg (CMake padrão deve encontrar no Conda)
option(USE_SYSTEM_FFMPEG "Usar FFmpeg do sistema" ON) # Pode remover esta opção agora

# Adicionar flags de otimização para builds de Release
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3" CACHE STRING "C flags for Release builds" FORCE)
# Para debug, geralmente queremos símbolos de depuração e talvez otimizações leves
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g" CACHE STRING "C flags for Debug builds" FORCE)

# Encontrar pthreads (necessário para Linux/macOS)
find_package(Threads REQUIRED)

# --- Adicionar FFmpeg --- 
# Encontrar FFmpeg usando nosso FindFFmpeg.cmake via CMAKE_MODULE_PATH
find_package(FFmpeg REQUIRED) # Chamada usa nosso módulo

# Definir os arquivos fonte
set(SOURCE_FILES
    src/camera_processor.c
    src/logger.c
    src/callback_utils.c
    src/camera_thread.c
    src/frame_queue.c
)

# Criar a biblioteca compartilhada
add_library(camera_pipeline_c SHARED ${SOURCE_FILES})

# Adicionar diretório de includes
target_include_directories(camera_pipeline_c PRIVATE 
    include # Nosso diretório de includes
    ${FFMPEG_INCLUDE_DIRS} # Usar variável do nosso FindFFmpeg.cmake
)

# No Linux precisamos garantir que a biblioteca tenha o nome sem o prefixo "lib"
# para corresponder ao que o Python está procurando
if(UNIX AND NOT APPLE)
    set_target_properties(camera_pipeline_c PROPERTIES
        PREFIX "" # Remove o prefixo "lib"
    )
    # Configurar RPATH para encontrar libs do Conda
    if(DEFINED ENV{CONDA_PREFIX})
        set(CMAKE_INSTALL_RPATH "$ENV{CONDA_PREFIX}/lib")
        message(STATUS "Setting RPATH for Conda environment to: ${CMAKE_INSTALL_RPATH}")
    else()
        message(WARNING "CONDA_PREFIX environment variable not found. RPATH might not be set correctly for Conda libs.")
        set(CMAKE_INSTALL_RPATH "$ORIGIN") # Fallback mínimo
    endif()
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE) # Adiciona paths das libs lincadas ao RPATH
    message(STATUS "Final RPATH setting: ${CMAKE_INSTALL_RPATH}")
endif()

# Lincar dependências (FFmpeg e Pthreads)
target_link_libraries(camera_pipeline_c PRIVATE 
    Threads::Threads
    ${FFMPEG_LIBRARIES} # Usar libs do nosso FindFFmpeg.cmake
)

# Instalar a biblioteca no subdiretório correto relativo à raiz da instalação CMake
# Como wheel.install-dir="" no pyproject.toml, este caminho será preservado na wheel.
install(TARGETS camera_pipeline_c
        LIBRARY DESTINATION camera_pipeline/core 
        ARCHIVE DESTINATION camera_pipeline/core
       )
