#!/bin/sh
# Build the game DLL and copy it into the game folder only if the build succeeded.
cd "$(dirname "$0")/.." || exit 1
out=$(cmd //c "game\build.bat" 2>&1)
echo "$out" | grep -i " error\|built"
echo "$out" | grep -q "^built " || { echo "BUILD FAILED - not deployed"; exit 1; }
cp game/build/dinput8.dll "/c/Program Files (x86)/Steam/steamapps/common/swkotor/dinput8.dll" && echo deployed
