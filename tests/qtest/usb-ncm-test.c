/*
 * QTest testcase for USB CDC NCM device (usb-ncm)
 *
 * Copyright (c) 2026 The QEMU contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "libqos/usb.h"

/*
 * Cold-plug regression. usb_desc_init() defaults dev->speed to FULL
 * before any port attach, so if the device only provides .high/.super
 * USBDescDevices (no .full), dev->device stays NULL until the host
 * controller's port attach re-runs setdefaults via handle_attach.
 * Prior to the handle_attach=usb_desc_attach wiring this segfaulted
 * on the very first GET_DESCRIPTOR after enumeration. Boot-time
 * cold-plug exercises that path.
 */
static void test_usb_ncm_coldplug(void)
{
    QTestState *qts = qtest_init(
        "-device qemu-xhci,id=xhci"
        " -netdev user,id=n0"
        " -device usb-ncm,netdev=n0");
    qtest_quit(qts);
}

/*
 * Hot-plug: device_add / device_del through QMP. Catches reference
 * count, alias, and unrealize bugs.
 */
static void test_usb_ncm_hotplug(void)
{
    QTestState *qts = qtest_init(
        "-device qemu-xhci,id=xhci"
        " -netdev user,id=n0");

    qtest_qmp_device_add(qts, "usb-ncm", "ncm0",
                         "{'netdev': 'n0', 'bus': 'xhci.0'}");
    qtest_qmp_device_del(qts, "ncm0");

    /* Re-add to make sure unrealize cleaned up. */
    qtest_qmp_device_add(qts, "usb-ncm", "ncm0",
                         "{'netdev': 'n0', 'bus': 'xhci.0'}");
    qtest_qmp_device_del(qts, "ncm0");

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_device("usb-ncm")) {
        return 0;
    }
    qtest_add_func("/usb-ncm/coldplug", test_usb_ncm_coldplug);
    qtest_add_func("/usb-ncm/hotplug", test_usb_ncm_hotplug);

    return g_test_run();
}
