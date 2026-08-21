vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rhajamor/tiny_agent
    REF v0.3.0
    SHA512 0  # replaced with the real hash when the v0.3.0 tag exists
    HEAD_REF main
)

file(INSTALL "${SOURCE_PATH}/include/tiny_agent"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
