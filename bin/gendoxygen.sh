#!/usr/bin/env bash
if [ ! -f CMakeLists.txt ]; then
	echo "run from root of project!"
	exit 1
fi
mkdir -p docs
doxygen config/Doxyfile
