 ###############################################################
 # 
 # Copyright 2019, Condor Team, Computer Sciences Department,
 # University of Wisconsin-Madison, WI.
 # 
 # Licensed under the Apache License, Version 2.0 (the "License"); you 
 # may not use this file except in compliance with the License.  You may 
 # obtain a copy of the License at 
 # 
 #    http://www.apache.org/licenses/LICENSE-2.0 
 # 
 # Unless required by applicable law or agreed to in writing, software 
 # distributed under the License is distributed on an "AS IS" BASIS, 
 # WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 # See the License for the specific language governing permissions and 
 # limitations under the License. 
 # 
 ############################################################### 


##
# The approach here (foreach-loop over format types)
# is largely informed by how CMake itself generates its
# documentation files.
find_program(SPHINX_EXECUTABLE NAMES sphinx-build)

set(doc_formats "")

if (SPHINX_EXECUTABLE)
	list(APPEND doc_formats man)
	list(APPEND doc_formats html)
endif()

set(doc_format_outputs "")

foreach(format ${doc_formats})

	set(doc_format_output "${CMAKE_CURRENT_BINARY_DIR}/docs/${format}")

	add_custom_command(
		OUTPUT ${doc_format_output}
		COMMAND ${SPHINX_EXECUTABLE}
			-b ${format}
			${CMAKE_SOURCE_DIR}/docs
			${CMAKE_CURRENT_BINARY_DIR}/docs/${format}
	)

	list(APPEND doc_format_outputs ${doc_format_output})

endforeach()

add_custom_target(documentation ALL DEPENDS ${doc_format_outputs})

