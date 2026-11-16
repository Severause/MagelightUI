# MgPageRootTest.dll, the in-game AddPageRoot check (README.md). Built only with -DMG_TEST_PLUGINS=ON; never staged or
# packaged.
add_library(MgPageRootTest SHARED plugin.cpp)
target_include_directories(MgPageRootTest PRIVATE ${CMAKE_SOURCE_DIR}/api)
target_link_libraries(MgPageRootTest PRIVATE CommonLibSSE::CommonLibSSE)
target_compile_definitions(MgPageRootTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(MgPageRootTest PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
set_target_properties(MgPageRootTest PROPERTIES
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
