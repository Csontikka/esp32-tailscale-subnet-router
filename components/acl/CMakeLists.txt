# ACL (Access Control List) Firewall Component
#
# Stateless IPv4 packet filter with four direction-oriented rule lists.

idf_component_register(
    SRCS "acl.c" "acl_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_netif esp_timer
    PRIV_REQUIRES nvs_flash
)
