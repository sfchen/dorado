OPTION(BUILD_KOI_FROM_SOURCE OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)

    if(BUILD_KOI_FROM_SOURCE)
        message("-- Building Koi from source")

        set(KOI_DIR "${DORADO_3RD_PARTY}/koi")
	
        if(NOT EXISTS ${KOI_DIR})
            if(DEFINED GITLAB_CI_TOKEN)
                message("Cloning Koi using CI token")
                execute_process(COMMAND git clone https://gitlab-ci-token:${GITLAB_CI_TOKEN}@git.oxfordnanolabs.local/machine-learning/koi.git ${KOI_DIR})
            else()
                message("Cloning Koi using ssh")
                execute_process(COMMAND git clone git@git.oxfordnanolabs.local:machine-learning/koi.git ${KOI_DIR})
            endif()
        endif()
        execute_process(COMMAND git checkout dbb8369ddbc5710d5f3b47d48f2aff098e33a6a9 WORKING_DIRECTORY ${KOI_DIR})
        add_subdirectory(${KOI_DIR}/koi/lib)

	set(KOI_INCLUDE ${KOI_DIR}/koi/lib)
	set(KOI_LIBRARIES koi)

    else()

        message("-- Using prebuilt Koi from ${KOI_DIR}")

        if(LINUX)
            download_and_extract(https://nanoporetech.box.com/shared/static/qbasibmplodr2ixztz97v53vmkttxia1.gz koi_lib)
        elseif(WIN32)
            download_and_extract(https://nanoporetech.box.com/shared/static/wth54wdx8ls5w3m3wvb9c2s6ng65ahj2.zip koi_lib)
        endif()

        file(GLOB KOI_DIR "${DORADO_3RD_PARTY}/koi_lib/*")
	set(KOI_INCLUDE ${KOI_DIR}/include)
	set(KOI_LIBRARIES ${KOI_DIR}/lib/libkoi.a)

    endif()

endif()

# export koi KOI_INCLUDE, KOI_LIB
