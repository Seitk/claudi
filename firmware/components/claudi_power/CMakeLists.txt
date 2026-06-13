# claudi_power — AXP2101 battery state via XPowersLib over the shared I2C bus.
idf_component_register(
    SRCS "claudi_power.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver XPowersLib
)
