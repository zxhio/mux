cmake_minimum_required(VERSION 3.5)
project(mux CXX)

include(ExternalProject)

# build libev
set(LIBEV_ROOT        ${CMAKE_BINARY_DIR}/third-party/libev)
set(LIBEV_LIB_DIR     ${LIBEV_ROOT}/lib)
set(LIBEV_INCLUDE_DIR ${LIBEV_ROOT}/include)
 
set(LIBEV_URL       ${PROJECT_SOURCE_DIR}/third-party/libev/libev-4.33.tar.gz)
set(LIBEV_CONFIGURE cd ${LIBEV_ROOT}/src/libev && ./configure --enable-shared=no --prefix=${LIBEV_ROOT})
set(LIBEV_MAKE      cd ${LIBEV_ROOT}/src/libev && make)
set(LIBEV_INSTALL   cd ${LIBEV_ROOT}/src/libev && make install)
 
ExternalProject_Add(libev
        URL                        ${LIBEV_URL}
        DOWNLOAD_NAME              libev-4.33.tar.gz
        PREFIX                     ${LIBEV_ROOT}
        CONFIGURE_COMMAND          ${LIBEV_CONFIGURE}
        BUILD_COMMAND              ${LIBEV_MAKE}
        INSTALL_COMMAND            ${LIBEV_INSTALL}
        DOWNLOAD_EXTRACT_TIMESTAMP ""
)

include_directories(${LIBEV_INCLUDE_DIR})
link_directories(${LIBEV_LIB_DIR})

add_executable(${PROJECT_NAME} main.cpp logrus.cpp)
add_dependencies(${PROJECT_NAME} libev)
target_link_libraries(${PROJECT_NAME} ev fmt)