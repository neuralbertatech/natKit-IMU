idf_component_register(
    SRCS
        "sh2.c"
        "sh2_SensorValue.c"
        "sh2_util.c"
        "shtp.c"
    INCLUDE_DIRS
        "include"
)

# Third-party code, compiled as-is. See README.md for provenance: this is
# Hillcrest/CEVA's own SH-2 driver, byte-identical to the copy the Arduino
# firmware links, which is what makes a recording from this fork comparable with
# one from ../embeded.
#
# The warnings below are the vendor's, not ours. They are silenced rather than
# fixed because patching vendored sources is how a copy stops being comparable --
# and every local edit would have to be re-applied on any driver update.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-sign-compare
)
