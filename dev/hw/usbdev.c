#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <dev/registers.h>
#include <kernel/fault.h>

#include "usbdev_internals.h"
#include <dev/hw/usbdev.h>

static inline void usbdev_clocks_init(void);

static inline void usbdev_enable_receive(void);

static inline void usbdev_handle_usbrst(void);
static inline void usbdev_handle_srqint(void);
static inline void usbdev_handle_enumdne(void);
static inline void usbdev_handle_sof(void);
static inline void usbdev_handle_rxflvl(void);
static inline void usbdev_handle_iepint(void);
static inline void usbdev_handle_oepint(void);

static inline void usbdev_send_status_packet(void);

static inline void usbdev_handle_out_packet_received(uint32_t status);
static inline void usbdev_handle_setup_packet_received(uint32_t status);

static void parse_setup_packet(uint32_t *packet, uint32_t len);
static void handle_std_setup_packet(struct usbdev_setup_packet *setup);
static void handle_class_setup_packet(struct usbdev_setup_packet *setup);

static void usbdev_set_configuration(uint16_t configuration);

static void fill_fifo(int endpoint);

uint8_t setup_complete = 0;

struct fifo_buf {
    uint32_t *buf;
    int len;
};

struct fifo_buf fifo_buf[4] = {{NULL, 0}, {NULL, 0}, {NULL, 0}, {NULL, 0}};

#define USB_CDC_ACM_ENDPOINT    (1)
#define USB_CDC_ACM_MPSIZE      (64)

#define USB_CDC_TX_ENDPOINT     (2)
#define USB_CDC_TX_MPSIZE       (64)

#define USB_CDC_RX_ENDPOINT     (3)
#define USB_CDC_RX_MPSIZE       (64)

const struct usb_device_descriptor usb_device_descriptor = {
    .bLength =             sizeof(struct usb_device_descriptor),
    .bDescriptorType =     USB_SETUP_DESCRIPTOR_DEVICE,
    .bcdUSB =              USB_VERSION_1_1,
    .bDeviceClass =        USB_CLASS_CDC,
    .bDeviceSubClass =     0,
    .bDeviceProtocol =     0,
    .bMaxPacketSize0 =     64,
    .idVendor =            0x0483,     /* https://github.com/ericherman/stm32f4-discovery-example/blob/master/rotate-chars/rotate-chars-usb-descriptors.h */
    .idProduct =           0x5740,
    .bcdDevice =           1,
    .iManufacturer =       0,
    .iProduct =            0,
    .iSerialNumber =       0,
    .bNumConfigurations =  1
};

const struct __attribute__((packed)) usbdev_configuration1 {
    struct usb_configuration_descriptor                     config;
    struct usb_interface_descriptor                         interface0;
    struct usb_cdc_header_functional_descriptor             interface0_header;
    struct usb_cdc_call_management_functional_descriptor    interface0_call_man;
    struct usb_cdc_acm_functional_descriptor                interface0_acm;
    struct usb_cdc_acm_union_functional_descriptor          interface0_union;
    struct usb_endpoint_descriptor                          interface0_endpoint;
    struct usb_interface_descriptor                         interface1;
    struct usb_endpoint_descriptor                          interface1_endpoint1;
    struct usb_endpoint_descriptor                          interface1_endpoint2;
} usbdev_configuration1 = {
    {   /* Configuration Descriptor */
        /* WARNING DEFINED BELOW AS WELL*/
        .bLength =                       sizeof(struct usb_configuration_descriptor),
        .bDescriptorType =               USB_SETUP_DESCRIPTOR_CONFIG,
        .wTotalLength =                  sizeof(struct usbdev_configuration1),
        .bNumInterfaces =                2,
        .bConfigurationValue =           1,
        .iConfiguration =                0,
        .bmAttributes_reserved_0 =       0,
        .bmAttributes_remote_wakeup =    1,
        .bmAttributes_self_power =       1,
        .bmAttributes_reserved_1 =       1,
        .bMaxPower =                     0
    },
    {   /* Interface 0 Descriptor */
        .bLength =              sizeof(struct usb_interface_descriptor),
        .bDescriptorType =      USB_SETUP_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber =     0,
        .bAlternateSetting =    0,
        .bNumEndpoints =        1,
        .bInterfaceClass =      USB_CLASS_CDC,
        .bInterfaceSubClass =   USB_CDC_SUBCLASS_ACM,
        .bInterfaceProtocol =   USB_CDC_PROTOCOL_AT,
        .iInterface =           0
    },
    {   /* CDC Header Functional Descriptor */
        .bFunctionLength =      sizeof(struct usb_cdc_header_functional_descriptor), 
        .bDescriptorType =      USB_CDC_DESCRIPTOR_CS_INTERFACE,
        .bDescriptorSubtype =   USB_CDC_DESCRIPTOR_SUBTYPE_HEADER,
        .bcdCDC =               USB_CDC_VERSION_1_1
    },
    {   /* CDC Call Management Functional Descriptor */
        .bFunctionLength =                              sizeof(struct usb_cdc_call_management_functional_descriptor),
        .bDescriptorType =                              USB_CDC_DESCRIPTOR_CS_INTERFACE,
        .bDcescriptorSubtype =                          USB_CDC_DESCRIPTOR_SUBTYPE_CALL_MANAGEMENT,
        .bmCapabilities_handle_call_management =        1,
        .bmCapabilities_call_management_on_data_class = 0,
        .bmCapabilities_reserved_0 =                    0,
        .bDataInterface =                               1
    },
    {   /* CDC ACM Functional Descriptor */
        .bFunctionLength =                          sizeof(struct usb_cdc_acm_functional_descriptor),
        .bDescriptorType =                          USB_CDC_DESCRIPTOR_CS_INTERFACE,
        .bDcescriptorSubtype =                      USB_CDC_DESCRIPTOR_SUBTYPE_ACM,
        .bmCapabilities_support_comm_feature =      0,
        .bmCapabilities_support_line_coding =       1,
        .bmCapabilities_support_send_break =        1,
        .bmCapabilities_support_network_connect =   0,
        .bmCapabilities_reserved_0 =                0
    },
    {   /* CDC Union Functional Descriptor */
        .bFunctionLength =      sizeof(struct usb_cdc_acm_union_functional_descriptor),
        .bDescriptorType =      USB_CDC_DESCRIPTOR_CS_INTERFACE,
        .bDcescriptorSubtype =  USB_CDC_DESCRIPTOR_SUBTYPE_UNION,
        .bMasterInterface =     0,
        .bSlaveInterface0 =     1
    },
    {   /* Endpoint ACM Descriptor */
        .bLength =                      sizeof(struct usb_endpoint_descriptor),
        .bDescriptorType =              USB_SETUP_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress_number =      USB_CDC_ACM_ENDPOINT,
        .bEndpointAddress_reserved_0 =  0,
        .bEndpointAddress_direction =   USB_SETUP_ENDPOINT_DIR_IN,
        .bmAttributes_transfer_type =   USB_SETUP_ENDPOINT_TRANSFER_INTERRUPT,
        .bmAttributes_sync_type =       USB_SETUP_ENDPOINT_SYNC_NONE,
        .bmAttributes_usage_type =      0,
        .bmAttributes_reserved_0 =      0,
        .wMaxPacketSize =               USB_CDC_ACM_MPSIZE,
        .bInterval =                    64
    },
    {   /* Interface 1 Descriptor */
        .bLength =              sizeof(struct usb_interface_descriptor),
        .bDescriptorType =      USB_SETUP_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber =     1,
        .bAlternateSetting =    0,
        .bNumEndpoints =        2,
        .bInterfaceClass =      USB_CLASS_CDC_DATA,
        .bInterfaceSubClass =   0,
        .bInterfaceProtocol =   0,
        .iInterface =           0
    },
    {   /* Endpoint RX Descriptor */
        .bLength =                      sizeof(struct usb_endpoint_descriptor),
        .bDescriptorType =              USB_SETUP_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress_number =      USB_CDC_RX_ENDPOINT,
        .bEndpointAddress_reserved_0 =  0,
        .bEndpointAddress_direction =   USB_SETUP_ENDPOINT_DIR_OUT,
        .bmAttributes_transfer_type =   USB_SETUP_ENDPOINT_TRANSFER_BULK,
        .bmAttributes_sync_type =       USB_SETUP_ENDPOINT_SYNC_NONE,
        .bmAttributes_usage_type =      0,
        .bmAttributes_reserved_0 =      0,
        .wMaxPacketSize =               USB_CDC_RX_MPSIZE,
        .bInterval =                    0
    },
    {   /* Endpoint TX Descriptor */
        .bLength =                      sizeof(struct usb_endpoint_descriptor),
        .bDescriptorType =              USB_SETUP_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress_number =      USB_CDC_TX_ENDPOINT,
        .bEndpointAddress_reserved_0 =  0,
        .bEndpointAddress_direction =   USB_SETUP_ENDPOINT_DIR_IN,
        .bmAttributes_transfer_type =   USB_SETUP_ENDPOINT_TRANSFER_BULK,
        .bmAttributes_sync_type =       USB_SETUP_ENDPOINT_SYNC_NONE,
        .bmAttributes_usage_type =      0,
        .bmAttributes_reserved_0 =      0,
        .wMaxPacketSize =               USB_CDC_TX_MPSIZE,
        .bInterval =                    0
    }
};

const struct usb_configuration_descriptor usbdev_configuration1_descriptor = {
        /* WARNING DEFINED ABOVE AS WELL*/
        .bLength =                       sizeof(struct usb_configuration_descriptor),
        .bDescriptorType =               USB_SETUP_DESCRIPTOR_CONFIG,
        .wTotalLength =                  sizeof(struct usbdev_configuration1),
        .bNumInterfaces =                2,
        .bConfigurationValue =           1,
        .iConfiguration =                0,
        .bmAttributes_reserved_0 =       0,
        .bmAttributes_remote_wakeup =    1,
        .bmAttributes_self_power =       1,
        .bmAttributes_reserved_1 =       1,
        .bMaxPower =                     0
};

static inline void usbdev_clocks_init(void) {
    *RCC_AHB2ENR |= RCC_AHB2ENR_OTGFSEN;    /* Enable USB OTG FS clock */
    *RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN;    /* Enable GPIOA Clock */
    *NVIC_ISER2 |= (1 << 3);                /* Unable USB FS Interrupt */

    /* Set PA9, PA10, PA11, and PA12 to alternative function OTG
     * See stm32f4_ref.pdf pg 141 and stm32f407.pdf pg 51 */

    /* Sets PA8, PA9, PA10, PA11, and PA12 to alternative function mode */
    *GPIOA_MODER &= ~((3 << (8 * 2)) | (3 << (9 * 2)) | (3 << (10 * 2)) | (3 << (11 * 2)) | (3 << (12 * 2)));
    *GPIOA_MODER |= (GPIO_MODER_ALT << (8 * 2)) | (GPIO_MODER_ALT << (9 * 2)) | (GPIO_MODER_ALT << (10 * 2)) | (GPIO_MODER_ALT << (11 * 2)) | (GPIO_MODER_ALT << (12 * 2));

    /* Sets PA8, PA9, PA10, PA11, and PA12 to OTG mode */
    *GPIOA_AFRH  &= ~((0xF << ((8-8) * 4)) | (0xF << ((9-8) * 4)) | (0xF << ((10-8) * 4)) | (0xF << ((11-8) * 4)) | (0xF << ((12-8) * 4)));
    *GPIOA_AFRH  |= (GPIO_AF_OTG << ((8-8) * 4)) | (GPIO_AF_OTG << ((9-8) * 4)) | (GPIO_AF_OTG << ((10-8) * 4)) | (GPIO_AF_OTG << ((11-8) * 4)) | (GPIO_AF_OTG << ((12-8) * 4));

    /* Sets pin output to push/pull */
    *GPIOA_OTYPER &= ~((1 << 8) | (1 << 9) | (1 << 11) | (1 << 12));
    *GPIOA_OTYPER |= (1 << 10);

    /* No pull-up, no pull-down */
    *GPIOA_PUPDR &= ~((3 << (8 * 2)) | (3 << (9 * 2)) | (3 << (10 * 2)) | (3 << (11 * 2)) | (3 << (12 * 2)));
    *GPIOA_PUPDR |= (1 << (10 * 2));

    /* Speed to 100Mhz */
    *GPIOA_OSPEEDR &= ~((3 << (8 * 2)) | (3 << (9 * 2)) | (3 << (10 * 2)) | (3 << (11 * 2)) | (3 << (12 * 2)));
    *GPIOA_OSPEEDR |= (3 << (8 * 2)) | (3 << (9 * 2)) | (3 << (10 * 2)) | (3 << (11 * 2)) | (3 << (12 * 2));
}

void init_usbdev(void) {
    usbdev_clocks_init();

    /* Global unmask of USB interrupts, TX empty interrupt when TX is actually empty */
    *USB_FS_GAHBCFG |= USB_FS_GAHBCFG_GINTMSK;

    /* Enable session request protocol, USB turnaround time 15, force device mode */
    *USB_FS_GUSBCFG |= USB_FS_GUSBCFG_SRPCAP | USB_FS_GUSBCFG_TRDT(15) | USB_FS_GUSBCFG_FDMOD;

    /* Clear global interrupts */
    *USB_FS_GINTSTS = 0xFFFFFFFF;
    *USB_FS_GOTGINT = 0xFFFFFFFF;

    /* Unmask mode mismatch, OTG, USB reset, enum done, early susp, USB susp, and SOF interrupts */
    *USB_FS_GINTMSK |= USB_FS_GINTMSK_USBRSTM | USB_FS_GINTMSK_ENUMDNEM | USB_FS_GINTMSK_ESUSPM | USB_FS_GINTMSK_USBSUSPM | USB_FS_GINTMSK_SOFM;

    /* Set device to full speed (USB 1.1) */
    *USB_FS_DCFG |= USB_FS_DCFG_DSPD_FS | USB_FS_DCFG_NZLSOHSK;

    /* Enable VBUS B sensing */
    *USB_FS_GCCFG |= USB_FS_GCCFG_VBUSBSEN | USB_FS_GCCFG_PWRDWN;
}

static inline void usbdev_enable_receive(void) {
    *USB_FS_DOEPTSIZ0 = USB_FS_DOEPTSIZ0_XFRSIZ(64) | USB_FS_DOEPTSIZ0_PKTCNT(1) | USB_FS_DOEPTSIZ0_STUPCNT(3);
    *USB_FS_DOEPCTL0 |= USB_FS_DOEPCTL0_CNAK | USB_FS_DOEPCTL0_EPENA;

    if (setup_complete) {
        *USB_FS_DOEPTSIZ(USB_CDC_RX_ENDPOINT) = USB_FS_DOEPTSIZx_XFRSIZ(USB_CDC_RX_MPSIZE) | USB_FS_DOEPTSIZx_PKTCNT(1);
        *USB_FS_DOEPCTL(USB_CDC_RX_ENDPOINT) |= USB_FS_DOEPCTLx_CNAK | USB_FS_DOEPCTLx_EPENA;
    }
}

void usbdev_rx(uint32_t *buf, int words) {
    while (words > 0) {
        *buf = *USB_FS_DFIFO_EP(0);
        buf++;
        words--;
    }
}

/* packet points to first word in packet.  size is packet size in bytes */
int usbdev_tx(uint8_t endpoint, uint32_t *packet, int size) {
    uint8_t packets = size % 64 ? size/64 + 1 : size/64;
    if (!packets) {
        packets = 1;
    }

    if (endpoint == 0) {
        *USB_FS_DIEPTSIZ0 = USB_FS_DIEPTSIZ0_PKTCNT(packets) | USB_FS_DIEPTSIZ0_XFRSIZ(size);
        *USB_FS_DIEPCTL0 |= USB_FS_DIEPCTL0_CNAK | USB_FS_DIEPCTL0_EPENA;
    }
    else {
        *USB_FS_DIEPTSIZ(endpoint) = USB_FS_DIEPTSIZx_PKTCNT(packets) | USB_FS_DIEPTSIZx_XFRSIZ(size);
        *USB_FS_DIEPCTL(endpoint) |= USB_FS_DIEPCTLx_CNAK | USB_FS_DIEPCTLx_EPENA;
    }

    fifo_buf[endpoint].buf = packet;
    fifo_buf[endpoint].len = size;

    /* Enable TX FIFO empty interrupt */
    *USB_FS_DIEPEMPMSK |= (1 << endpoint);

    //printk("Sending data: ");

    //while (size > 0) {
    //    while (*USB_FS_DTXFSTS(endpoint) < size/packets);

    //    printk("0x%x ", *packet);
    //    *USB_FS_DFIFO_EP(endpoint) = *packet;
    //    packet++;
    //    size -= 4;
    //}

    //printk("Done writing ");

    return 0;
}

/* USB OTG FS Global Interrupt Handler */
void usbdev_handler(void) {
    uint32_t interrupts = *USB_FS_GINTSTS;
    uint8_t handled = 0;
    
    // Clear interrupts
    *USB_FS_GINTSTS = 0xFFFFFFFF;

    //printk("\r\nInterrupt = 0x%x\r\n", interrupts);
    //*LED_ODR ^= (1 << 12);
    
    /* USB Reset */
    if (interrupts & USB_FS_GINTSTS_USBRST) {
        handled = 1;
        printk("USB: Reset\r\n");
        usbdev_handle_usbrst();
    }
    interrupts &= ~USB_FS_GINTSTS_USBRST;

    if (interrupts & USB_FS_GINTSTS_SRQINT) {
        handled = 1;
        printk("USB: New session detected\r\n");
        usbdev_handle_srqint();
    }
    interrupts &= ~USB_FS_GINTSTS_SRQINT;

    /* Enumeration done */
    if (interrupts & USB_FS_GINTSTS_ENUMDNE) {
        handled = 1;
        printk("USB: Enumeration done\r\n");
        usbdev_handle_enumdne();
    }
    interrupts &= ~USB_FS_GINTSTS_ENUMDNE;

    /* Start of frame token received */
    if (interrupts & USB_FS_GINTSTS_SOF) {
        handled = 1;
        //printk("Start of frame token received.\r\n");
        usbdev_handle_sof();
    }
    interrupts &= ~USB_FS_GINTSTS_SOF;

    if (interrupts & USB_FS_GINTSTS_IEPINT) {
        handled = 1;
        printk("USB: IN endpoint interrupt. ");
        usbdev_handle_iepint();
    }
    interrupts &= ~USB_FS_GINTSTS_IEPINT;

    if (interrupts & USB_FS_GINTSTS_OEPINT) {
        handled = 1;
        printk("USB: OUT endpoint interrupt. ");
        usbdev_handle_oepint();
    }
    interrupts &= ~USB_FS_GINTSTS_OEPINT;

    if (interrupts & USB_FS_GINTSTS_EOPF) {
        handled = 1;
        //printk("End of periodic frame\r\n");
    }
    interrupts &= ~USB_FS_GINTSTS_EOPF;

    if (interrupts & USB_FS_GINTSTS_ESUSP) {
        handled = 1;
        printk("USB: Early suspend.\r\n");
    }
    interrupts &= ~USB_FS_GINTSTS_ESUSP;

    if (interrupts & USB_FS_GINTSTS_USBSUSP) {
        handled = 1;
        printk("USB: USB suspend.\r\n");
    }
    interrupts &= ~USB_FS_GINTSTS_USBSUSP;

    if (interrupts & USB_FS_GINTSTS_OTGINT) {
        handled = 1;
        printk("USB: OTG Interrupt: 0x%x.\r\n", *USB_FS_GOTGINT);
        *USB_FS_GOTGINT = 0xFFFFFFFF;
    }
    interrupts &= ~USB_FS_GINTSTS_OTGINT;

    if (interrupts & USB_FS_GINTSTS_RXFLVL) {
        handled = 1;
        printk("USB: Received packet: ");
        usbdev_handle_rxflvl();
        usbdev_enable_receive();
    }
    interrupts &= ~USB_FS_GINTSTS_RXFLVL;

    if (!handled) {
        printk("USB: Unhandled interrupt: 0x%x \r\n", interrupts);
    }
}

#define RX_FIFO_SIZE    128
#define TX0_FIFO_SIZE   32
#define TX1_FIFO_SIZE   16
#define TX2_FIFO_SIZE   128
#define TX3_FIFO_SIZE   16

static inline void usbdev_handle_usbrst(void) {
    // Clear interrupt
    *USB_FS_GINTSTS = USB_FS_GINTSTS_USBRST;

    /* NAK bits */
    *USB_FS_DOEPCTL0 |= USB_FS_DOEPCTL0_SNAK;
    *USB_FS_DOEPCTL(1) |= USB_FS_DOEPCTLx_SNAK;
    *USB_FS_DOEPCTL(2) |= USB_FS_DOEPCTLx_SNAK;
    *USB_FS_DOEPCTL(3) |= USB_FS_DOEPCTLx_SNAK;

    /* Unmask interrupts */
    *USB_FS_DAINTMSK |= USB_FS_DAINT_IEPM(0) | USB_FS_DAINT_OEPM(0);
    *USB_FS_DOEPMSK |= USB_FS_DOEPMSK_STUPM | USB_FS_DOEPMSK_XFRCM;

    /* Back-to-back SETUP packets */
    *USB_FS_DOEPTSIZ0 |= USB_FS_DOEPTSIZ0_STUPCNT(3);

    /* Set RX FIFO size */
    *USB_FS_GRXFSIZ = RX_FIFO_SIZE;

    /* EP0 TX Size */
    *USB_FS_DIEPTXF0 = USB_FS_DIEPTXF0_TX0FSA(RX_FIFO_SIZE) | USB_FS_DIEPTXF0_TX0FD(TX0_FIFO_SIZE);
    /* EP1 TX Size */
    *USB_FS_DIEPTXF1 = USB_FS_DIEPTXF_INEPTXSA((RX_FIFO_SIZE+TX0_FIFO_SIZE)) | USB_FS_DIEPTXF_INEPTXFD(TX1_FIFO_SIZE);
    /* EP2 TX Size */
    *USB_FS_DIEPTXF2 = USB_FS_DIEPTXF_INEPTXSA((RX_FIFO_SIZE+TX0_FIFO_SIZE+TX1_FIFO_SIZE)) | USB_FS_DIEPTXF_INEPTXFD(TX2_FIFO_SIZE);
    /* EP3 TX Size */
    *USB_FS_DIEPTXF3 = USB_FS_DIEPTXF_INEPTXSA((RX_FIFO_SIZE+TX0_FIFO_SIZE+TX1_FIFO_SIZE+TX2_FIFO_SIZE)) | USB_FS_DIEPTXF_INEPTXFD(TX3_FIFO_SIZE);
}

static inline void usbdev_handle_srqint(void) {
    // Do nothing, clear interrupt
    *USB_FS_GINTSTS = USB_FS_GINTSTS_SRQINT;
}

static inline void usbdev_handle_enumdne(void) {
    uint32_t device_status = *USB_FS_DSTS;

    if ((device_status & USB_FS_DSTS_ENUMSPD) != USB_FS_DSTS_ENUMSPD_FS) {
        panic_print("USB FS enumerated a speed other than FS.");
    }

    /* Set maximum packet size */
    *USB_FS_DIEPCTL0 &= ~(USB_FS_DIEPCTL0_MPSIZE);
    *USB_FS_DIEPCTL0 |= USB_FS_DIEPCTL0_MPSIZE_64;
    *USB_FS_DOEPCTL0 &= ~(USB_FS_DOEPCTL0_MPSIZE);
    *USB_FS_DOEPCTL0 |= USB_FS_DOEPCTL0_MPSIZE_64;

    /* Clear interrupt */
    *USB_FS_GINTSTS = USB_FS_GINTSTS_ENUMDNE;
}

static inline void usbdev_handle_sof(void) {
    /* Clear interrupt */
    *USB_FS_GINTSTS = USB_FS_GINTSTS_SOF;
}

static inline void usbdev_handle_rxflvl(void) {
    uint32_t receive_status = *USB_FS_GRXSTSP;
    uint32_t packet_status = USB_FS_GRXSTS_PKTSTS(receive_status);

    switch (packet_status) {
        case USB_FS_GRXSTS_PKTSTS_NAK:
            printk("Global NAK: ");
            break;
        case USB_FS_GRXSTS_PKTSTS_ORX:
            printk("OUT received: ");
            usbdev_handle_out_packet_received(receive_status);
            break;
        case USB_FS_GRXSTS_PKTSTS_OCP:
            printk("OUT complete ");
            break;
        case USB_FS_GRXSTS_PKTSTS_STUPCP:
            printk("SETUP complete ");
            uint32_t buffer;
            usbdev_rx(&buffer, sizeof(buffer)/4);
            break;
        case USB_FS_GRXSTS_PKTSTS_STUPRX:
            printk("SETUP received: ");
            usbdev_handle_setup_packet_received(receive_status);
            break;
        default:
            printk("Unknown packet, receive status: 0x%x ", receive_status);
    }

    printk("\r\n");
}

//uint32_t crap[] = {0x30303030,0x31313131,0x32323232,0x33333333,0x34343434,0x35353535,0x36363636,0x37373737,0x38383838,0x39393939,0x3a3a3a3a,0x3b3b3b3b,0x3c3c3c3c,0x3d3d3d3d,0x3f3f3f3f,0x40404040};
//uint32_t crap[] = {0x494E4550, 0x00002053};
//uint32_t crap[] = {0x30303030};
char crap[] = {'H','e','l','l','o',' ','W','o','r','l','d',' '};

static inline void usbdev_handle_out_packet_received(uint32_t status) {
    uint32_t word_count = USB_FS_GRXSTS_BCNT(status) % 4 ? USB_FS_GRXSTS_BCNT(status)/4 + 1 : USB_FS_GRXSTS_BCNT(status)/4;

    if (word_count == 0) {
        return;
    }

    uint32_t buf[word_count];

    usbdev_rx(buf, word_count);

    for (int i = 0; i < word_count; i++) {
        printk("0x%x ", buf[i]);
    }

    if (USB_FS_GRXSTS_EPNUM(status) == USB_CDC_RX_ENDPOINT && USB_FS_GRXSTS_BCNT(status) == 1) {
        printk("\r\n\r\nReceived: '%c'\r\n", (char) (buf[0]));
        usbdev_tx(USB_CDC_TX_ENDPOINT, (uint32_t *) crap, sizeof(crap));
    }
}

uint32_t setup_packet[2];

static inline void usbdev_handle_setup_packet_received(uint32_t status) {
    uint32_t word_count = USB_FS_GRXSTS_BCNT(status)/4;

    if (word_count == 0) {
        return;
    }

    //uint32_t buf[word_count];

    //usbdev_rx(buf, word_count);
    usbdev_rx(setup_packet, 2);

    for (int i = 0; i < word_count; i++) {
        //printk("0x%x ", buf[i]);
        printk("0x%x ", setup_packet[i]);
    }

    struct usbdev_setup_packet *pack = (struct usbdev_setup_packet *) setup_packet;
    if (!pack->direction && pack->length) {
        printk("Data to receive. ");
    }

    //parse_setup_packet(buf, word_count);
}

static inline void usbdev_send_status_packet(void) {
    *USB_FS_DIEPTSIZ0 = USB_FS_DIEPTSIZ0_PKTCNT(1) | USB_FS_DIEPTSIZ0_XFRSIZ(0);
    *USB_FS_DIEPCTL0 |= USB_FS_DIEPCTL0_CNAK | USB_FS_DIEPCTL0_EPENA;

    *USB_FS_DOEPTSIZ0 = USB_FS_DOEPTSIZ0_STUPCNT(3) | USB_FS_DOEPTSIZ0_PKTCNT(1) | USB_FS_DOEPTSIZ0_XFRSIZ(8*3);
    //*USB_FS_DOEPCTL0 |= USB_FS_DOEPCTL0_CNAK | USB_FS_DOEPCTL0_EPENA;
}

static void parse_setup_packet(uint32_t *packet, uint32_t len) {
    struct usbdev_setup_packet *setup = (struct usbdev_setup_packet *) packet;

    switch (setup->type) {
        case USB_SETUP_REQUEST_TYPE_TYPE_STD:
            handle_std_setup_packet(setup);
            break;
        case USB_SETUP_REQUEST_TYPE_TYPE_CLASS:
            handle_class_setup_packet(setup);
            break;
        default:
            printk("Unhandled SETUP packet, type %d. ", setup->type);
    }
}

static void handle_std_setup_packet(struct usbdev_setup_packet *setup) {
    switch (setup->request) {
    case USB_SETUP_REQUEST_GET_DESCRIPTOR:
        printk("GET_DESCRIPTOR ");
        switch (setup->value >> 8) {
        case USB_SETUP_DESCRIPTOR_DEVICE:
            printk("DEVICE ");
            usbdev_tx(0, (uint32_t *) &usb_device_descriptor, sizeof(struct usb_device_descriptor));
            break;
        case USB_SETUP_DESCRIPTOR_CONFIG:
            printk("CONFIGURATION ");
            if (setup->length <= sizeof(usbdev_configuration1_descriptor)) {
                usbdev_tx(0, (uint32_t *) &usbdev_configuration1_descriptor, sizeof(usbdev_configuration1_descriptor));
            }
            else {
                usbdev_tx(0, (uint32_t *) &usbdev_configuration1, sizeof(usbdev_configuration1));
            }
            break;
        default:
            printk("OTHER DESCRIPTOR %d ", setup->value >> 8);
        }
        break;
    case USB_SETUP_REQUEST_SET_ADDRESS:
        printk("SET_ADDRESS %d ", setup->value);
        *USB_FS_DCFG |= USB_FS_DCFG_DAD(setup->value);
        usbdev_send_status_packet();
        break;
    case USB_SETUP_REQUEST_SET_CONFIGURATION:
        printk("SET_CONFIGURATION %d ", setup->value);
        usbdev_set_configuration(setup->value);
        usbdev_send_status_packet();
        break;
    case USB_SETUP_REQUEST_GET_STATUS:
        printk("GET_STATUS ");
        if (setup->recipient == USB_SETUP_REQUEST_TYPE_RECIPIENT_DEVICE) {
            printk("DEVICE ");
            uint32_t buf = 0x11; /* Self powered and remote wakeup */
            usbdev_tx(0, &buf, sizeof(buf));
        }
        else {
            printk("OTHER ");
        }
        break;
    default:
        printk("STD: OTHER_REQUEST %d ", setup->request);
    }
}

static void handle_class_setup_packet(struct usbdev_setup_packet *setup) {
    switch (setup->request) {
    case USB_SETUP_REQUEST_CDC_SET_CONTROL_LINE_STATE:
        printk("CDC: SET_CONTROL_LINE_STATE ");
        if (setup->value & USB_SETUP_REQUEST_CDC_SET_CONTROL_LINE_STATE_DTE) {
            printk("warning: not handling DTE. ");
        }
        if (setup->value & USB_SETUP_REQUEST_CDC_SET_CONTROL_LINE_STATE_CARRIER) {
            printk("warning: not handling carrier. ");
        }
        usbdev_send_status_packet();
        break;
    case USB_SETUP_REQUEST_CDC_SET_LINE_CODING:
        printk("CDC: SET_LINE_CODING ");
        usbdev_send_status_packet();
        break;
    default:
        printk("CDC: OTHER_REQUEST %d ", setup->request);
    }
}

static inline void usbdev_handle_iepint(void) {
    for (int i = 0; i <= 3; i++) {
        if (*USB_FS_DAINT & USB_FS_DAINT_IEPINT(i)) {
            printk("Endpoint %d. ", i);
        }
        else {
            continue;
        }

        uint32_t interrupts = *USB_FS_DIEPINT(i);

        if (interrupts & USB_FS_DIEPINTx_XFRC) {
            *USB_FS_DIEPINT(i) = USB_FS_DIEPINTx_XFRC;
            printk("Transfer complete. ");
        }

        if (interrupts & USB_FS_DIEPINTx_EPDISD) {
            *USB_FS_DIEPINT(i) = USB_FS_DIEPINTx_EPDISD;
            printk("Endpoint disabled. ");
        }

        if (interrupts & USB_FS_DIEPINTx_TOC) {
            *USB_FS_DIEPINT(i) = USB_FS_DIEPINTx_TOC;
            printk("Timeout condition. ");
        }

        if (interrupts & USB_FS_DIEPINTx_ITTXFE) {
            *USB_FS_DIEPINT(i) = USB_FS_DIEPINTx_ITTXFE;
            printk("IN token received when TX FIFO empty. ");
        }

        if (interrupts & USB_FS_DIEPINTx_INEPNE) {
            *USB_FS_DIEPINT(i) = USB_FS_DIEPINTx_INEPNE;
            printk("IN endpoint NAK effective. ");
        }

        if (interrupts & USB_FS_DIEPINTx_TXFE) {
            *USB_FS_DIEPINT(i) = USB_FS_DIEPINTx_TXFE;
            printk("Transmit FIFO empty. ");
            fill_fifo(i);
        }
    }

    printk("\r\n");
}

static void fill_fifo(int endpoint) {
    if (!fifo_buf[endpoint].buf) {
        return;
    }

    int min = fifo_buf[endpoint].len - 64;
    min = min >= 0 ? min : 0;

    printk("Writing FIFO %d: ", endpoint);

    while (fifo_buf[endpoint].len > min) {
        printk("0x%x ", *fifo_buf[endpoint].buf);
        *USB_FS_DFIFO_EP(endpoint) = *fifo_buf[endpoint].buf;
        fifo_buf[endpoint].buf++;
        fifo_buf[endpoint].len -= 4;
    }

    if (fifo_buf[endpoint].len <= 0) {
        fifo_buf[endpoint].buf = NULL;
        *USB_FS_DIEPEMPMSK &= ~(1 << endpoint);

        if (fifo_buf[endpoint].len < 0) {
            printk("warning: wrote past end of buffer. ");
        }
    }
}

static inline void usbdev_handle_oepint(void) {
    for (int i = 0; i <= 3; i++) {
        if (*USB_FS_DAINT & USB_FS_DAINT_OEPINT(i)) {
            printk("Endpoint %d. ", i);
        }
        else {
            continue;
        }

        uint32_t interrupts = *USB_FS_DOEPINT(i);

        if (interrupts & USB_FS_DOEPINTx_XFRC) {
            *USB_FS_DOEPINT(i) = USB_FS_DOEPINTx_XFRC;
            printk("Transfer complete. ");
        }

        if (interrupts & USB_FS_DOEPINTx_EPDISD) {
            *USB_FS_DOEPINT(i) = USB_FS_DOEPINTx_EPDISD;
            printk("Endpoint disabled. ");
        }

        if (interrupts & USB_FS_DOEPINTx_STUP) {
            *USB_FS_DOEPINT(i) = USB_FS_DOEPINTx_STUP;
            printk("SETUP phase done. ");
            parse_setup_packet(setup_packet, 2);
        }

        if (interrupts & USB_FS_DOEPINTx_OTEPDIS) {
            *USB_FS_DOEPINT(i) = USB_FS_DOEPINTx_OTEPDIS;
            printk("OUT token received when endpoint disabled. ");
        }

        if (interrupts & USB_FS_DOEPINTx_B2BSTUP) {
            *USB_FS_DOEPINT(i) = USB_FS_DOEPINTx_B2BSTUP;
            printk("Back-to-back SETUP packets received.");
        }
    }

    printk("\r\n");

    /* Clear interrupt */
    *USB_FS_GINTSTS = USB_FS_GINTSTS_OEPINT;
}

static void usbdev_set_configuration(uint16_t configuration) {
    if (configuration != 1) {
        printk("Cannot set configuration %u. ", configuration);
    }

    printk("Setting configuration %u. ", configuration);

    /* ACM Endpoint */
    *USB_FS_DIEPCTL(USB_CDC_ACM_ENDPOINT) |= USB_FS_DIEPCTLx_MPSIZE(USB_CDC_ACM_MPSIZE) | USB_FS_DIEPCTLx_EPTYP_INT | USB_FS_DIEPCTLx_TXFNUM(USB_CDC_ACM_ENDPOINT) | USB_FS_DIEPCTLx_USBAEP;

    /* RX Endpoint */
    *USB_FS_DOEPCTL(USB_CDC_RX_ENDPOINT) |= USB_FS_DOEPCTLx_MPSIZE(USB_CDC_RX_MPSIZE) | USB_FS_DOEPCTLx_EPTYP_BLK | USB_FS_DOEPCTLx_SD0PID | USB_FS_DOEPCTLx_EPENA | USB_FS_DOEPCTLx_USBAEP;

    /* TX Endpoint */
    *USB_FS_DIEPCTL(USB_CDC_TX_ENDPOINT) |= USB_FS_DIEPCTLx_MPSIZE(USB_CDC_TX_MPSIZE) | USB_FS_DIEPCTLx_EPTYP_BLK | USB_FS_DIEPCTLx_SD0PID | USB_FS_DIEPCTLx_TXFNUM(USB_CDC_TX_ENDPOINT) | USB_FS_DIEPCTLx_USBAEP;

    /* Flush TX FIFOs */
    *USB_FS_GRSTCTL |= USB_FS_GRSTCTL_TXFNUM(USB_CDC_TX_ENDPOINT) | USB_FS_GRSTCTL_TXFNUM(USB_CDC_ACM_ENDPOINT) | USB_FS_GRSTCTL_TXFFLSH;
    while (*USB_FS_GRSTCTL & USB_FS_GRSTCTL_TXFFLSH);

    /* Unmask interrupts */
    *USB_FS_DAINTMSK |= USB_FS_DAINT_IEPM(USB_CDC_ACM_ENDPOINT) | USB_FS_DAINT_IEPM(USB_CDC_TX_ENDPOINT) | USB_FS_DAINT_OEPM(USB_CDC_RX_ENDPOINT);
    *USB_FS_DIEPMSK |= USB_FS_DIEPMSK_XFRCM;

    setup_complete = 1;
    usbdev_enable_receive();
}
