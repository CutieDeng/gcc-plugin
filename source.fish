set -x CXX g++
set -x GCC_INCLUDE_PATH ($CXX -print-file-name=include)
set -x GCC_PLUGIN_INCLUDE_PATH ($CXX -print-file-name=plugin)/include
