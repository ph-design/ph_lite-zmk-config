/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include "usb-port-names.h"

void hub_usb_name_ports(void) {
    const struct device *management = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
    const struct device *studio = DEVICE_DT_GET(DT_NODELABEL(studio_radio_uart));

    STRUCT_SECTION_FOREACH_ALTERNATE(usbd_class_fs, usbd_class_node, node) {
        struct usbd_class_data *data = node->c_data;
        if ((usbd_class_get_private(data) == management ||
             usbd_class_get_private(data) == studio) && data->api->get_desc) {
            hub_usb_name_cdc_function(data->api->get_desc(data, USBD_SPEED_FS));
        }
    }
}
