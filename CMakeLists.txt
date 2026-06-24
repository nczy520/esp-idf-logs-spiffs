idf_component_register(
    SRCS 
        "src/my_component.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nvs_flash
    # PRIV_REQUIRES  # 私有依赖
)