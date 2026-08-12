/* transport.c - libusb bulk transport for Goodix 27c6:5125
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * DEVICE (verified from fingerprint.pcap, pkt7/pkt9 = 27c6:5125):
 *   USB CDC-ACM device, 2 interfaces:
 *     iface 0: CDC comm (EP 0x82 INT-IN 8B)   - notifications
 *     iface 1: CDC data (EP 0x01 BULK-OUT 64B
 *                        EP 0x81 BULK-IN  64B) - all command/data traffic
 *   接口选择（参考实现 usbhal.c）:
 *     bulk write pipe = iface1 EP 0x01
 *     bulk read  pipe = iface1 EP 0x81
 *     interrupt pipe  = iface0 EP 0x82 (notifications only)
 *
 * CDC-ACM 设备在数据端点激活前需要 SET_LINE_CODING 与
 * SET_CONTROL_LINE_STATE (DTR) 控制请求；参考实现同样如此，
 * 这里复现该行为以保证全新设备能应答命令。
 */
#include <libusb-1.0/libusb.h>
#include "goodix.h"

int gx_debug = 0;   /* set via GOODIX_DEBUG=1 */

#define GF_IFACE_COMM   0   /* CDC comm (interrupt EP 0x82) */

/* CDC class requests */
#define CDC_SET_LINE_CODING       0x20
#define CDC_SET_CONTROL_LINE_STATE 0x22
#define CDC_REQ_TYPE_HOST_TO_CLASS 0x21

/* claim one interface; detach kernel driver if present */
static int claim_iface(libusb_device_handle *h, int iface)
{
    int r = libusb_kernel_driver_active(h, iface);
    if (r == 1) {
        if (libusb_detach_kernel_driver(h, iface) < 0)
            LOG("detach kernel driver iface %d failed", iface);
    }
    r = libusb_claim_interface(h, iface);
    if (r < 0)
        LOG("claim iface %d failed: %s", iface, libusb_error_name(r));
    return r;
}

/* CDC activation (SET_LINE_CODING + SET_CONTROL_LINE_STATE/DTR).
 *
 * WHY THIS IS REQUIRED (root-cause of zero-response on Linux):
 *   27c6:5125 is a CDC-ACM device. The Linux cdc_acm kernel driver binds
 *   it on plug-in and issues SET_LINE_CODING + SET_CONTROL_LINE_STATE
 *   (DTR=1), which "activates" the device data endpoints. When we
 *   libusb_detach_kernel_driver() we take the interface back WITHOUT
 *   DTR, so the device falls back to an inactive state and silently
 *   drops every bulk command - exactly the zero-RX behavior seen.
 *   抓包记录在设备已被激活之后才进行（首帧是运行中的检测循环 0x32，
 *   而非初始化序列），因此看不到 CDC 控制请求——更早的激活发生在
 *   抓包窗口之外。
 *   We therefore must send the same two CDC control requests ourselves.
 */
static int cdc_activate(libusb_device_handle *h, int iface)
{
    int r;
    /* SET_LINE_CODING: 115200 8N1 (data rate is ignored by device) */
    uint8_t coding[7];
    uint32_t rate = 115200;
    coding[0] = (uint8_t)(rate & 0xFF);
    coding[1] = (uint8_t)((rate >> 8) & 0xFF);
    coding[2] = (uint8_t)((rate >> 16) & 0xFF);
    coding[3] = (uint8_t)((rate >> 24) & 0xFF);
    coding[4] = 0;   /* 1 stop bit */
    coding[5] = 0;   /* no parity */
    coding[6] = 8;   /* 8 data bits */
    r = libusb_control_transfer(h, 0x21, 0x20, 0, (uint16_t)iface,
                                coding, sizeof(coding), 1000);
    if (r < 0)
        LOG("SET_LINE_CODING iface %d: %s", iface, libusb_error_name(r));
    else
        LOG("SET_LINE_CODING ok (%d)", r);

    /* SET_CONTROL_LINE_STATE: DTR=1 | RTS=1 */
    r = libusb_control_transfer(h, 0x21, 0x22, 0x03, (uint16_t)iface,
                                NULL, 0, 1000);
    if (r < 0)
        LOG("SET_CONTROL_LINE_STATE iface %d: %s", iface, libusb_error_name(r));
    else
        LOG("SET_CONTROL_LINE_STATE DTR+RTS ok");
    return r < 0 ? r : 0;
}

/* find bulk endpoints on the data interface, store into dev struct */
static int discover_endpoints(struct goodix_dev *d)
{
    libusb_device *dev = libusb_get_device((libusb_device_handle *)d->usb_devh);
    struct libusb_config_descriptor *cfg = NULL;
    int r;

    r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r < 0)
        return r;

    for (int i = 0; i < cfg->bNumInterfaces && r == 0; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            if (alt->bInterfaceNumber != GOODIX_IFACE_DATA)
                continue;
            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                uint8_t addr = ep->bEndpointAddress;
                if ((ep->bmAttributes & 0x3) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if (addr & 0x80)
                    d->ep_in = addr;
                else
                    d->ep_out = addr;
            }
        }
    }
    libusb_free_config_descriptor(cfg);

    /* fallback to known values (verified from pcap) */
    if (!d->ep_in)
        d->ep_in = 0x81;
    if (!d->ep_out)
        d->ep_out = 0x01;

    LOG("endpoints: OUT 0x%02x IN 0x%02x (iface %d)",
        d->ep_out, d->ep_in, GOODIX_IFACE_DATA);
    return 0;
}

int gx_transport_open(struct goodix_dev *d)
{
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    ssize_t cnt;
    int r = -1;

    if (libusb_init(&ctx) < 0)
        return -1;
    d->usb_ctx = ctx;

    cnt = libusb_get_device_list(ctx, &list);
    if (cnt < 0)
        return (int)cnt;
    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd))
            continue;
        if (dd.idVendor == GOODIX_VID &&
            (dd.idProduct == d->pid ||
             (!d->pid && (dd.idProduct == GOODIX_PID_5125 ||
                          dd.idProduct == GOODIX_PID_5135)))) {
            if (libusb_open(list[i], (libusb_device_handle **)&d->usb_devh) == 0) {
                libusb_device_handle *h = (libusb_device_handle *)d->usb_devh;
                d->pid = dd.idProduct;
                /* 占用两个 CDC 接口（comm + data） */
                r = claim_iface(h, GF_IFACE_COMM);
                if (r == 0)
                    r = claim_iface(h, GOODIX_IFACE_DATA);
                if (r == 0) {
                    discover_endpoints(d);
                    /* Activate device data endpoints (DTR). cdc_acm did
                     * this before we detached it; we must redo it. */
                    cdc_activate(h, GF_IFACE_COMM);
                    LOG("opened %04x:%04x", GOODIX_VID, d->pid);
                    break;
                }
                libusb_close(h);
                d->usb_devh = NULL;
            }
        }
    }
    libusb_free_device_list(list, 1);
    if (r != 0) {
        libusb_exit(ctx);
        d->usb_ctx = NULL;
    }
    return r;
}


/* USB 复位：参考实现（WDF）在设备添加时复位端口（端口复位发生在
 * 参考实现的 USB 初始化中）。libusb 打开时不会复位端口，因此
 * 冷启动设备（挂起后）可能需要一次复位来唤醒 MCU 的 USB 外设。 */
int gx_usb_reset(struct goodix_dev *d)
{
    int r = libusb_reset_device((libusb_device_handle *)d->usb_devh);
    if (r < 0)
        LOG("usb reset failed: %s", libusb_error_name(r));
    else
        LOG("usb reset done");
    return r;
}

void gx_transport_close(struct goodix_dev *d)
{
    if (d->usb_devh) {
        libusb_release_interface((libusb_device_handle *)d->usb_devh, GOODIX_IFACE_DATA);
        libusb_release_interface((libusb_device_handle *)d->usb_devh, GF_IFACE_COMM);
        libusb_close((libusb_device_handle *)d->usb_devh);
        d->usb_devh = NULL;
    }
    if (d->usb_ctx) {
        libusb_exit((libusb_context *)d->usb_ctx);
        d->usb_ctx = NULL;
    }
}

int gx_usb_write(struct goodix_dev *d, const uint8_t *b, size_t n)
{
    int x = 0;
    int r = libusb_bulk_transfer((libusb_device_handle *)d->usb_devh,
                                 d->ep_out ? d->ep_out : 0x01,
                                 (uint8_t *)b, (int)n, &x, 1000);
    if (gx_debug) {
        fprintf(stderr, "[goodix] TX %zuB: ", n);
        for (size_t i = 0; i < n && i < 32; i++)
            fprintf(stderr, "%02X ", b[i]);
        fprintf(stderr, "\n");
    }
    if (r < 0)
        LOG("usb write ep 0x%02x: %s", d->ep_out, libusb_error_name(r));
    return r < 0 ? r : x;
}

int gx_usb_read(struct goodix_dev *d, uint8_t *b, size_t cap, int tmo)
{
    int x = 0;
    int r = libusb_bulk_transfer((libusb_device_handle *)d->usb_devh,
                                 d->ep_in ? d->ep_in : 0x81,
                                 b, (int)cap, &x, tmo);
    if (gx_debug && r >= 0) {
        fprintf(stderr, "[goodix] RX %dB: ", x);
        for (int i = 0; i < x && i < 32; i++)
            fprintf(stderr, "%02X ", b[i]);
        fprintf(stderr, "\n");
    }
    if (r < 0 && r != LIBUSB_ERROR_TIMEOUT)
        LOG("usb read ep 0x%02x: %s", d->ep_in, libusb_error_name(r));
    return r < 0 ? r : x;
}
