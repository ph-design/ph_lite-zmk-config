/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define USB_DESC_INTERFACE_ASSOC 11
#define USB_DESC_INTERFACE 4
#define USB_BCC_CDC_CONTROL 2
struct usb_desc_header { uint8_t bLength, bDescriptorType; };
struct usb_association_descriptor {
    uint8_t bLength, bDescriptorType, bFirstInterface, bInterfaceCount;
    uint8_t bFunctionClass, bFunctionSubClass, bFunctionProtocol, iFunction;
};
struct usb_if_descriptor {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
    uint8_t bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
};
#include "usb-port-names.h"

int main(void) {
    struct usb_association_descriptor management = {8, 11, 0, 2, 2, 2, 0, 0};
    struct usb_association_descriptor studio = {8, 11, 2, 2, 2, 2, 0, 0};
    struct usb_if_descriptor management_if = {9, 4, 0, 0, 1, 2, 2, 0, 7};
    struct usb_if_descriptor studio_if = {9, 4, 2, 0, 1, 2, 2, 0, 8};
    struct usb_desc_header end = {0, 0};
    const struct usb_desc_header *a[] = {(void *)&management, (void *)&management_if, &end};
    const struct usb_desc_header *b[] = {(void *)&studio, (void *)&studio_if, &end};
    assert(hub_usb_name_cdc_function(a) && hub_usb_name_cdc_function(b));
    assert(management.iFunction == 7 && studio.iFunction == 8);
    assert(management.bFirstInterface == 0 && studio.bFirstInterface == 2);
    assert(hub_usb_name_cdc_function(a)); /* initialization is idempotent */
    management.iFunction = 0;
    management_if.iInterface = 0;
    assert(!hub_usb_name_cdc_function(a) && !management.iFunction);
    management_if.iInterface = 7;
    management_if.bInterfaceNumber = 2;
    assert(!hub_usb_name_cdc_function(a) && !management.iFunction);
    management_if.bInterfaceNumber = 0;
    management.bFunctionClass = 3;
    assert(!hub_usb_name_cdc_function(a) && !management.iFunction);
    management.bLength = 2;
    assert(!hub_usb_name_cdc_function(a));
    puts("PASS: distinct CDC interface/function names, preserved interface numbers, idempotence and unsupported descriptors");
}
