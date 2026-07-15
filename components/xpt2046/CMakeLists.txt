idf_component_register(SRCS "xpt2046.c"
	                    INCLUDE_DIRS "."
						PRIV_REQUIRES main
						)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format)
