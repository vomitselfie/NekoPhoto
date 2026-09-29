# Writes a C++ source holding a file's bytes: cmake -DINPUT=<file> -DOUTPUT=<cpp> -DSYMBOL=<name> -P embed_blob.cmake
# The source defines `extern const unsigned char <SYMBOL>[]` and `extern const unsigned long long <SYMBOL>Size` in
# namespace compositor::blobs.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR size "${length} / 2")
# Two hex digits per byte, 32 bytes a line.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
"// Generated from ${INPUT} by embed_blob.cmake; do not edit.\n"
"namespace compositor::blobs {\n"
"extern const unsigned char ${SYMBOL}[];\n"
"extern const unsigned long long ${SYMBOL}Size;\n"
"const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\n"
"const unsigned long long ${SYMBOL}Size = ${size};\n"
"}\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
