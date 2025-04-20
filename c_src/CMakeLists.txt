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
# Remover pkg-config explícito daqui
# find_package(PkgConfig REQUIRED) 
# ... (linhas pkg_check_modules removidas)

# Encontrar FFmpeg usando nosso FindFFmpeg.cmake via CMAKE_MODULE_PATH
# A chamada agora usa o módulo que definimos
find_package(FFmpeg REQUIRED)

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
    ${FFMPEG_INCLUDE_DIRS} # Usar variável definida por nosso FindFFmpeg.cmake
)

# No Linux precisamos garantir que a biblioteca tenha o nome sem o prefixo "lib"
# para corresponder ao que o Python está procurando
if(UNIX AND NOT APPLE)
    set_target_properties(camera_pipeline_c PROPERTIES
        PREFIX "" # Remove o prefixo "lib"
    )
endif()

# Para Linux e macOS, configurar RPATH para que a biblioteca encontre as dependências
# **Importante:** Agora deve apontar para as libs do Conda se necessário,
# mas find_package geralmente configura isso melhor. Remover RPATH explícito por enquanto.
# if(UNIX)
#     if(APPLE)
#         set(ORIGIN "@loader_path")
#     else()
#         set(ORIGIN "$ORIGIN")
#     endif()
#     
#     set_target_properties(camera_pipeline_c PROPERTIES
#         INSTALL_RPATH "${ORIGIN}/lib;${ORIGIN}"
#         BUILD_WITH_INSTALL_RPATH TRUE
#     )
# endif()

# Lincar dependências (FFmpeg e Pthreads)
target_link_libraries(camera_pipeline_c PRIVATE 
    Threads::Threads
    # Usar as bibliotecas encontradas pelo nosso FindFFmpeg.cmake
    ${FFMPEG_LIBRARIES}
)

# Instalar a biblioteca no subdiretório correto relativo à raiz da instalação CMake
# Como wheel.install-dir="" no pyproject.toml, este caminho será preservado na wheel.
install(TARGETS camera_pipeline_c
        LIBRARY DESTINATION camera_pipeline/core 
        ARCHIVE DESTINATION camera_pipeline/core
       )
