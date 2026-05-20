# Custom DHCP server component.
#
# Wraps the ESP-IDF built-in dhcpserver_* symbols via -Wl,--wrap so the
# whole upstream implementation is replaced at link time by our copy in
# this folder. CONFIG_LWIP_DHCPS stays enabled — the Kconfig path keeps
# working, only the function bodies are ours.
#
# The reservation hook is dependency-inverted: the storage layer in
# main/dhcp_reservations.c registers a lookup callback via
# dhcps_set_reservation_lookup(), so this component does not depend on
# main and can be reused outside this project.

idf_component_register(
    SRCS "dhcpserver.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip
)

# Same warning suppressions as the upstream lwip component carries.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-address
    -Wno-unused-but-set-variable
)

# Public dhcpserver_* symbols we replace. For each one, --wrap rewrites
# every call site to land in our __wrap_<name>, and -u forces the symbol
# to be pulled in even when the only caller is inside another static
# library (otherwise the linker would drop it before the wrap takes
# effect).
set(DHCPS_WRAP_FUNCTIONS
    dhcps_new
    dhcps_delete
    dhcps_start
    dhcps_stop
    dhcps_option_info
    dhcps_set_option_info
    dhcp_search_ip_on_mac
    dhcps_dns_setserver
    dhcps_dns_setserver_by_type
    dhcps_dns_getserver
    dhcps_dns_getserver_by_type
    dhcps_set_new_lease_cb
)

foreach(func ${DHCPS_WRAP_FUNCTIONS})
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=${func}"
        "-u __wrap_${func}"
    )
endforeach()
