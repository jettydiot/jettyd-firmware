/*
 * Host-test stub for the ESP-IDF generated sdkconfig.h.
 *
 * Sources compiled into host unit tests (e.g. wifi.c) include "sdkconfig.h".
 * On the host there is no menuconfig-generated header, so this empty shim
 * satisfies the include. CONFIG_* symbols stay undefined here, so
 * `#if CONFIG_SOC_WIFI_SUPPORTED` evaluates false and radio-independent code
 * paths are selected. Tests that need a specific CONFIG_* value define it on
 * the compiler command line (see test/Makefile).
 */
#ifndef JETTYD_HOST_TEST_SDKCONFIG_H
#define JETTYD_HOST_TEST_SDKCONFIG_H

#endif /* JETTYD_HOST_TEST_SDKCONFIG_H */
