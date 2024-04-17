cmake_minimum_required(VERSION 3.5)
project(mux CXX)

set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")

add_executable(${PROJECT_NAME} main.cpp logrus.cpp relay.cpp errors.cpp netutil.cpp)
target_link_libraries(${PROJECT_NAME})