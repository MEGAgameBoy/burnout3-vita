# The Vita's packaging, for the engine's targets and the ports': an
# executable made into a SELF (eboot.bin) and a VPK the player installs with
# VitaShell.
#
#   ps2_vita_package(<target>
#       TITLE "<the name under the bubble>"
#       TITLE_ID <four capitals and five digits, unique to the application>
#       VERSION <01.00>
#       [SCE_SYS <a directory with icon0.png and livearea/contents/...>]
#       [EXTENDED_MEMORY])     # 365 MB of main memory instead of 256 (param.sfo's ATTRIBUTE2=12)
#
# Builds <target>.vpk beside the executable.

if(NOT VITA)
    return()
endif()
include("${VITASDK}/share/vita.cmake" REQUIRED)

function(ps2_vita_package target)
    cmake_parse_arguments(V "EXTENDED_MEMORY" "TITLE;TITLE_ID;VERSION;SCE_SYS" "" ${ARGN})
    if(NOT V_VERSION)
        set(V_VERSION "01.00")
    endif()
    # Relocations kept in the ELF (vita-elf-create needs them): the toolchain
    # file's -Wl,-q, for targets whose flags do not come from it.
    target_link_options(${target} PRIVATE -Wl,-q)
    set(sfo_flags "-d PARENTAL_LEVEL=1")
    if(V_EXTENDED_MEMORY)
        string(APPEND sfo_flags " -d ATTRIBUTE2=12")
    endif()
    # vita_create_vpk reads the flags from this variable: this target's own.
    set(VITA_MKSFOEX_FLAGS "${sfo_flags}")
    vita_create_self(${target}.self ${target} UNSAFE)
    set(files)
    if(V_SCE_SYS)
        file(GLOB_RECURSE sce_sys RELATIVE ${V_SCE_SYS} ${V_SCE_SYS}/*)
        foreach(f IN LISTS sce_sys)
            list(APPEND files FILE ${V_SCE_SYS}/${f} sce_sys/${f})
        endforeach()
    endif()
    vita_create_vpk(${target}.vpk ${V_TITLE_ID} ${target}.self VERSION ${V_VERSION} NAME "${V_TITLE}" ${files})
endfunction()
