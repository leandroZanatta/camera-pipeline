cmake_minimum_required(VERSION 3.10)
project(camera_pipeline_c C)

# Configuração: usar FFmpeg do sistema
option(USE_SYSTEM_FFMPEG "Usar FFmpeg do sistema" ON)

# Adicionar flags de otimização para builds de Release
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3" CACHE STRING "C flags for Release builds" FORCE)
# Para debug, geralmente queremos símbolos de depuração e talvez otimizações leves
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g" CACHE STRING "C flags for Debug builds" FORCE)

# Encontrar pthreads (necessário para Linux/macOS)
find_package(Threads REQUIRED)

# --- Adicionar FFmpeg --- 
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)

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
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

# No Linux precisamos garantir que a biblioteca tenha o nome sem o prefixo "lib"
# para corresponder ao que o Python está procurando
if(UNIX AND NOT APPLE)
    set_target_properties(camera_pipeline_c PROPERTIES
        PREFIX "" # Remove o prefixo "lib"
    )
endif()

# Para Linux e macOS, configurar RPATH para que a biblioteca encontre as dependências vendidas
if(UNIX)
    if(APPLE)
        set(ORIGIN "@loader_path")
    else()
        set(ORIGIN "$ORIGIN")
    endif()
    
    # Configurar RPATH para procurar nas pastas "lib" e "." relativas ao local da biblioteca
    set_target_properties(camera_pipeline_c PROPERTIES
        INSTALL_RPATH "${ORIGIN}/lib;${ORIGIN}"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# Lincar dependências (FFmpeg e Pthreads)
target_link_libraries(camera_pipeline_c PRIVATE 
    Threads::Threads
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)

# Instalar a biblioteca na raiz do diretório de instalação do CMake
# scikit-build-core mapeará isso para camera_pipeline/core na wheel
install(TARGETS camera_pipeline_c
        LIBRARY DESTINATION . 
        ARCHIVE DESTINATION .
       )
