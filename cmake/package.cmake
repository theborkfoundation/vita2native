# "package" recipe: builds vita2native (armhf) and launcher (arm64) and
# bundles them into a zip whose root directory is `vita2native/`.

set(V2N_PACKAGE_STAGE "${CMAKE_BINARY_DIR}/package-stage")
set(V2N_PACKAGE_ROOT "${V2N_PACKAGE_STAGE}/vita2native")
set(V2N_PACKAGE_ZIP "${CMAKE_BINARY_DIR}/vita2native.zip")
set(V2N_LAUNCHER_ARM64_DIR "${CMAKE_SOURCE_DIR}/build/launcher-arm64-cross")

add_custom_target(package
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${V2N_PACKAGE_STAGE}" "${V2N_PACKAGE_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${V2N_PACKAGE_ROOT}"
    COMMAND ${CMAKE_COMMAND} -S "${CMAKE_SOURCE_DIR}/source/launcher" --preset arm64-cross
    COMMAND ${CMAKE_COMMAND} --build "${V2N_LAUNCHER_ARM64_DIR}" --target launcher
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:vita2native>" "${V2N_PACKAGE_ROOT}/vita2native"
    COMMAND ${CMAKE_COMMAND} -E copy "${V2N_LAUNCHER_ARM64_DIR}/launcher" "${V2N_PACKAGE_ROOT}/launcher"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/source/launcher/launcher.toml.example" "${V2N_PACKAGE_ROOT}/launcher.toml"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/source/launcher/vita2native.sh" "${V2N_PACKAGE_STAGE}/vita2native.sh"
    COMMAND ${CMAKE_COMMAND} -E chdir "${V2N_PACKAGE_STAGE}" ${CMAKE_COMMAND} -E tar cf "${V2N_PACKAGE_ZIP}" --format=zip vita2native.sh vita2native
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS vita2native
    COMMENT "Packaging vita2native (armhf) + launcher (arm64) into ${V2N_PACKAGE_ZIP}"
    USES_TERMINAL
)
