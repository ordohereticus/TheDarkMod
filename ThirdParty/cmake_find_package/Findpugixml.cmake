include(${CMAKE_CURRENT_LIST_DIR}/tdm_find_package.cmake)

set(pugixml_FOUND 1)
set(pugixml_INCLUDE_DIRS "${ARTEFACTS_DIR}/pugixml/include")
set(pugixml_LIBRARY_DIR "${ARTEFACTS_DIR}/pugixml/lib/${PACKAGE_PLATFORM}")
set(pugixml_LIBRARY_D_DIR "${ARTEFACTS_DIR}/pugixml/lib/${PACKAGE_PLATFORM_DEBUG}")
if(MSVC)
	set(pugixml_LIBRARIES "${pugixml_LIBRARY_DIR}/pugixml.lib")
	set(pugixml_LIBRARIES_D "${pugixml_LIBRARY_D_DIR}/pugixml.lib")
else()
	set(pugixml_LIBRARIES "${pugixml_LIBRARY_DIR}/libpugixml.a")
	set(pugixml_LIBRARIES_D "${pugixml_LIBRARY_D_DIR}/libpugixml.a")
endif()
