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

# --- Comando para copiar a biblioteca compilada para a árvore de fontes Python ---
# Obter o nome do arquivo da biblioteca (varia por plataforma)
get_target_property(LIB_OUTPUT_NAME camera_pipeline_c OUTPUT_NAME)
if(WIN32)
  set(LIB_SUFFIX ".dll")
elseif(APPLE)
  set(LIB_SUFFIX ".dylib")
else()
  set(LIB_SUFFIX ".so")
endif()
# O OUTPUT_NAME já deve ter o prefixo removido no Linux via set_target_properties.
set(BUILT_LIBRARY_NAME "${LIB_OUTPUT_NAME}${LIB_SUFFIX}")
# Onde a biblioteca é criada pelo compilador (geralmente o diretório binário atual)
set(BUILT_LIBRARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/${BUILT_LIBRARY_NAME}") 

# Definir o destino da cópia dentro da árvore de fontes Python
# (Relativo a este CMakeLists.txt em c_src/)
set(PYTHON_PACKAGE_DEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../camera_pipeline/core")

# Garantir que o diretório de destino exista
add_custom_command(
    OUTPUT ${PYTHON_PACKAGE_DEST_DIR}/${BUILT_LIBRARY_NAME}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYTHON_PACKAGE_DEST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BUILT_LIBRARY_PATH}"
            "${PYTHON_PACKAGE_DEST_DIR}/${BUILT_LIBRARY_NAME}"
    DEPENDS camera_pipeline_c # Depende do target da biblioteca
    COMMENT "Copiando ${BUILT_LIBRARY_NAME} para ${PYTHON_PACKAGE_DEST_DIR}..."
)

# Adicionar a saída do comando customizado como fonte para um target dummy
# Isso ajuda a garantir que o comando seja executado.
add_custom_target(copy_library_to_package ALL DEPENDS ${PYTHON_PACKAGE_DEST_DIR}/${BUILT_LIBRARY_NAME})
# -----------------------------------------------------------------------------

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

# Instalar a biblioteca principal sob o prefixo gerenciado por scikit-build
install(TARGETS camera_pipeline_c
        LIBRARY DESTINATION . 
        ARCHIVE DESTINATION .
)
