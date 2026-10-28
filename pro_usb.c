#include "pro_usb.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb.h>
#include <usb.h>
#include <usb_hid.h>
#include <string.h>
#include <stdio.h>
#include <storage/storage.h>

#define TAG "ProUsb"

#define EP_IN  0x81
#define EP_OUT 0x01
#define EP_SIZE 64

/* Defined in the firmware's private furi_hal_usb_i.h; must match the USB stack's EP0 size. */
#ifndef USB_EP0_SIZE
#define USB_EP0_SIZE 8
#endif

#define PRO_VID 0x057E
#define PRO_PID 0x2009

#define REPORT_INTERVAL_MS 15

#define STICK_CENTER 0x800
#define STICK_RANGE  0x700

/* Any 6 bytes work as the "MAC address". */
static const uint8_t pro_mac[6] = {0x7C, 0xBB, 0x8A, 0xAA, 0xBB, 0xCC};

/* ------------------------------------------------------------------ */
/* USB descriptors                                                     */
/* ------------------------------------------------------------------ */

/* Vendor-defined reports, 63 data bytes each (64 byte packets with the ID). */
static const uint8_t pro_report_desc[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x05, // Usage (Game Pad)
    0xA1, 0x01, // Collection (Application)
    0x06, 0x01, 0xFF, //   Usage Page (Vendor)
    0x15, 0x00, //   Logical Min 0
    0x26, 0xFF, 0x00, //   Logical Max 255
    0x75, 0x08, //   Report Size 8
    0x95, 0x3F, //   Report Count 63
    /* Input reports: standard state, subcommand reply, USB handshake reply */
    0x85, 0x21, 0x09, 0x21, 0x81, 0x02,
    0x85, 0x30, 0x09, 0x30, 0x81, 0x02,
    0x85, 0x81, 0x09, 0x81, 0x81, 0x02,
    /* Output reports: subcommand, rumble only, USB handshake command */
    0x85, 0x01, 0x09, 0x01, 0x91, 0x02,
    0x85, 0x10, 0x09, 0x10, 0x91, 0x02,
    0x85, 0x80, 0x09, 0x80, 0x91, 0x02,
    0x85, 0x82, 0x09, 0x82, 0x91, 0x02,
    0xC0,
};

struct ProIntfDescriptor {
    struct usb_interface_descriptor intf;
    struct usb_hid_descriptor hid;
    struct usb_endpoint_descriptor ep_in;
    struct usb_endpoint_descriptor ep_out;
} __attribute__((packed));

struct ProConfigDescriptor {
    struct usb_config_descriptor config;
    struct ProIntfDescriptor intf_0;
} __attribute__((packed));

static struct usb_device_descriptor pro_dev_desc = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bDeviceSubClass = USB_SUBCLASS_NONE,
    .bDeviceProtocol = USB_PROTO_NONE,
    .bMaxPacketSize0 = USB_EP0_SIZE,
    .idVendor = PRO_VID,
    .idProduct = PRO_PID,
    .bcdDevice = VERSION_BCD(2, 0, 0),
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static struct ProConfigDescriptor pro_cfg_desc = {
    .config =
        {
            .bLength = sizeof(struct usb_config_descriptor),
            .bDescriptorType = USB_DTYPE_CONFIGURATION,
            .wTotalLength = sizeof(struct ProConfigDescriptor),
            .bNumInterfaces = 1,
            .bConfigurationValue = 1,
            .iConfiguration = 0,
            .bmAttributes = 0xA0, // bus powered + remote wakeup, like the real thing
            .bMaxPower = 250, // 500 mA
        },
    .intf_0 =
        {
            .intf =
                {
                    .bLength = sizeof(struct usb_interface_descriptor),
                    .bDescriptorType = USB_DTYPE_INTERFACE,
                    .bInterfaceNumber = 0,
                    .bAlternateSetting = 0,
                    .bNumEndpoints = 2,
                    .bInterfaceClass = 0x03, // HID
                    .bInterfaceSubClass = 0,
                    .bInterfaceProtocol = 0,
                    .iInterface = 0,
                },
            .hid =
                {
                    .bLength = sizeof(struct usb_hid_descriptor),
                    .bDescriptorType = USB_DTYPE_HID,
                    .bcdHID = VERSION_BCD(1, 1, 1),
                    .bCountryCode = 0,
                    .bNumDescriptors = 1,
                    .bDescriptorType0 = USB_DTYPE_HID_REPORT,
                    .wDescriptorLength0 = sizeof(pro_report_desc),
                },
            .ep_in =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = EP_IN,
                    .bmAttributes = USB_EPTYPE_INTERRUPT,
                    .wMaxPacketSize = EP_SIZE,
                    .bInterval = 8,
                },
            .ep_out =
                {
                    .bLength = sizeof(struct usb_endpoint_descriptor),
                    .bDescriptorType = USB_DTYPE_ENDPOINT,
                    .bEndpointAddress = EP_OUT,
                    .bmAttributes = USB_EPTYPE_INTERRUPT,
                    .wMaxPacketSize = EP_SIZE,
                    .bInterval = 8,
                },
        },
};

static const struct usb_string_descriptor str_manuf = USB_STRING_DESC("Nintendo Co., Ltd.");
static const struct usb_string_descriptor str_prod = USB_STRING_DESC("Pro Controller");
static const struct usb_string_descriptor str_serial = USB_STRING_DESC("000000000001");

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t len;
    uint8_t data[EP_SIZE];
} RxPacket;

typedef struct {
    usbd_device* dev;
    FuriHalUsbInterface* prev_if;
    FuriSemaphore* tx_sem;
    FuriMessageQueue* rx_queue;
    FuriMutex* input_mutex;
    FuriThread* worker;

    volatile bool run;
    volatile bool active; // between interface init and deinit
    volatile bool connected;
    volatile bool handshake;
    volatile bool streaming;

    uint32_t buttons;
    int8_t lx, ly, rx, ry;
} ProUsb;

static ProUsb pro;

/* ------------------------------------------------------------------ */
/* Capture log: first LOG_MAX events, dumped to the SD card on exit.   */
/* R = received from host, T = sent to host, E = USB event            */
/* (E codes: 0 deconfigure, 1 configure, 2 wakeup, 3 suspend).         */
/* ------------------------------------------------------------------ */

#define LOG_MAX 120
#define LOG_BYTES 40
#define LOG_PATH "/ext/switch_pro_log.txt"

typedef struct {
    uint32_t tick;
    char dir;
    uint8_t len;
    uint8_t data[LOG_BYTES];
} LogEntry;

static LogEntry logbuf[LOG_MAX];
static volatile uint16_t log_n;

static void log_add(char dir, const uint8_t* d, uint8_t len) {
    if(log_n >= LOG_MAX) return;
    LogEntry* e = &logbuf[log_n];
    e->tick = furi_get_tick();
    e->dir = dir;
    e->len = len;
    memcpy(e->data, d, len < LOG_BYTES ? len : LOG_BYTES);
    log_n++;
}

static void log_event(uint8_t code) {
    log_add('E', &code, 1);
}

static void log_dump(void) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(st);
    if(storage_file_open(f, LOG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char line[160];
        for(uint16_t i = 0; i < log_n; i++) {
            const LogEntry* e = &logbuf[i];
            int n = snprintf(line, sizeof(line), "%lu %c len=%u:", (unsigned long)e->tick, e->dir, e->len);
            uint8_t shown = e->len < LOG_BYTES ? e->len : LOG_BYTES;
            for(uint8_t b = 0; b < shown && n < (int)sizeof(line) - 4; b++) {
                n += snprintf(&line[n], sizeof(line) - n, " %02X", e->data[b]);
            }
            n += snprintf(&line[n], sizeof(line) - n, "\n");
            storage_file_write(f, line, n);
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

/* Fake SPI flash contents the Switch reads while setting the controller up. */
static uint8_t spi_stick_cal[18];

static const uint8_t spi_imu_cal[24] = {
    0xBE, 0xFF, 0x3E, 0x00, 0xF0, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
    0xFE, 0xFF, 0xFE, 0xFF, 0x08, 0x00, 0xE7, 0x3B, 0xE7, 0x3B, 0xE7, 0x3B,
};

static const uint8_t spi_stick_params[18] = {
    0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54,
    0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
};

static const uint8_t spi_colors[13] = {
    0x32, 0x32, 0x32, // body
    0xFF, 0xFF, 0xFF, // buttons
    0xFF, 0xFF, 0xFF, // left grip
    0xFF, 0xFF, 0xFF, // right grip
    0x01,
};

typedef struct {
    uint32_t addr;
    uint8_t len;
    const uint8_t* data;
} SpiRegion;

static SpiRegion spi_regions[] = {
    {0x6020, sizeof(spi_imu_cal), spi_imu_cal},
    {0x603D, sizeof(spi_stick_cal), spi_stick_cal},
    {0x6050, sizeof(spi_colors), spi_colors},
    {0x6086, sizeof(spi_stick_params), spi_stick_params},
    {0x6098, sizeof(spi_stick_params), spi_stick_params},
};

/* Two 12-bit values packed into 3 bytes. */
static void pack12(uint8_t* out, uint16_t a, uint16_t b) {
    out[0] = a & 0xFF;
    out[1] = ((a >> 8) & 0x0F) | ((b & 0x0F) << 4);
    out[2] = (b >> 4) & 0xFF;
}

static void build_spi_tables(void) {
    /* Left stick:  above-center delta, center, below-center delta.
     * Right stick: center, below-center delta, above-center delta. */
    pack12(&spi_stick_cal[0], STICK_RANGE, STICK_RANGE);
    pack12(&spi_stick_cal[3], STICK_CENTER, STICK_CENTER);
    pack12(&spi_stick_cal[6], STICK_RANGE, STICK_RANGE);
    pack12(&spi_stick_cal[9], STICK_CENTER, STICK_CENTER);
    pack12(&spi_stick_cal[12], STICK_RANGE, STICK_RANGE);
    pack12(&spi_stick_cal[15], STICK_RANGE, STICK_RANGE);
}

/* Anything not covered by a region reads as 0xFF, which the Switch treats as
 * "not set" (no serial number, no user calibration). */
static void spi_read(uint32_t addr, uint8_t len, uint8_t* out) {
    memset(out, 0xFF, len);
    for(size_t r = 0; r < COUNT_OF(spi_regions); r++) {
        const SpiRegion* reg = &spi_regions[r];
        for(uint8_t i = 0; i < len; i++) {
            uint32_t a = addr + i;
            if(a >= reg->addr && a < reg->addr + reg->len) {
                out[i] = reg->data[a - reg->addr];
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Report building and sending                                         */
/* ------------------------------------------------------------------ */

static uint16_t stick_val(int8_t d) {
    return (uint16_t)(STICK_CENTER + d * STICK_RANGE);
}

/* Fills bytes 1..12 of a standard input report. r[0] must already hold the ID. */
static void fill_state(uint8_t* r) {
    furi_mutex_acquire(pro.input_mutex, FuriWaitForever);
    uint32_t buttons = pro.buttons;
    int8_t lx = pro.lx, ly = pro.ly, rx = pro.rx, ry = pro.ry;
    furi_mutex_release(pro.input_mutex);

    r[1] = (uint8_t)(furi_get_tick() / 5); // timer
    r[2] = 0x90; // battery full, USB powered
    r[3] = buttons & 0xFF;
    r[4] = (buttons >> 8) & 0xFF;
    r[5] = (buttons >> 16) & 0xFF;
    pack12(&r[6], stick_val(lx), stick_val(ly));
    pack12(&r[9], stick_val(rx), stick_val(ry));
    r[12] = 0xB0; // vibrator report
}

static bool pro_send(const uint8_t* buf) {
    if(!pro.active || !pro.connected) return false;
    if(furi_semaphore_acquire(pro.tx_sem, 30) != FuriStatusOk) return false;
    if(!pro.connected) return false;
    static bool logged_stream = false;
    if(buf[0] != 0x30 || !logged_stream) {
        if(buf[0] == 0x30) logged_stream = true;
        log_add('T', buf, EP_SIZE);
    }
    usbd_ep_write(pro.dev, EP_IN, buf, EP_SIZE);
    return true;
}

static void send_input_report(void) {
    uint8_t r[EP_SIZE] = {0};
    r[0] = 0x30;
    fill_state(r);
    pro_send(r);
}

static void send_usb_reply(uint8_t cmd, const uint8_t* data, size_t len) {
    uint8_t r[EP_SIZE] = {0};
    r[0] = 0x81;
    r[1] = cmd;
    if(len) memcpy(&r[2], data, len);
    pro_send(r);
}

static void send_subcmd_reply(uint8_t ack, uint8_t subcmd, const uint8_t* data, size_t len) {
    uint8_t r[EP_SIZE] = {0};
    r[0] = 0x21;
    fill_state(r);
    r[13] = ack;
    r[14] = subcmd;
    if(len > sizeof(r) - 15) len = sizeof(r) - 15;
    if(len) memcpy(&r[15], data, len);
    pro_send(r);
}

/* ------------------------------------------------------------------ */
/* Protocol handling                                                   */
/* ------------------------------------------------------------------ */

static void handle_usb_cmd(uint8_t cmd) {
    switch(cmd) {
    case 0x01: { // request MAC / controller type
        uint8_t d[8] = {0x00, 0x03}; // 0x03 = Pro Controller
        memcpy(&d[2], pro_mac, 6);
        send_usb_reply(0x01, d, sizeof(d));
        break;
    }
    case 0x02: // handshake
        pro.handshake = true;
        send_usb_reply(0x02, NULL, 0);
        break;
    case 0x03: // 3 Mbaud, ignored over USB
        send_usb_reply(0x03, NULL, 0);
        break;
    case 0x04: // HID-only mode, no timeout: start streaming
        pro.streaming = true;
        break;
    case 0x05: // re-enable the timeout, nothing to do
    default:
        break;
    }
}

static void handle_subcommand(const RxPacket* p) {
    if(p->len < 11) return;
    uint8_t sub = p->data[10];
    const uint8_t* args = &p->data[11];

    switch(sub) {
    case 0x02: { // device info
        uint8_t d[12] = {0x03, 0x48, 0x03, 0x02};
        memcpy(&d[4], pro_mac, 6);
        d[10] = 0x01;
        d[11] = 0x01; // use colors from SPI
        send_subcmd_reply(0x82, sub, d, sizeof(d));
        break;
    }
    case 0x10: { // SPI flash read: addr(4, LE) len(1)
        uint32_t addr = args[0] | (args[1] << 8) | (args[2] << 16) | ((uint32_t)args[3] << 24);
        uint8_t len = args[4];
        if(len > 35) len = 35;
        uint8_t d[5 + 35];
        memcpy(d, args, 5);
        spi_read(addr, len, &d[5]);
        send_subcmd_reply(0x90, sub, d, 5 + len);
        break;
    }
    case 0x01: { // Bluetooth manual pairing, three stages (the Switch does this over USB too)
        uint8_t stage = args[0];
        uint8_t d[24] = {stage};
        size_t dl = 1;
        if(stage == 0x01) { // host sent its address; answer with ours
            memcpy(&d[1], pro_mac, 6);
            d[7] = 0x00;
            d[8] = 0x03; // controller type: Pro Controller
            dl = 9;
        } else if(stage == 0x02) { // 16 byte key exchange; echo it back for now
            memcpy(&d[1], &args[1], 16);
            dl = 17;
        }
        send_subcmd_reply(0x81, sub, d, dl);
        break;
    }
    case 0x04: { // trigger button elapsed time
        uint8_t d[14] = {0};
        send_subcmd_reply(0x83, sub, d, sizeof(d));
        break;
    }
    default: // set mode, lights, IMU, vibration, shipment mode, ...: plain ack
        pro.streaming = true;
        send_subcmd_reply(0x80, sub, NULL, 0);
        break;
    }
}

static void handle_rx(const RxPacket* p) {
    if(p->len < 1) return;
    log_add('R', p->data, p->len);
    switch(p->data[0]) {
    case 0x80:
        if(p->len >= 2) handle_usb_cmd(p->data[1]);
        break;
    case 0x01:
        handle_subcommand(p);
        break;
    default: // 0x10 rumble-only and anything else is ignored
        break;
    }
}

static int32_t pro_worker(void* ctx) {
    UNUSED(ctx);
    RxPacket pkt;
    uint32_t last = furi_get_tick();

    while(pro.run) {
        if(furi_message_queue_get(pro.rx_queue, &pkt, 4) == FuriStatusOk) {
            handle_rx(&pkt);
        }
        uint32_t now = furi_get_tick();
        if(pro.streaming && pro.connected && (now - last) >= REPORT_INTERVAL_MS) {
            last = now;
            send_input_report();
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* USB interface callbacks (run on the USB stack's context)            */
/* ------------------------------------------------------------------ */

static void pro_ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    if(event == usbd_evt_eptx) {
        furi_semaphore_release(pro.tx_sem);
    } else if(event == usbd_evt_eprx) {
        RxPacket pkt;
        int32_t n = usbd_ep_read(dev, ep, pkt.data, sizeof(pkt.data));
        if(n > 0) {
            pkt.len = (uint8_t)n;
            furi_message_queue_put(pro.rx_queue, &pkt, 0);
        }
    }
}

static usbd_respond pro_ep_config(usbd_device* dev, uint8_t cfg) {
    switch(cfg) {
    case 0: // deconfigure
        usbd_ep_deconfig(dev, EP_IN);
        usbd_ep_deconfig(dev, EP_OUT);
        usbd_reg_endpoint(dev, EP_IN, NULL);
        usbd_reg_endpoint(dev, EP_OUT, NULL);
        pro.connected = false;
        pro.handshake = false;
        pro.streaming = false;
        log_event(0);
        return usbd_ack;
    case 1: // configure
        usbd_ep_config(dev, EP_IN, USB_EPTYPE_INTERRUPT, EP_SIZE);
        usbd_ep_config(dev, EP_OUT, USB_EPTYPE_INTERRUPT, EP_SIZE);
        usbd_reg_endpoint(dev, EP_IN, pro_ep_callback);
        usbd_reg_endpoint(dev, EP_OUT, pro_ep_callback);
        usbd_ep_write(dev, EP_IN, NULL, 0);
        pro.connected = true;
        log_event(1);
        return usbd_ack;
    default:
        return usbd_fail;
    }
}

static usbd_respond pro_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(callback);

    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_CLASS) &&
       req->wIndex == 0) {
        switch(req->bRequest) {
        case USB_HID_SETIDLE:
        case USB_HID_SETPROTOCOL:
            return usbd_ack;
        default:
            return usbd_fail;
        }
    }

    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_STANDARD) &&
       req->wIndex == 0 && req->bRequest == USB_STD_GET_DESCRIPTOR) {
        switch(req->wValue >> 8) {
        case USB_DTYPE_HID:
            dev->status.data_ptr = (void*)&pro_cfg_desc.intf_0.hid;
            dev->status.data_count = sizeof(pro_cfg_desc.intf_0.hid);
            return usbd_ack;
        case USB_DTYPE_HID_REPORT:
            dev->status.data_ptr = (void*)pro_report_desc;
            dev->status.data_count = sizeof(pro_report_desc);
            return usbd_ack;
        default:
            return usbd_fail;
        }
    }
    return usbd_fail;
}

static void pro_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx) {
    UNUSED(intf);
    UNUSED(ctx);
    pro.dev = dev;
    pro.active = true;
    usbd_reg_config(dev, pro_ep_config);
    usbd_reg_control(dev, pro_control);
    usbd_connect(dev, true);
}

static void pro_deinit(usbd_device* dev) {
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);
    pro.connected = false;
    pro.handshake = false;
    pro.streaming = false;
    pro.active = false;
}

static void pro_wakeup(usbd_device* dev) {
    UNUSED(dev);
    pro.connected = true;
    log_event(2);
}

static void pro_suspend(usbd_device* dev) {
    UNUSED(dev);
    pro.connected = false;
    pro.handshake = false;
    pro.streaming = false;
    log_event(3);
    furi_semaphore_release(pro.tx_sem);
}

static FuriHalUsbInterface usb_pro = {
    .init = pro_init,
    .deinit = pro_deinit,
    .wakeup = pro_wakeup,
    .suspend = pro_suspend,
    .dev_descr = &pro_dev_desc,
    .str_manuf_descr = (void*)&str_manuf,
    .str_prod_descr = (void*)&str_prod,
    .str_serial_descr = (void*)&str_serial,
    .cfg_descr = &pro_cfg_desc,
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void pro_usb_start(void) {
    memset(&pro, 0, sizeof(pro));
    log_n = 0;
    build_spi_tables();

    pro.input_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    pro.tx_sem = furi_semaphore_alloc(1, 1);
    pro.rx_queue = furi_message_queue_alloc(8, sizeof(RxPacket));

    pro.run = true;
    pro.worker = furi_thread_alloc_ex("ProUsbWorker", 2048, pro_worker, NULL);
    furi_thread_start(pro.worker);

    pro.prev_if = furi_hal_usb_get_config();
    if(pro.prev_if == NULL) pro.prev_if = &usb_cdc_single;
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_pro, NULL) == true);
    FURI_LOG_I(TAG, "Pro Controller interface requested");
}

void pro_usb_stop(void) {
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(pro.prev_if, NULL) == true);

    /* The mode switch is asynchronous; wait for our deinit before freeing. */
    for(int i = 0; i < 40 && pro.active; i++) furi_delay_ms(50);

    log_dump();

    pro.run = false;
    furi_thread_join(pro.worker);
    furi_thread_free(pro.worker);

    furi_message_queue_free(pro.rx_queue);
    furi_semaphore_free(pro.tx_sem);
    furi_mutex_free(pro.input_mutex);
}

void pro_usb_set_input(uint32_t buttons, int8_t lx, int8_t ly, int8_t rx, int8_t ry) {
    furi_mutex_acquire(pro.input_mutex, FuriWaitForever);
    pro.buttons = buttons;
    pro.lx = lx;
    pro.ly = ly;
    pro.rx = rx;
    pro.ry = ry;
    furi_mutex_release(pro.input_mutex);
}

ProUsbStatus pro_usb_status(void) {
    if(!pro.connected) return ProUsbDisconnected;
    if(!pro.handshake) return ProUsbEnumerated;
    if(!pro.streaming) return ProUsbHandshake;
    return ProUsbActive;
}
