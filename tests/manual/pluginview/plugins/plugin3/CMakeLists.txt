add_library(tst_manual_plugin3 SHARED
  plugin3.cpp plugin3.h
)

set_target_properties(tst_manual_plugin3 PROPERTIES
  OUTPUT_NAME "plugin3"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
)

target_link_libraries(tst_manual_plugin3 PRIVATE ExtensionSystem
                                         PRIVATE Utils
                                         PRIVATE tst_manual_plugin2)
