idf_component_register(SRCS "common_buffer.c"
	                    INCLUDE_DIRS "include"
						PRIV_REQUIRES fifo
						)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format)
