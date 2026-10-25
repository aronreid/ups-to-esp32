/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Does a descriptor in a configuration descriptor really have the bytes its
 * type's fields live in? Pure, so tools/test/test_power_iface.py can compile
 * it on the host.
 *
 * ESP-IDF's usb_parse_next_descriptor() promises a returned descriptor only
 * the two bytes bLength and bDescriptorType. A device that ends its
 * configuration with an interface descriptor whose bLength is 9 but of which
 * only 2 bytes lie inside wTotalLength made the scan read up to 7 bytes past
 * the heap buffer holding it. Read-only, but a hostile device chooses what
 * the scan does with whatever it finds there.
 */
#ifndef UPSA_USB_DESC_CHECK_H
#define UPSA_USB_DESC_CHECK_H

#include <stdbool.h>
#include <stdint.h>

/* `offset`: where the descriptor starts in the configuration descriptor.
 * `need`: bytes the fields about to be read span (9 for an interface, 7 for an
 * endpoint, 9 for HID). True if bLength covers them and the descriptor ends
 * inside the configuration descriptor. */
static inline bool usb_desc_fits(int offset, uint8_t bLength, uint16_t wTotalLength, uint8_t need)
{
    return offset >= 0 && bLength >= need && (int)offset + (int)bLength <= (int)wTotalLength;
}

#endif
