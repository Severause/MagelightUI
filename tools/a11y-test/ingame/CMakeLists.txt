# MgA11yTest.dll, the in-game accessibility channel check (README.md). Built only with -DMG_TEST_PLUGINS=ON; never
# staged or packaged.
add_library(MgA11yTest SHARED plugin.cpp)
target_include_directories(MgA11yTest PRIVATE ${CMAKE_SOURCE_DIR}/api)
target_link_libraries(MgA11yTest PRIVATE CommonLibSSE::CommonLibSSE)
target_compile_definitions(MgA11yTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(MgA11yTest PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
set_target_properties(MgA11yTest PROPERTIES
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
