# lzfse

# liblzfse - build it statically

include(ExternalProject)
ExternalProject_Add(lzfse_ext
        DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/src/
        GIT_REPOSITORY "https://github.com/lzfse/lzfse.git"
        GIT_TAG "master"
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/lzfse
        CMAKE_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_C_FLAGS=${ZSTD_C_FLAGS}
        -DCMAKE_AR=${CMAKE_AR}
        -DBUILD_SHARED_LIBS=OFF
        BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/liblzfse
        BUILD_COMMAND $(MAKE)
        INSTALL_COMMAND "true")

# force lzfse make to be called on each time
ExternalProject_Add_Step(lzfse_ext forcebuild
  DEPENDEES configure
  DEPENDERS build
  COMMAND "true"
  ALWAYS 1)

add_library(lzfse STATIC IMPORTED)
set_target_properties(lzfse PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/lzfse/src"
        IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/liblzfse/liblzfse.a")
add_dependencies(lzfse lzfse_ext)

set(lzfse_sources
  CompressionPluginLzfse.cc
  LzfseCompressor.cc
)

add_library(ceph_lzfse SHARED ${lzfse_sources})
target_link_libraries(ceph_lzfse PRIVATE lzfse)
install(TARGETS ceph_lzfse DESTINATION ${compressor_plugin_dir})
