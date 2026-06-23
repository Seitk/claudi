# claudi_imu — QMI8658 accelerometer reads over the shared I2C bus (new I2C
# master driver). Mirrors claudi_power: main passes bsp_i2c_get_handle().
idf_component_register(
    SRCS "claudi_imu.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
