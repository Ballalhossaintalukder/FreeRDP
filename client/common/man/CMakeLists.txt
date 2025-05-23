if(CMAKE_CROSSCOMPILING)
  find_package(GenerateArgumentManpage)
else()
  include_directories(${CMAKE_BINARY_DIR}/include/)

  add_executable(generate_argument_manpage generate_argument_manpage.c ../cmdline.h)

  include(CompilerDetect)
  if(CMAKE_COMPILER_IS_CLANG OR CMAKE_COMPILER_IS_GNUCC)
    target_compile_options(generate_argument_manpage PRIVATE -fno-sanitize=all)
    target_compile_options(generate_argument_manpage PRIVATE -fno-sanitize=all)

    target_link_options(generate_argument_manpage PRIVATE -fno-sanitize=all)
    target_link_options(generate_argument_manpage PRIVATE -fno-sanitize=all)
  endif()

  export(TARGETS generate_argument_manpage FILE "${CMAKE_BINARY_DIR}/GenerateArgumentManpageConfig.cmake")
endif()

set(MAN_OPTIONS_FILE "${CMAKE_CURRENT_BINARY_DIR}/freerdp-global-options.1")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MAN_OPTIONS_FILE})

add_custom_command(
  OUTPUT ${MAN_OPTIONS_FILE} COMMAND $<TARGET_FILE:generate_argument_manpage> "${MAN_OPTIONS_FILE}"
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} DEPENDS generate_argument_manpage
)
add_custom_target(
  generate_argument_manpage.target DEPENDS ${MAN_OPTIONS_FILE} ${CMAKE_BINARY_DIR}/include/freerdp/config.h
                                           $<TARGET_FILE:generate_argument_manpage>
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
