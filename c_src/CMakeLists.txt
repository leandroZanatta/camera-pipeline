cmake_minimum_required(VERSION 3.10)
project(camera_processor C)

# Adicionar flags de otimização para builds de Release
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3" CACHE STRING "C flags for Release builds" FORCE)
# Para debug, geralmente queremos símbolos de depuração e talvez otimizações leves
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g" CACHE STRING "C flags for Debug builds" FORCE)

# Encontrar pthreads (necessário para Linux/macOS)
find_package(Threads REQUIRED)

# Adicionar dependências (EXEMPLO - você precisará ajustar)
# find_package(OpenH264 REQUIRED) # Exemplo
# find_package(LibRTMP REQUIRED) # Exemplo
# find_package(CURL REQUIRED)    # Removido

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
add_library(camera_processor SHARED ${SOURCE_FILES})

# Adicionar diretório de includes
target_include_directories(camera_processor PRIVATE 
    include # Nosso diretório de includes
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

# Lincar dependências (FFmpeg e Pthreads)
target_link_libraries(camera_processor PRIVATE 
    Threads::Threads
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)

# Opcional: Definir onde instalar a biblioteca
# Descomentar e ajustar para scikit-build-core
install(TARGETS camera_processor 
        LIBRARY DESTINATION . # Instala .so no diretório raiz definido em setup.py (cmake_install_dir)
        ARCHIVE DESTINATION . # Necessário para alguns sistemas/builds
)

# Instalar o cabeçalho público (se seu código Python precisar dele ou for uma boa prática)
# Ajuste o caminho se seu header principal for outro
# install(FILES include/camera_processor.h DESTINATION include)

# Se você tiver outros headers públicos em 'include', pode instalar o diretório:
install(DIRECTORY include/ DESTINATION include # Instala a pasta 'include' dentro de cmake_install_dir
        FILES_MATCHING PATTERN "*.h"
)
