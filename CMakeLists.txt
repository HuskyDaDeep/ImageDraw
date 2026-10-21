cmake_minimum_required(VERSION 3.20)
project(Img2Vec CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw  GIT_REPOSITORY https://github.com/glfw/glfw.git  GIT_TAG 3.4)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5)
FetchContent_Declare(stb   GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master)
FetchContent_Declare(nfd   GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git GIT_TAG v1.2.1)
FetchContent_MakeAvailable(glfw imgui stb nfd)

find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

find_package(Threads REQUIRED)
add_executable(img2vec main.cpp vectorizer.cpp exporter.cpp raster.cpp)
target_include_directories(img2vec PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(img2vec PRIVATE imgui_lib nfd Threads::Threads)
