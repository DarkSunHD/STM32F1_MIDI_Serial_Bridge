/*
 * Copyright (C) 2017 Alexander Hold <darksunhd@gmail.com>
 * Copyright (C) 2016 Alexey Bolshakov <ua3mqj@gmail.com>
 * Copyright (C) 2016 Paul Fertser <fercerpav@gmail.com>
 * Copyright (C) 2014 Daniel Thompson <daniel@redfelineninja.org.uk>
 * Copyright (C) 2010 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2009 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2013 Stephen Dwyer <scdwyer@ualberta.ca>
 * Copyright (C) 2014 Laurent Barattero <laurentba@larueluberlu.net>
 *
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/audio.h>
#include <libopencm3/usb/midi.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/desig.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/pwr.h>
#include <libopencm3/stm32/spi.h>
#include <libopencm3/stm32/usart.h>
#include <libopencmsis/core_cm3.h>


static usbd_device *usbd_dev;

int rec = 0;

static int midicmd = 0;
static int midipar1 = 0;
static int midipar2 = 0;

/*
 * All references in this file come from Universal Serial Bus Device Class
 * Definition for MIDI Devices, release 1.0.
 */

/*
 * Table B-1: MIDI Adapter Device Descriptor
 */
static const struct usb_device_descriptor dev_descr = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,    /* was 0x0110 in Table B-1 example descriptor */
	.bDeviceClass = 0,   /* device defined at interface level */
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x6666,  /* Prototype product vendor ID */
	.idProduct = 0x5119, /* dd if=/dev/random bs=2 count=1 | hexdump */
	.bcdDevice = 0x0100,
	.iManufacturer = 1,  /* index to string desc */
	.iProduct = 2,       /* index to string desc */
	.iSerialNumber = 3,  /* index to string desc */
	.bNumConfigurations = 1,
};

/*
 * Midi specific endpoint descriptors.
 */
static const struct usb_midi_endpoint_descriptor midi_bulk_endp[] = {{
	/* Table B-12: MIDI Adapter Class-specific Bulk OUT Endpoint
	 * Descriptor
	 */
	.head = {
		.bLength = sizeof(struct usb_midi_endpoint_descriptor),
		.bDescriptorType = USB_AUDIO_DT_CS_ENDPOINT,
		.bDescriptorSubType = USB_MIDI_SUBTYPE_MS_GENERAL,
		.bNumEmbMIDIJack = 1,
	},
	.jack[0] = {
		.baAssocJackID = 0x01,
	},
}, {
	/* Table B-14: MIDI Adapter Class-specific Bulk IN Endpoint
	 * Descriptor
	 */
	.head = {
		.bLength = sizeof(struct usb_midi_endpoint_descriptor),
		.bDescriptorType = USB_AUDIO_DT_CS_ENDPOINT,
		.bDescriptorSubType = USB_MIDI_SUBTYPE_MS_GENERAL,
		.bNumEmbMIDIJack = 1,
	},
	.jack[0] = {
		.baAssocJackID = 0x03,
	},
} };

/*
 * Standard endpoint descriptors
 */
static const struct usb_endpoint_descriptor bulk_endp[] = {{
	/* Table B-11: MIDI Adapter Standard Bulk OUT Endpoint Descriptor */
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x01,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 0x40,
	.bInterval = 0x00,

	.extra = &midi_bulk_endp[0],
	.extralen = sizeof(midi_bulk_endp[0])
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x81,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 0x40,
	.bInterval = 0x00,

	.extra = &midi_bulk_endp[1],
	.extralen = sizeof(midi_bulk_endp[1])
} };

/*
 * Table B-4: MIDI Adapter Class-specific AC Interface Descriptor
 */
static const struct {
	struct usb_audio_header_descriptor_head header_head;
	struct usb_audio_header_descriptor_body header_body;
} __attribute__((packed)) audio_control_functional_descriptors = {
	.header_head = {
		.bLength = sizeof(struct usb_audio_header_descriptor_head) +
		           1 * sizeof(struct usb_audio_header_descriptor_body),
		.bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
		.bDescriptorSubtype = USB_AUDIO_TYPE_HEADER,
		.bcdADC = 0x0100,
		.wTotalLength =
			   sizeof(struct usb_audio_header_descriptor_head) +
			   1 * sizeof(struct usb_audio_header_descriptor_body),
		.binCollection = 1,
	},
	.header_body = {
		.baInterfaceNr = 0x01,
	},
};

/*
 * Table B-3: MIDI Adapter Standard AC Interface Descriptor
 */
static const struct usb_interface_descriptor audio_control_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_AUDIO_SUBCLASS_CONTROL,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.extra = &audio_control_functional_descriptors,
	.extralen = sizeof(audio_control_functional_descriptors)
} };

/*
 * Class-specific MIDI streaming interface descriptor
 */
static const struct {
	struct usb_midi_header_descriptor header;
	struct usb_midi_in_jack_descriptor in_embedded;
	struct usb_midi_in_jack_descriptor in_external;
	struct usb_midi_out_jack_descriptor out_embedded;
	struct usb_midi_out_jack_descriptor out_external;
} __attribute__((packed)) midi_streaming_functional_descriptors = {
	/* Table B-6: Midi Adapter Class-specific MS Interface Descriptor */
	.header = {
		.bLength = sizeof(struct usb_midi_header_descriptor),
		.bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
		.bDescriptorSubtype = USB_MIDI_SUBTYPE_MS_HEADER,
		.bcdMSC = 0x0100,
		.wTotalLength = sizeof(midi_streaming_functional_descriptors),
	},
	/* Table B-7: MIDI Adapter MIDI IN Jack Descriptor (Embedded) */
	.in_embedded = {
		.bLength = sizeof(struct usb_midi_in_jack_descriptor),
		.bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
		.bDescriptorSubtype = USB_MIDI_SUBTYPE_MIDI_IN_JACK,
		.bJackType = USB_MIDI_JACK_TYPE_EMBEDDED,
		.bJackID = 0x01,
		.iJack = 0x00,
	},
	/* Table B-8: MIDI Adapter MIDI IN Jack Descriptor (External) */
	.in_external = {
		.bLength = sizeof(struct usb_midi_in_jack_descriptor),
		.bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
		.bDescriptorSubtype = USB_MIDI_SUBTYPE_MIDI_IN_JACK,
		.bJackType = USB_MIDI_JACK_TYPE_EXTERNAL,
		.bJackID = 0x02,
		.iJack = 0x00,
	},
	/* Table B-9: MIDI Adapter MIDI OUT Jack Descriptor (Embedded) */
	.out_embedded = {
		.head = {
			.bLength = sizeof(struct usb_midi_out_jack_descriptor),
			.bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
			.bDescriptorSubtype = USB_MIDI_SUBTYPE_MIDI_OUT_JACK,
			.bJackType = USB_MIDI_JACK_TYPE_EMBEDDED,
			.bJackID = 0x03,
			.bNrInputPins = 1,
		},
		.source[0] = {
			.baSourceID = 0x02,
			.baSourcePin = 0x01,
		},
		.tail = {
			.iJack = 0x00,
		}
	},
	/* Table B-10: MIDI Adapter MIDI OUT Jack Descriptor (External) */
	.out_external = {
		.head = {
			.bLength = sizeof(struct usb_midi_out_jack_descriptor),
			.bDescriptorType = USB_AUDIO_DT_CS_INTERFACE,
			.bDescriptorSubtype = USB_MIDI_SUBTYPE_MIDI_OUT_JACK,
			.bJackType = USB_MIDI_JACK_TYPE_EXTERNAL,
			.bJackID = 0x04,
			.bNrInputPins = 1,
		},
		.source[0] = {
			.baSourceID = 0x01,
			.baSourcePin = 0x01,
		},
		.tail = {
			.iJack = 0x00,
		},
	},
};

/*
 * Table B-5: MIDI Adapter Standard MS Interface Descriptor
 */
static const struct usb_interface_descriptor midi_streaming_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_AUDIO_SUBCLASS_MIDISTREAMING,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = bulk_endp,

	.extra = &midi_streaming_functional_descriptors,
	.extralen = sizeof(midi_streaming_functional_descriptors)
} };

static const struct usb_interface ifaces[] = {{
	.num_altsetting = 1,
	.altsetting = audio_control_iface,
}, {
	.num_altsetting = 1,
	.altsetting = midi_streaming_iface,
} };

/*
 * Table B-2: MIDI Adapter Configuration Descriptor
 */
static const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0, /* can be anything, it is updated automatically
			      when the usb code prepares the descriptor */
	.bNumInterfaces = 2, /* control and data */
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0x80, /* bus powered */
	.bMaxPower = 0x32,

	.interface = ifaces,
};

static const char * usb_strings[] = {
	"Hold-Solutions.com",
	"Alex Hold | Serial -> MIDI",
	"AHSM00001\0"
};

/* Buffer to be used for control requests. */
uint8_t usbd_control_buffer[128];

/* SysEx identity message, preformatted with correct USB framing information */
const uint8_t sysex_identity[] = {
	0x04,	/* USB Framing (3 byte SysEx) */
	0xf0,	/* SysEx start */
	0x7e,	/* non-realtime */
	0x00,	/* Channel 0 */
	0x04,	/* USB Framing (3 byte SysEx) */
	0x7d,	/* Educational/prototype manufacturer ID */
	0x66,	/* Family code (byte 1) */
	0x66,	/* Family code (byte 2) */
	0x04,	/* USB Framing (3 byte SysEx) */
	0x51,	/* Model number (byte 1) */
	0x19,	/* Model number (byte 2) */
	0x00,	/* Version number (byte 1) */
	0x04,	/* USB Framing (3 byte SysEx) */
	0x00,	/* Version number (byte 2) */
	0x01,	/* Version number (byte 3) */
	0x00,	/* Version number (byte 4) */
	0x05,	/* USB Framing (1 byte SysEx) */
	0xf7,	/* SysEx end */
	0x00,	/* Padding */
	0x00,	/* Padding */
};

static bool midiusart = true;

static void usbmidi_data_rx_cb(usbd_device *dev, uint8_t ep)
{
	(void)ep;

	//static bool keys[128] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

	char buf[64];
	int len = usbd_ep_read_packet(dev, 0x01, buf, 64);

	/* This implementation treats any message from the host as a SysEx
	 * identity request. This works well enough providing the host
	 * packs the identify request in a single 8 byte USB message.
	 */
	/*if (len) {
		while (usbd_ep_write_packet(usbd_dev, 0x81, sysex_identity,
					    sizeof(sysex_identity)) == 0);
	}
	*/

	if (len) {

		usart_send_blocking(USART1, buf[1]);
		usart_send_blocking(USART1, buf[2]);
		usart_send_blocking(USART1, buf[3]);

		/*

		if(buf[0]==0x09) {
			//note on
			keys[(uint8_t)(buf[2])] = 1;
		} else if(buf[0]==0x08) {
			//note off
			keys[(uint8_t)(buf[2])] = 0;
		}
		
		bool note_on = false;
		for (unsigned int i = 0; i < 128; i++) {
			if(keys[i] == 1) {
				note_on = true;
			}
		}
		if(note_on) {
			if(midiusart){
			

			

			for(int ix = 0; ix < 2; ix++){
			gpio_clear(GPIOC, GPIO13); 
			
                       int a_as_int = (int)buf[ix+2];

  			unsigned char strx[16];

  			sprintf(strx, "%d", a_as_int);

			int length;
			length = sizeof(strx) / sizeof(int);


			for(int i = 0; i < length; i++){
				usart_send_blocking(USART1, strx[i]);   // USART1: Send byte. 
			}
                        
			

			usart_send_blocking(USART1, '\r');
                        usart_send_blocking(USART1, '\n');

			}

			usart_send_blocking(USART1, '\r');
                        usart_send_blocking(USART1, '\n');

                        //c = (c == 9) ? 0 : c + 1;       // Increment c. 
                        //if ((j++ % 80) == 0) {  // Newline after line full. 
                         //       usart_send_blocking(USART1, '\r');
                           //     usart_send_blocking(USART1, '\n');
                        //}

                        //for (i = 0; i < 800000; i++)    // Wait a bit. 
                           //     __asm__("nop");

				
				midiusart=false; 
			}

		} else {
			gpio_set(GPIOC, GPIO13);
			midiusart = true;
		}

		*/

	}

/*
	while(1){
			int c = 0, j = 0, i;
			    // LED on/off 
	                usart_send_blocking(USART1, c + '0');   // USART1: Send byte. 
	                c = (c == 9) ? 0 : c + 1;       // Increment c. 
        	        if ((j++ % 80) == 0) {  // Newline after line full. 
                        	usart_send_blocking(USART1, '\r');
                        	usart_send_blocking(USART1, '\n');
                	}
                	
			for (i = 0; i < 800000; i++)    // Wait a bit. 
                        	__asm__("nop");

	}
*/

}





static void usbmidi_set_config(usbd_device *dev, uint16_t wValue)
{
/*
	(void)wValue;

	usbd_ep_setup(dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64,
			usbmidi_data_rx_cb);
	usbd_ep_setup(dev, 0x81, USB_ENDPOINT_ATTR_BULK, 64, NULL);
*/
	
	(void)wValue;

	usbd_ep_setup(dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64,
			usbmidi_data_rx_cb);
	usbd_ep_setup(dev, 0x81, USB_ENDPOINT_ATTR_BULK, 64, NULL);

	//systick init
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
	// SysTick interrupt every N clock pulses: set reload to N-1
	systick_set_reload(99999);
	systick_interrupt_enable();
	systick_counter_enable();

}

static void button_send_event(usbd_device *dev, int pressed)
{

/*
	char buf[4] = { 0x08, // USB framing: virtual cable 0, note on 
			0x80, // MIDI command: note on, channel 1 
			60,   // Note 60 (middle C) 
			64,   // "Normal" velocity 
	};
*/



	char buf[4] = { 0x08, // USB framing: virtual cable 0, note on 
			midicmd,
			midipar1,
			midipar2
	};


	buf[0] |= pressed;
	//buf[1] |= pressed << 4;

	while (usbd_ep_write_packet(dev, 0x81, buf, sizeof(buf)) == 0);
}


/*
static void pot_send_event(uint8_t pot_num, uint8_t pot_value)
{
	static uint8_t last_sent_pot_value = 0;	
	static uint8_t pot_buf[16];
	static uint8_t pot_buf_ptr = 0;

	pot_buf[pot_buf_ptr] = pot_value;
	pot_buf_ptr = (pot_buf_ptr==15) ? 0 : pot_buf_ptr + 1;

	uint16_t avg_pot_value = 0;

	for (unsigned int i = 0; i < 16; i++) {
		avg_pot_value += pot_buf[i];
	}

	if(last_sent_pot_value != (avg_pot_value/16) ) {
		last_sent_pot_value = (avg_pot_value/16);

		char buf[4] = { 0x03, // USB framing: virtual cable 0, three byte System Common Message
				0b10110000, // MIDI command: Control Change, channel 0 
				16+pot_num,   // CC number 
				last_sent_pot_value,   // CC value 
		};

		while (usbd_ep_write_packet(usbd_dev, 0x81, buf, sizeof(buf)) == 0);
	}
}

*/





static void usart_setup(void)
{
        /*
 	//Setup GPIO pin GPIO_USART1_TX. 
        gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
                      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);

	gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT, GPIO_USART2_RX);

        // Setup UART parameters. 
	//usart_set_baudrate(USART1, 38400);
	usart_set_baudrate(USART1, 115200);
        usart_set_databits(USART1, 8);
        usart_set_stopbits(USART1, USART_STOPBITS_1);
       // usart_set_mode(USART1, USART_MODE_TX);
	usart_set_mode(USART2, USART_MODE_TX_RX);
        usart_set_parity(USART1, USART_PARITY_NONE);
        usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);

        // Finally enable the USART.
        usart_enable(USART1);
	*/

	nvic_enable_irq(NVIC_USART1_IRQ);

	/* Setup GPIO pin GPIO_USART1_RE_TX on GPIO port B for transmit. */
	gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);

	/* Setup GPIO pin GPIO_USART1_RE_RX on GPIO port B for receive. */
	gpio_set_mode(GPIOA, GPIO_MODE_INPUT,
		      GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);

	/* Setup UART parameters. */
	//usart_set_baudrate(USART1, 230400);
	usart_set_baudrate(USART1, 115200);
	usart_set_databits(USART1, 8);
	usart_set_stopbits(USART1, USART_STOPBITS_1);
	usart_set_parity(USART1, USART_PARITY_NONE);
	usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
	usart_set_mode(USART1, USART_MODE_TX_RX);

	/* Enable USART1 Receive interrupt. */
	USART_CR1(USART1) |= USART_CR1_RXNEIE;

	/* Finally enable the USART. */
	usart_enable(USART1);

	
}




void usart1_isr(void)
{
	static uint8_t data = 'A';

	/* Check if we were called because of RXNE. */
	if (((USART_CR1(USART1) & USART_CR1_RXNEIE) != 0) &&
	    ((USART_SR(USART1) & USART_SR_RXNE) != 0)) {

		/* Indicate that we got data. */
		


		/* Retrieve the data from the peripheral. */
		data = usart_recv(USART1);
		

		if(data > 127){
			rec = 1;
			midicmd = data;
		}else{
			if(rec == 2){
				midipar2 = data;
				rec = 0;
				gpio_toggle(GPIOC, GPIO13);
				button_send_event(usbd_dev, 1);
			}
			if(rec == 1){
				midipar1 = data;
				rec = 2;
			}

		}

		/* Enable transmit interrupt so it sends back the data. */
		USART_CR1(USART1) |= USART_CR1_TXEIE;
	}

	/* Check if we were called because of TXE. */
	if (((USART_CR1(USART1) & USART_CR1_TXEIE) != 0) &&
	    ((USART_SR(USART1) & USART_SR_TXE) != 0)) {

		/* Indicate that we are sending out data. */
		// gpio_toggle(GPIOA, GPIO7);

		/* Put data into the transmit register. */
		data = 'A';
		//usart_send(USART1, data);

		/* Disable the TXE interrupt as we don't need it anymore. */
		USART_CR1(USART1) &= ~USART_CR1_TXEIE;
	}
}



int main(void)
{
	//usbd_device *usbd_dev;

	rcc_clock_setup_in_hse_8mhz_out_72mhz();

	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_GPIOB);

	rcc_periph_clock_enable(RCC_USART1); //alex

	usart_setup();

	gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);

	gpio_set(GPIOC, GPIO13); // led off

	// parallel debug out
	gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ,
		      GPIO_CNF_OUTPUT_PUSHPULL, GPIO8|GPIO9|GPIO10|GPIO11|GPIO12|GPIO13|GPIO14|GPIO15);

	// button pin
	//gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, GPIO0);



	usbd_dev = usbd_init(&st_usbfs_v1_usb_driver, &dev_descr, &config,
			usb_strings, 3,
			usbd_control_buffer, sizeof(usbd_control_buffer));

	usbd_register_set_config_callback(usbd_dev, usbmidi_set_config);


	//adc_setup();
	//spi_setup();

	gpio_toggle(GPIOC, GPIO13); 
	while (1) {
		usbd_poll(usbd_dev);
		//button_poll(usbd_dev);	
/*
		 int c = 0, j = 0, i;


                        gpio_toggle(GPIOC, GPIO13);     // LED on/off 
                        usart_send_blocking(USART1, c + '0');   // USART1: Send byte. 
                        c = (c == 9) ? 0 : c + 1;       // Increment c. 
                        if ((j++ % 80) == 0) {  // Newline after line full. 
                                usart_send_blocking(USART1, '\r');
                                usart_send_blocking(USART1, '\n');
                        }

                        for (i = 0; i < 800000; i++)    // Wait a bit. 
                                __asm__("nop");
*/
	}
	
}
