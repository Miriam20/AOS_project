
#----- Add "ADAPTIVECPU" target dynamic library

if (NOT CONFIG_BBQUE_SCHEDPOL_ADAPTIVECPU)
	return(adaptiveCPU)
endif(NOT CONFIG_BBQUE_SCHEDPOL_ADAPTIVECPU)

# Set the macro for the scheduling policy loading
if (CONFIG_BBQUE_SCHEDPOL_DEFAULT_ADAPTIVECPU)
  set (BBQUE_SCHEDPOL_DEFAULT "adaptiveCPU" CACHE STRING
	  "Setting scheduling policy name" FORCE)
endif (CONFIG_BBQUE_SCHEDPOL_DEFAULT_ADAPTIVECPU)

set(PLUGIN_ADAPTIVECPU_SRC adaptiveCPU_schedpol adaptiveCPU_plugin)

add_library(bbque_schedpol_adaptiveCPU MODULE ${PLUGIN_ADAPTIVECPU_SRC})

target_link_libraries(
	bbque_schedpol_adaptiveCPU
	${Boost_LIBRARIES}
)

install(TARGETS bbque_schedpol_adaptiveCPU LIBRARY
		DESTINATION ${BBQUE_PATH_PLUGINS}
		COMPONENT BarbequeRTRM)
