/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Called after class initialization and before USB enable. The pinned CDC
 * driver registers iInterface but leaves the IAD function name unset. */
static inline bool hub_usb_name_cdc_function(const struct usb_desc_header **descs) {
    struct usb_association_descriptor *association = NULL;
    struct usb_if_descriptor *control = NULL;

    for (unsigned i = 0; i < 32 && descs[i] && descs[i]->bLength; i++) {
        if (descs[i]->bDescriptorType == USB_DESC_INTERFACE_ASSOC &&
            descs[i]->bLength >= sizeof(struct usb_association_descriptor)) {
            association = (struct usb_association_descriptor *)descs[i];
        } else if (descs[i]->bDescriptorType == USB_DESC_INTERFACE &&
                   descs[i]->bLength >= sizeof(struct usb_if_descriptor)) {
            struct usb_if_descriptor *iface = (struct usb_if_descriptor *)descs[i];
            if (iface->bInterfaceClass == USB_BCC_CDC_CONTROL) control = iface;
        }
    }

    if (!association || !control || !control->iInterface ||
        association->bFunctionClass != USB_BCC_CDC_CONTROL ||
        association->bFirstInterface != control->bInterfaceNumber) return false;

    association->iFunction = control->iInterface;
    return true;
}

void hub_usb_name_ports(void);
