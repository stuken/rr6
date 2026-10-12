# XenosRecomp runs on the build host and turns RR6's Xenos shaders into the
# embedded DXIL/SPIR-V cache. RR6_RECOMP selects the XDK 2.0.2135 container
# handling (revision 0x0E) and keeps Blue Dragon's shader rewrites out.
# REBLUE_RECOMP selects the shader ABI shared with the reblue-derived renderer.
add_subdirectory(XenosRecomp EXCLUDE_FROM_ALL)
target_compile_definitions(XenosRecomp PRIVATE REBLUE_RECOMP RR6_RECOMP)
