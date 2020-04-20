
#----- Add "ADAPTIVE_CPU" target dynamic library

if (NOT CONFIG_BBQUE_SCHEDPOL_ADAPTIVE_CPU)
	return(adaptiveCPU)
endif(NOT CONFIG_BBQUE_SCHEDPOL_ADAPTIVE_CPU)

# Set the macro for the scheduling policy loading
if (CONFIG_BBQUE_SCHEDPOL_DEFAULT_ADAPTIVE_CPU)
  set (BBQUE_SCHEDPOL_DEFAULT "adaptive_cpu" CACHE STRING
	  "Setting scheduling policy name" FORCE)
endif (CONFIG_BBQUE_SCHEDPOL_DEFAULT_ADAPTIVE_CPU)

set(PLUGIN_ADAPTIVE_CPU_SRC adaptiveCPU_schedpol adaptiveCPU_plugin)

add_library(bbque_schedpol_adaptiveCPU MODULE ${PLUGIN_ADAPTIVE_CPU_SRC})

target_link_libraries(
	bbque_schedpol_adaptiveCPU
	${Boost_LIBRARIES}
)

install(TARGETS bbque_schedpol_adaptiveCPU LIBRARY
		DESTINATION ${BBQUE_PATH_PLUGINS}
		COMPONENT BarbequeRTRM)
