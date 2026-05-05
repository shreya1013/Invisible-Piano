idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver)

target_link_libraries(${COMPONENT_LIB} m)