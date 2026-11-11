# Install rules. GNUInstallDirs is included by cmake/deps.cmake.

install(TARGETS vita2native RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/vita2native.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/x-vita-vpk.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
install(FILES README.md licenses/COPYING.txt DESTINATION ${CMAKE_INSTALL_DOCDIR})
