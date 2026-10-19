# src/web_transport/src/CMakeLists.txt
# No separate compilation unit — all source lives in web_transport_msquic_session.cpp
# and web_transport_plugin.cpp (listed in ../CMakeLists.txt).
# This file exists so CMake's add_subdirectory() can descend into this directory
# without error if needed in the future.
