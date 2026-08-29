cmake_minimum_required(VERSION 3.20)
project(LimitOrderBook)

option(
    ORDERBOOK_ENABLE_IPO
    "Enable supported interprocedural optimization in release builds"
    ON
)

option(
    ORDERBOOK_ENABLE_NATIVE_ARCH
    "Tune release binaries for the build host CPU (non-portable)"
    OFF
)

set(ORDERBOOK_IPO_SUPPORTED OFF)
if(ORDERBOOK_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ORDERBOOK_IPO_SUPPORTED OUTPUT ORDERBOOK_IPO_ERROR)
    if(NOT ORDERBOOK_IPO_SUPPORTED)
        message(WARNING "IPO is unavailable: ${ORDERBOOK_IPO_ERROR}")
    endif()
endif()

option(
    ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS
    "Collect active-ID probe counts on the command path"
    OFF
)

option(
    ORDERBOOK_USE_FLAT_PRICE_BITMAP
    "Use a flat occupied-price bitmap for books capped near 1024 price levels"
    OFF
)

# main orderbook library
add_library(orderbook 
    src/order_book.cpp
    src/fast_order_book.cpp
)

target_include_directories(orderbook
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(orderbook PUBLIC cxx_std_20)
set_target_properties(orderbook PROPERTIES CXX_EXTENSIONS OFF)

if(ORDERBOOK_IPO_SUPPORTED)
    set_property(TARGET orderbook PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

target_compile_definitions(orderbook
    PUBLIC
        ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS=$<BOOL:${ORDERBOOK_ENABLE_ID_INDEX_DIAGNOSTICS}>
        ORDERBOOK_USE_FLAT_PRICE_BITMAP=$<BOOL:${ORDERBOOK_USE_FLAT_PRICE_BITMAP}>
)

if(MSVC)
    target_compile_options(orderbook PRIVATE /W4 /permissive-)
    if(ORDERBOOK_ENABLE_NATIVE_ARCH)
        target_compile_options(orderbook PRIVATE $<$<CONFIG:Release>:/arch:AVX2>)
    endif()
else()
    target_compile_options(orderbook PRIVATE -Wall -Wextra -Wpedantic)
    if(ORDERBOOK_ENABLE_NATIVE_ARCH)
        target_compile_options(orderbook PRIVATE $<$<CONFIG:Release>:-march=native>)
    endif()
endif()

# matching wrapper library
add_library(matchingengine
    src/matching_engine.cpp
)

target_link_libraries(matchingengine PUBLIC orderbook)

target_include_directories(matchingengine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"    
)

target_compile_features(matchingengine PUBLIC cxx_std_20)

if(ORDERBOOK_IPO_SUPPORTED)
    set_property(TARGET matchingengine PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# interactive command-line client
add_executable(orderbook_cli
    app/orderbook_cli.cpp
)

target_link_libraries(orderbook_cli PRIVATE matchingengine)
target_compile_features(orderbook_cli PRIVATE cxx_std_20)
set_target_properties(orderbook_cli PROPERTIES CXX_EXTENSIONS OFF)

if(ORDERBOOK_IPO_SUPPORTED)
    set_property(TARGET orderbook_cli PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

if(MSVC)
    target_compile_options(orderbook_cli PRIVATE /W4 /permissive-)
else()
    target_compile_options(orderbook_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()


# testing suite
include(CTest)

if(BUILD_TESTING)
    add_executable(orderbook_tests
        test/order_book_tests.cpp
    )

    target_link_libraries(orderbook_tests PRIVATE orderbook)

    add_executable(orderbook_randomized_tests
        test/randomized_tests.cpp
    )

    target_link_libraries(orderbook_randomized_tests PRIVATE orderbook)

    add_executable(orderbook_components_tests
        test/order_pool_tests.cpp
    )

    target_link_libraries(orderbook_components_tests PRIVATE orderbook)

    foreach(test_target orderbook_tests orderbook_randomized_tests orderbook_components_tests)
        set_target_properties(${test_target} PROPERTIES CXX_EXTENSIONS OFF)
        if(ORDERBOOK_IPO_SUPPORTED)
            set_property(
                TARGET ${test_target}
                PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
            )
        endif()
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /W4 /permissive-)
        else()
            target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()

    add_test(
        NAME orderbook_tests
        COMMAND orderbook_tests
    )

    add_test(
        NAME orderbook_randomized_tests
        COMMAND orderbook_randomized_tests
    )

    add_test(
        NAME orderbook_components_tests
        COMMAND orderbook_components_tests
    )

    option(
        ORDERBOOK_ENABLE_SANITIZERS
        "Build tests with AddressSanitizer and, where available, UndefinedBehaviorSanitizer"
        OFF
    )

    if(ORDERBOOK_ENABLE_SANITIZERS)
        if(MSVC)
            # MSVC supports AddressSanitizer but not UndefinedBehaviorSanitizer.
            target_compile_options(orderbook PRIVATE /fsanitize=address)
            target_compile_options(matchingengine PRIVATE /fsanitize=address)
            target_compile_options(orderbook_cli PRIVATE /fsanitize=address)
            target_link_options(orderbook_cli PRIVATE /INCREMENTAL:NO)
            foreach(test_target orderbook_tests orderbook_randomized_tests orderbook_components_tests)
                target_compile_options(${test_target} PRIVATE /fsanitize=address)
                target_link_options(${test_target} PRIVATE /INCREMENTAL:NO)
            endforeach()
        else()
            set(SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_compile_options(orderbook PRIVATE ${SANITIZER_FLAGS})
            target_compile_options(matchingengine PRIVATE ${SANITIZER_FLAGS})
            target_compile_options(orderbook_cli PRIVATE ${SANITIZER_FLAGS})
            target_link_options(orderbook_cli PRIVATE ${SANITIZER_FLAGS})
            foreach(test_target orderbook_tests orderbook_randomized_tests orderbook_components_tests)
                target_compile_options(${test_target} PRIVATE ${SANITIZER_FLAGS})
                target_link_options(${test_target} PRIVATE ${SANITIZER_FLAGS})
            endforeach()
        endif()
    endif()
endif()

# benchmark
add_executable(orderbook_bench 
    bench/order_book_bench.cpp
)

target_link_libraries(orderbook_bench PRIVATE orderbook)

if(ORDERBOOK_IPO_SUPPORTED)
    set_property(TARGET orderbook_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

if(ORDERBOOK_ENABLE_NATIVE_ARCH)
    if(MSVC)
        target_compile_options(orderbook_bench PRIVATE $<$<CONFIG:Release>:/arch:AVX2>)
    else()
        target_compile_options(orderbook_bench PRIVATE $<$<CONFIG:Release>:-march=native>)
    endif()
endif()

if(ORDERBOOK_ENABLE_SANITIZERS)
    if(MSVC)
        target_compile_options(orderbook_bench PRIVATE /fsanitize=address)
        target_link_options(orderbook_bench PRIVATE /INCREMENTAL:NO)
    else()
        target_compile_options(orderbook_bench PRIVATE ${SANITIZER_FLAGS})
        target_link_options(orderbook_bench PRIVATE ${SANITIZER_FLAGS})
    endif()
endif()

