# third_party/usrsctp/CMakeLists.txt
# =============================================================================
# usrsctp — portable userland SCTP stack (Transport PAL Slice 5 / v0.11.0).
#
# Populated by:
#   curl -L https://codeload.github.com/sctplab/usrsctp/tar.gz/refs/tags/0.9.5.0 \
#       | tar -xz -C src/third_party/usrsctp/src --strip-components=1
#
# Upstream version: usrsctp 0.9.5.0 (vendored commit: see src/LICENSE.md).
#
# Target produced by upstream: `usrsctp` (STATIC, the library has ~30 .c
# files in usrsctplib/netinet/, usrsctplib/netinet6/, and usrsctplib/).
# usrsctp is a pure userland SCTP stack — it does NOT use the kernel's
# SCTP protocol module. It carries its own FreeBSD-derived netinet/netinet6
# headers (`usrsctplib/netinet/sctp*.h`) so the NimRTC SCTP seam
# (`UsrsctpSocket`) only needs `<usrsctp.h>` from the public include root.
#
# IMPORTANT — we override sctp_werror=0 to keep MSVC quiet on harmless
# signed/unsigned conversion warnings that flood the build. The static
# library is built into nimrtc_sctp and never re-exported, so we don't
# need warnings-as-errors to surface ABI mismatches.
#
# IMPORTANT — sctp_build_programs=OFF and sctp_build_fuzzer=OFF: we only
# need the library, not the rtcweb/discard_server/... examples (which
# are CLI programs with UTF-8 literals that MSVC reads as CP1252 anyway).
# =============================================================================

set(_upstream_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Upstream options — disable everything we don't need.
set(sctp_werror         OFF CACHE BOOL "" FORCE)
set(sctp_build_programs OFF CACHE BOOL "" FORCE)
set(sctp_build_fuzzer   OFF CACHE BOOL "" FORCE)
set(sctp_debug               1 CACHE BOOL "" FORCE)
set(sctp_invariants          0 CACHE BOOL "" FORCE)
set(sctp_inet                1 CACHE BOOL "" FORCE)
set(sctp_inet6               1 CACHE BOOL "" FORCE)
set(sctp_build_shared_lib    0 CACHE BOOL "" FORCE)

# Force a static build even if the upstream CMakeLists defaulted to shared.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# usrsctp 0.9.5.0's CMakeLists.txt requires CMake >= 3.0 and does its
# own `project(usrsctplib C)`, so we cannot add_subdirectory(usrsctplib)
# directly without it complaining that we set BUILD_SHARED_LIBS after
# the project() call. Instead we instantiate a sub-build into the
# vendor binary directory and consume the imported `usrsctp` target
# from there. This avoids the "project name mismatch" warning CMake
# 3.27+ emits when a subdirectory declares its own project().
#
# Approach: use ExternalProject_Add with a no-op update step (we have
# the source already; no fetch required) and consume its outputs.
#
# Simpler & more robust approach: directly compile the .c files in
# nimrtc_sctp via add_library(nimrtc_vendor_usrsctp ...). This avoids
# add_subdirectory entirely and keeps the Windows link list (ws2_32,
# iphlpapi, crypt32, winmm) in our own CMakeLists.txt where we can
# tweak them without wrestling upstream.
#
# We compile the union of upstream's `usrsctp_sources` list (see
# upstream usrsctplib/CMakeLists.txt lines 165-185).

set(_usrsctp_root "${_upstream_dir}/usrsctplib")

list(APPEND _usrsctp_srcs
    "${_usrsctp_root}/netinet/sctp_asconf.c"
    "${_usrsctp_root}/netinet/sctp_auth.c"
    "${_usrsctp_root}/netinet/sctp_bsd_addr.c"
    "${_usrsctp_root}/netinet/sctp_callout.c"
    "${_usrsctp_root}/netinet/sctp_cc_functions.c"
    "${_usrsctp_root}/netinet/sctp_crc32.c"
    "${_usrsctp_root}/netinet/sctp_indata.c"
    "${_usrsctp_root}/netinet/sctp_input.c"
    "${_usrsctp_root}/netinet/sctp_output.c"
    "${_usrsctp_root}/netinet/sctp_pcb.c"
    "${_usrsctp_root}/netinet/sctp_peeloff.c"
    "${_usrsctp_root}/netinet/sctp_sha1.c"
    "${_usrsctp_root}/netinet/sctp_ss_functions.c"
    "${_usrsctp_root}/netinet/sctp_sysctl.c"
    "${_usrsctp_root}/netinet/sctp_timer.c"
    "${_usrsctp_root}/netinet/sctp_userspace.c"
    "${_usrsctp_root}/netinet/sctp_usrreq.c"
    "${_usrsctp_root}/netinet/sctputil.c"
    "${_usrsctp_root}/netinet6/sctp6_usrreq.c"
    "${_usrsctp_root}/user_environment.c"
    "${_usrsctp_root}/user_mbuf.c"
    "${_usrsctp_root}/user_recv_thread.c"
    "${_usrsctp_root}/user_socket.c"
    # NimRTC seam helper — exports C-linkage symbols
    # (nimrtc_set_userspace_udp_socket / nimrtc_get_userspace_udp_socket)
    # that read/write the upstream `system_base_info.userspace_udpsctp`
    # slot. Compiled in here because it must NOT be visible from
    # the NimRTC C++ seam — the seam only sees <usrsctp.h>, not the
    # netinet/* internal headers (they re-define usrsctp.h's tags and
    # trigger MSVC C2011). See nimrtc_sctp_helpers.c header for the
    # full design.
    "${_usrsctp_root}/nimrtc_sctp_helpers.c"
)

# Some upstream files might be missing on an incomplete vendor extract
# (the task brief allows for partial vendors).  Verify each one exists
# and emit a friendly FATAL_ERROR otherwise so the failure mode is
# obvious (rather than CMake's silent "no rule to build obj.o").
foreach(_src ${_usrsctp_srcs})
    if(NOT EXISTS "${_src}")
        message(FATAL_ERROR
            "usrsctp vendor source missing: ${_src}\n"
            "Run scripts/vendor-fetch.sh usrsctp (or pull the tarball "
            "from https://codeload.github.com/sctplab/usrsctp/tar.gz/refs/tags/0.9.5.0) "
            "and unpack into src/third_party/usrsctp/src/. The TPAL-5 "
            "SCTP backend cannot build without this file.")
    endif()
endforeach()

add_library(nimrtc_vendor_usrsctp STATIC ${_usrsctp_srcs})
add_library(nimrtc::vendor::usrsctp ALIAS nimrtc_vendor_usrsctp)

# usrsctp's public header (`usrsctp.h`) lives at usrsctplib/usrsctp.h.
# Internal headers (netinet/*.h, netinet6/*.h, user_*.h) live in the
# same directory and must be on the include path for compilation.
target_include_directories(nimrtc_vendor_usrsctp
    PUBLIC
        $<BUILD_INTERFACE:${_usrsctp_root}>
        $<BUILD_INTERFACE:${_usrsctp_root}/..>   # for "usrsctp.h" via usrsctp/ subpath (not used)
    PRIVATE
        ${_usrsctp_root}                          # user_*.h, netinet/*.h
)

# usrsctp's CMakeLists.txt adds these defines unconditionally; we do
# the same so the .c files compile without #ifdef ladders.
target_compile_definitions(nimrtc_vendor_usrsctp
    PUBLIC
        __Userspace__=1                           # switch userland code path
        SCTP_SIMPLE_ALLOCATOR=1                   # usrsctp's malloc wrapper
        SCTP_PROCESS_LEVEL_LOCKS=1                # use pthread/process locks
        WIN32=1                                   # upstream branches on _WIN32
        _WINDOWS=1
        _CRT_SECURE_NO_WARNINGS=1                 # upstream uses strerror/sprintf
        WINDOWS=1                                 # upstream "windows" branch in
                                                 # userspace.h / user_environment.h
        HAVE_STDATOMIC_H=1                        # C11 atomics (upstream probes this)
        INET=1
        INET6=1
        __APPLE_USE_RFC_3542=1
        SCTP_DEBUG=1                              # sctp_debug option default
)

# Linux: enable _GNU_SOURCE so <netinet/in.h> exposes the IPv6 advanced
# API (struct in6_pktinfo, struct in6_addr, IPV6_RECVPKTINFO, etc.).
# Without this, user_recv_thread.c fails to compile on glibc-based
# systems with "error: invalid application of 'sizeof' to incomplete
# type 'struct in6_pktinfo'".  macOS uses the __APPLE_USE_RFC_3542
# branch above and Windows uses a separate codepath; the flag is
# therefore a no-op outside Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(nimrtc_vendor_usrsctp PRIVATE _GNU_SOURCE=1)
endif()

# Windows: link Winsock + ip helper API. upstream usrsctplib/CMakeLists.txt
# only adds ws2_32 + iphlpapi; we add the same set. If link errors
# pop up (e.g. getaddrinfo is in normal ws2_32 already), the test
# pipeline will surface them.
if(WIN32)
    target_link_libraries(nimrtc_vendor_usrsctp
        PUBLIC ws2_32 iphlpapi)
endif()

# Linux/macOS: link pthread (usrsctp creates internal threads via
# user_recv_thread.c, which uses pthread_create).
if(UNIX)
    target_link_libraries(nimrtc_vendor_usrsctp PUBLIC pthread)
endif()

# Force synchronous PDB writes on MSVC for parallel builds.
if(MSVC)
    target_compile_options(nimrtc_vendor_usrsctp PRIVATE /FS)
    # Suppress the upstream C4127 / C4701 / C4244 noise that floods the
    # build. These are upstream-idiomatic warnings (conditionals inside
    # switch arms, intentional conversions in checksum code) that the
    # usrsctp maintainers are not going to fix.
    target_compile_options(nimrtc_vendor_usrsctp PRIVATE
        /wd4127   # conditional expression is constant
        /wd4244   # conversion from 'type1' to 'type2', possible loss of data
        /wd4701   # potentially uninitialized local variable 'name' used
        /wd4200   # nonstandard extension used: zero-sized array in struct/union
        /wd4214   # bit field types other than int
        /wd4706   # assignment within conditional expression
        /wd4245   # conversion from 'type1' to 'type2', signed/unsigned mismatch
        /wd4389   # 'operator' : signed/unsigned mismatch
    )
endif()

# GCC/Clang: silence the same noise.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(nimrtc_vendor_usrsctp PRIVATE
        -Wno-sign-compare
        -Wno-unused-parameter
        -Wno-pointer-sign
        -Wno-missing-field-initializers
        -Wno-implicit-fallthrough
        -Wno-stringop-truncation
        -Wno-stringop-overflow
        -Wno-array-bounds
        -Wno-address-of-packed-member
        -Wno-unused-but-set-variable
        -Wno-unused-variable
        -Wno-unused-function
    )
endif()

target_compile_features(nimrtc_vendor_usrsctp PUBLIC c_std_99)
