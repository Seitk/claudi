# claudi_core — portable behaviour core (snapshot model + state derivation).
# No hardware/IDF deps so it also builds on the host (see test/).
idf_component_register(
    SRCS "claudi_core.c"
    INCLUDE_DIRS "include"
)
