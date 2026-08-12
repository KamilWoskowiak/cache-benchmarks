find_package(Doxygen)

# Required settings for Doxygen Awesome.
set(DOXYGEN_GENERATE_TREEVIEW YES)
set(DOXYGEN_DISABLE_INDEX NO)
set(DOXYGEN_FULL_SIDEBAR NO)
set(DOXYGEN_HTML_EXTRA_STYLESHEET "${doxygen_awesome_SOURCE_DIR}/doxygen-awesome.css")
set(DOXYGEN_HTML_COLORSTYLE LIGHT)

set(DOXYGEN_PROJECT_BRIEF "A C++20 framework for concurrent caching")
set(DOXYGEN_PROJECT_NAME "ALCaMI")
set(DOXYGEN_STRIP_FROM_PATH "${alcami_SOURCE_DIR}/include;${alcami_SOURCE_DIR}/build/include")
set(DOXYGEN_JAVADOC_AUTOBRIEF YES)
set(DOXYGEN_EXCLUDE_SYMBOLS "alc::detail,alc::detail::*,phmap::*")
set(DOXYGEN_FORMULA_FONTSIZE 16)

set(DOXYGEN_LAYOUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/DoxygenLayout.xml")

set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
set(DOXYGEN_GENERATE_HTML YES)
set(DOXYGEN_USE_MATHJAX YES)

doxygen_add_docs(alcami-doxygen "${alcami_SOURCE_DIR}/include" "${alcami_BINARY_DIR}/include")
