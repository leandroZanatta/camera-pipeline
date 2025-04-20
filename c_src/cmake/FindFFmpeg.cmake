# Use pkg-config to find FFmpeg components if available
find_package(PkgConfig QUIET)

# --- Adicionar tentativa explícita com PKG_CONFIG_PATH do Conda ---
set(_PKG_CONFIG_CMD "${PKG_CONFIG_EXECUTABLE}")
set(_PKG_CONFIG_EXTRA_ARGS "")
if(DEFINED ENV{CONDA_PREFIX} AND IS_DIRECTORY "$ENV{CONDA_PREFIX}/lib/pkgconfig")
    message(STATUS "Tentando pkg-config com path Conda: $ENV{CONDA_PREFIX}/lib/pkgconfig")
    set(_PKG_CONFIG_EXTRA_ARGS "--define-variable=prefix=$ENV{CONDA_PREFIX} --variable=pc_path=$ENV{CONDA_PREFIX}/lib/pkgconfig")
    # Ou tentar definir a variável de ambiente diretamente para o comando
    # set(_PKG_CONFIG_CMD "env PKG_CONFIG_PATH=$ENV{CONDA_PREFIX}/lib/pkgconfig ${PKG_CONFIG_EXECUTABLE}")
endif()
# ----------------------------------------------------------

# List of all possible FFmpeg components (libraries)
# ... (resto como estava) ...

foreach(_component ${FFmpeg_FIND_COMPONENTS})
    string(TOUPPER ${_component} _COMPONENT_UPPER)
    set(_FFMPEG_COMPONENT_FOUND FALSE)

    # Try using pkg-config first (com tentativa explícita de path Conda)
    if(PKG_CONFIG_FOUND)
        # Usar execute_process para mais controle e usar _PKG_CONFIG_EXTRA_ARGS
        execute_process(
            COMMAND ${_PKG_CONFIG_EXECUTABLE} --exists --print-errors "lib${_component}" ${_PKG_CONFIG_EXTRA_ARGS}
            RESULT_VARIABLE _pc_result
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(_pc_result EQUAL 0) # pkg-config encontrou o módulo
            # Obter variáveis necessárias
            execute_process(COMMAND ${_PKG_CONFIG_EXECUTABLE} --cflags-only-I "lib${_component}" ${_PKG_CONFIG_EXTRA_ARGS} OUTPUT_VARIABLE PC_${_COMPONENT_UPPER}_INCLUDE_DIRS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            execute_process(COMMAND ${_PKG_CONFIG_EXECUTABLE} --libs-only-L "lib${_component}" ${_PKG_CONFIG_EXTRA_ARGS} OUTPUT_VARIABLE PC_${_COMPONENT_UPPER}_LIBRARY_DIRS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            execute_process(COMMAND ${_PKG_CONFIG_EXECUTABLE} --libs-only-l "lib${_component}" ${_PKG_CONFIG_EXTRA_ARGS} OUTPUT_VARIABLE PC_${_COMPONENT_UPPER}_LIBRARIES OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            execute_process(COMMAND ${_PKG_CONFIG_EXECUTABLE} --cflags-only-other "lib${_component}" ${_PKG_CONFIG_EXTRA_ARGS} OUTPUT_VARIABLE PC_${_COMPONENT_UPPER}_CFLAGS_OTHER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            # Limpar flags -I e -L das variáveis
            string(REPLACE "-I" "" PC_${_COMPONENT_UPPER}_INCLUDE_DIRS "${PC_${_COMPONENT_UPPER}_INCLUDE_DIRS}")
            string(REPLACE "-L" "" PC_${_COMPONENT_UPPER}_LIBRARY_DIRS "${PC_${_COMPONENT_UPPER}_LIBRARY_DIRS}")
            string(REPLACE "-l" "" PC_${_COMPONENT_UPPER}_LIBRARIES "${PC_${_COMPONENT_UPPER}_LIBRARIES}")
            # Separar diretórios/libs por ponto e vírgula
            string(REGEX REPLACE " +" ";" PC_${_COMPONENT_UPPER}_INCLUDE_DIRS "${PC_${_COMPONENT_UPPER}_INCLUDE_DIRS}")
            string(REGEX REPLACE " +" ";" PC_${_COMPONENT_UPPER}_LIBRARY_DIRS "${PC_${_COMPONENT_UPPER}_LIBRARY_DIRS}")
            string(REGEX REPLACE " +" ";" PC_${_COMPONENT_UPPER}_LIBRARIES "${PC_${_COMPONENT_UPPER}_LIBRARIES}")

            set(PC_${_COMPONENT_UPPER}_FOUND TRUE)
            message(STATUS "pkg-config encontrou ${component} (com path Conda explícito)")
        else()
             message(WARNING "pkg-config NÃO encontrou ${component} (com path Conda explícito)")
             set(PC_${_COMPONENT_UPPER}_FOUND FALSE)
        endif()
    endif()

    # O if(PC_${_COMPONENT_UPPER}_FOUND) abaixo continua como estava...
    if(PC_${_COMPONENT_UPPER}_FOUND)
        # --- Bloco if PC_COMPONENT_FOUND --- 
        set(FFMPEG_${_COMPONENT_UPPER}_INCLUDE_DIRS ${PC_${_COMPONENT_UPPER}_INCLUDE_DIRS})
        set(FFMPEG_${_COMPONENT_UPPER}_LIBRARY_DIRS ${PC_${_COMPONENT_UPPER}_LIBRARY_DIRS})
        set(FFMPEG_${_COMPONENT_UPPER}_LIBRARIES ${PC_${_COMPONENT_UPPER}_LIBRARIES})
        set(FFMPEG_${_COMPONENT_UPPER}_DEFINITIONS ${PC_${_COMPONENT_UPPER}_CFLAGS_OTHER}) # Pode precisar de parsing
        set(_FFMPEG_COMPONENT_FOUND TRUE)
        list(APPEND FFMPEG_INCLUDE_DIRS ${FFMPEG_${_COMPONENT_UPPER}_INCLUDE_DIRS})
        list(APPEND FFMPEG_LIBRARY_DIRS ${FFMPEG_${_COMPONENT_UPPER}_LIBRARY_DIRS})
        list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${_COMPONENT_UPPER}_LIBRARIES})
        list(APPEND FFMPEG_DEFINITIONS ${FFMPEG_${_COMPONENT_UPPER}_DEFINITIONS})
        # --- Fim do Bloco if PC_COMPONENT_FOUND --- 
    else()
        # --- Bloco else (Fallback para find_library/find_path) ---
        message(STATUS "Fallback para find_library/path para ${component}")
        find_ffmpeg_library("lib${_component}" FFMPEG_${_COMPONENT_UPPER}_LIBRARY)
        find_ffmpeg_include("${_component}.h" FFMPEG_${_COMPONENT_UPPER}_INCLUDE_DIR PATH_SUFFIXES "lib${_component}")

        if(FFMPEG_${_COMPONENT_UPPER}_LIBRARY AND FFMPEG_${_COMPONENT_UPPER}_INCLUDE_DIR)
            set(_FFMPEG_COMPONENT_FOUND TRUE)
            list(APPEND FFMPEG_INCLUDE_DIRS ${FFMPEG_${_COMPONENT_UPPER}_INCLUDE_DIR})
            set(FFMPEG_${_COMPONENT_UPPER}_DEFINITIONS "") # Nenhum conhecido aqui
        endif()
        # --- Fim do Bloco else --- 
    endif()

    if(_FFMPEG_COMPONENT_FOUND)
        # --- Bloco if _FFMPEG_COMPONENT_FOUND (após pkg-config ou fallback) ---
        list(APPEND _FFMPEG_FOUND_COMPONENTS ${_component})
        # Define IMPORTED target (a lógica original aqui estava OK)
        if(NOT TARGET FFmpeg::${_component})
            add_library(FFmpeg::${_component} INTERFACE IMPORTED)
            # Definir propriedades baseadas em PC_* ou FFMPEG_* 
            if(PC_${_COMPONENT_UPPER}_FOUND)
                if(PC_${_COMPONENT_UPPER}_INCLUDE_DIRS)
                  set_target_properties(FFmpeg::${_component} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${PC_${_COMPONENT_UPPER}_INCLUDE_DIRS}")
                endif()
                # Para linkagem, usar os nomes das libs encontradas
                if(PC_${_COMPONENT_UPPER}_LIBRARIES)
                   set_property(TARGET FFmpeg::${_component} APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${PC_${_COMPONENT_UPPER}_LIBRARIES}")
                endif()
                 # Para diretórios de bibliotecas
                 if(PC_${_COMPONENT_UPPER}_LIBRARY_DIRS)
                    set_property(TARGET FFmpeg::${_component} APPEND PROPERTY INTERFACE_LINK_DIRECTORIES "${PC_${_COMPONENT_UPPER}_LIBRARY_DIRS}")
                endif()
                # Para definições/flags (simplificado)
                if(PC_${_COMPONENT_UPPER}_CFLAGS_OTHER)
                   set_property(TARGET FFmpeg::${_component} APPEND PROPERTY INTERFACE_COMPILE_OPTIONS "${PC_${_COMPONENT_UPPER}_CFLAGS_OTHER}")
                endif()
            elseif(FFMPEG_${_COMPONENT_UPPER}_LIBRARY)
                # Se encontrado via find_library/path
                set_target_properties(FFmpeg::${_component} PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${_COMPONENT_UPPER}_INCLUDE_DIR}"
                    INTERFACE_LINK_LIBRARIES "${FFMPEG_${_COMPONENT_UPPER}_LIBRARY}"
                )
            endif()
        endif()
        set(FFmpeg_${_COMPONENT_UPPER}_FOUND TRUE)
        # --- Fim do Bloco if _FFMPEG_COMPONENT_FOUND ---
    else()
         # --- Bloco else (Componente não encontrado nem via pkg-config nem fallback) ---
        set(FFmpeg_${_COMPONENT_UPPER}_FOUND FALSE)
        if(FFmpeg_FIND_REQUIRED_${_component})
            message(FATAL_ERROR "Could not find FFmpeg component '${_component}'")
        endif()
         # --- Fim do Bloco else ---
    endif()

endforeach()

# --- Final processing --- (como estava)
# ... 