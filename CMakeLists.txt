idf_component_register(SRCS "dsi_ext.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_mm esp_timer
                       PRIV_REQUIRES hal soc)
