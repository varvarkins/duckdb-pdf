include_directories(src/include)

set(EXTENSION_SOURCES src/pdf_extension.cpp src/podofo_reader.cpp)
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})

foreach(target ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_compile_definitions(${target} PRIVATE PDF_USE_PODOFO)
  target_link_libraries(${target} podofo)
endforeach()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
