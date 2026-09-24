# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


find_package(SCTP REQUIRED)

# Platform backend of the SCTP socket layer: the Linux kernel stack or the usrsctp user-space stack (macOS).
# The shared sources below are platform-agnostic: the backend selection happens here, at the CMake level.
if (APPLE)
    list(APPEND SCTP_BACKEND_SOURCES sctp_socket_usrsctp.cpp)
else ()
    list(APPEND SCTP_BACKEND_SOURCES sctp_socket_linux.cpp sctp_network_server_impl_linux.cpp)
endif ()

add_library(ocudu_gateway
        sctp_socket.cpp
        sctp_network_gateway_common_impl.cpp
        sctp_network_client_impl.cpp
        sctp_network_server_impl.cpp
        sctp_dtls.cpp
        sctp_dtls_ssl.cpp
        udp_network_gateway_impl.cpp
        sctp_network_client_factory.cpp
        sctp_network_server_factory.cpp
        udp_network_gateway_factory.cpp
        ${SCTP_BACKEND_SOURCES}
)

if (OPENSSL_HAS_DTLS_SCTP)
	target_compile_definitions(ocudu_gateway PRIVATE OCUDU_HAVE_OPENSSL_DTLS=1)
	target_link_libraries(ocudu_gateway ocudu_support ocudulog OpenSSL::SSL OpenSSL::Crypto ${SCTP_LIBRARIES})
else ()
	target_link_libraries(ocudu_gateway ocudu_support ocudulog ${SCTP_LIBRARIES})
endif()
