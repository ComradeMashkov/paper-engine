# SDL compiles an in-memory MSL string. Inline our shared contract because the runtime
# compiler has no repository include path; the generated source remains self-contained.
file(READ "${SHADER_SOURCE}" shader)
file(READ "${SHADER_CONSTANTS}" constants)
string(REPLACE "#include \"../include/paper/render/shader_constants.h\"" "${constants}" shader "${shader}")
file(WRITE "${SHADER_OUTPUT}" "${shader}")
