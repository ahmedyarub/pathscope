cmake_minimum_required(VERSION 3.21)
project(pathscope C)

# ---------------------------------------------------------------------------
# pathscope - restores path auto-completion in Windows file dialogs.
#
# Build both architectures: a global WH_CBT hook only injects into processes
# of matching bitness, so 32-bit apps need the 32-bit DLL.
#
#   cmake -B build/x64 -A x64  && cmake --build build/x64 --config Release
#   cmake -B build/x86 -A Win32 && cmake --build build/x86 --config Release
# ---------------------------------------------------------------------------

if(NOT WIN32)
    message(FATAL_ERROR "pathscope is Windows-only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "pathscope requires MSVC (uses SEH-free Win32 C and a .def export).")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PS_ARCH x64)
else()
    set(PS_ARCH x86)
endif()
message(STATUS "pathscope: building for ${PS_ARCH}")

# The hook DLL is injected into every GUI process on the desktop, so it must
# not depend on redistributable runtime DLLs. Static CRT keeps its imports to
# user32 + kernel32 only.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- the hook -----------------------------------------------------------
# pathscope.def exports CbtProc undecorated. Without it the x86 build exports
# _CbtProc@16 (stdcall decoration) and GetProcAddress fails with error 127.
add_library(pathscope SHARED pathscope.c pathscope.def)
set_target_properties(pathscope PROPERTIES OUTPUT_NAME "pathscope_${PS_ARCH}")
target_link_libraries(pathscope PRIVATE user32)

# --- console host (testing; Ctrl+C to stop) -----------------------------
add_executable(pshost pshost.c)
set_target_properties(pshost PROPERTIES OUTPUT_NAME "pshost_${PS_ARCH}")
target_link_libraries(pshost PRIVATE user32)

# --- windowless host (autostart) ----------------------------------------
add_executable(pshostw WIN32 pshostw.c)
set_target_properties(pshostw PROPERTIES OUTPUT_NAME "pshostw_${PS_ARCH}")
target_link_libraries(pshostw PRIVATE user32)

install(TARGETS pathscope pshost pshostw
        RUNTIME DESTINATION .
        LIBRARY DESTINATION .)
install(FILES README.md DESTINATION .)
