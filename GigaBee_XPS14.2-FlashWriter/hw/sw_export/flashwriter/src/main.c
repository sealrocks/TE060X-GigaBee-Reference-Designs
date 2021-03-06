/*
 * main.c
 *
 *  Created on: Dec 20, 2011
 *      Author: Ziga Lenarcic
 */


#include "xparameters.h"

#include <stdio.h>
#include <malloc.h>

#include "xil_cache.h"
#include "xio.h"

#include "xil_testmem.h" // for Xil_TestMem32 in selftest


#include "misc_util.h"
#include "timing_util.h"

#include "eth_packet_util.h"
#include "eth_util.h"

#include "eeprom_util.h"
#include "flash_util.h"

#include "xgpio_l.h"



/*
	AXI4-Stream Status Word 2:

	Bits 5-3 : Specifies the receive full checksum status.
	This is relevant only for full checksum offloading.
	000 -> Neither the IP header nor the TCP/UDP checksums were checked.
	001 -> The IP header checksum was checked and was correct. The TCP/UDP checksum was not checked.
	010 -> Both the IP header checksum and the TCP checksum were checked and were correct.
	011 -> Both the IP header checksum and the UDP checksum were checked and were correct.
	100 -> Reserved.
	101 -> The IP header checksum was checked and was incorrect. The TCP/UDP checksum was not checked.
	110 -> The IP header checksum was checked and is correct but the TCP checksum was checked and was incorrect.
	111 -> The IP header checksum was checked and is correct but the UDP checksum was checked and was incorrect.
	*/
#define IP_OFF_TCPUDP_OFF			0x00000000
#define IP_OK_TCPUDP_OFF			0x00000001
#define IP_OK_TCP_OK			0x00000002
#define IP_OK_UDP_OK			0x00000003

#define IP_NOTOK_TCPUDP_OFF		0x00000005
#define IP_OK_TCP_NOTOK				0x00000006
#define IP_OK_UDP_NOTOK				0x00000007


#define FULL_CSUM_STATUS_MASK	0x00000038 /* Mask to extract full checksum
					    * status field from AXI4 Stream
					    * Status Word 2.
					    */

#define FULL_CSUM_VALIDATED	0x00000002 /* If bits 3-5 in AXI4 Status word
					    * have a value of 0x010, it means
					    * both IP and TCP checksums have
					    * been found to be correct.
					    */


//#include "flash_util.h"

#define RAM_ADDR XPAR_MCB_DDR3_S0_AXI_BASEADDR
#define RAM_SIZE 0x08000000 // 134217728 bytes (128 MB)
#define RAM_RX_SPACE (RAM_ADDR + 0x06000000)
#define RAM_TX_SPACE (RAM_ADDR + 0x05000000)
#define RAM_FRAME_SIZE 9216 // (9 * 1024) // so much storage for 1 packet

/*
 * This structure holds an ethernet frame in RAM, basically it's some
 * information + byte array of received/ bytes to send.
 */
typedef struct __attribute__ ((__packed__)) {
	u32 data_length;
	u32 status;
	unsigned char reserved[56];
	unsigned char data[0]; // should at 64 bytes offset (aligned)
} frame_t;

#define FLASH_IMAGE_RAM_ADDR (RAM_ADDR + 0x07000000) // 16 MB
#define FLASH_IMAGE_SIZE (8192 * 1024)

#define PONG_DATA_LEN 14 //(6 + 4 + 4) mac + IP + test result
#define SRC_PORT 5099

enum {
	GFW_OP_PING_CMD = 1,
	GFW_OP_PING_ACK,
	GFW_OP_FLASH_DATA_TRANSFER_CMD,
	GFW_OP_FLASH_DATA_TRANSFER_ACK,

	GFW_OP_FLASH_DATA_RESEND_CMD,

	GFW_OP_FLASH_WRITE_CMD,
	GFW_OP_FLASH_WRITE_ACK

};

enum {
    SELF_TEST_ALL_OK = 0,
    SELF_TEST_RAM = 1,
    SELF_TEST_FLASH = 2,
    SELF_TEST_EEPROM = 4,
    SELF_TEST_ETHERNET = 8,
    SELF_TEST_OTHER = 16
};

#define MAGIC1 'G'
#define MAGIC2 'F'
#define MAGIC3 'W'

typedef struct __attribute__ ((__packed__)) {
	u8 magic1; // G
	u8 magic2; // F
	u8 magic3; // W
	u8 op; // operation
	u32 id; // id / sequence number
	u32 data_length;
	u32 param1;

	u32 address_offset;
	u32 number_of_packets;

	unsigned char data[0];
} gfw_datagram_t;

#define DST_PORT 0x8813 // 0x1388 = 5000 swaped bytes (endianicity)

u32 self_test_result;

unsigned char myMAC[6] = { 0x00, 0x0A, 0x35, 0x01, 0x02, 0x03 };
u32 myIP = QUAD_TO_IP(169,254,169,99);
//u32 myIP = 0x0;

// 42 bytes arp message
unsigned char *gratious_arp_packet;
/*
 * returns 0 if packet should be ignored
 * returns 1 for ARP packet
 * returns 2 for UDP packet
 */
enum {
	PACKET_TYPE_IGNORE = 0,
	PACKET_TYPE_ARP = 1,
	PACKET_TYPE_UDP = 2
};

int eth_arp_form_reply(arp_packet_t *arp_in, arp_packet_t *arp_out, const unsigned char *mymac, u32 myip)
{
	if (arp_in->target_ip != myip) return 0;

	memcpy(arp_out->ether_hdr.dst_mac, arp_in->sender_mac, 6);
	memcpy(arp_out->ether_hdr.src_mac, mymac, 6);
	arp_out->ether_hdr.ethertype = Xil_Htons(0x0806);

	arp_out->hw_type = Xil_Htons(0x0001);
	arp_out->protocol_type = Xil_Htons(0x0800);
	arp_out->hw_size = 0x06;
	arp_out->protocol_size = 0x04;
	arp_out->opcode = Xil_Htons(0x0002); // reply

	memcpy(arp_out->sender_mac, mymac, 6);
	arp_out->sender_ip = myip;
	memcpy(arp_out->target_mac, arp_in->sender_mac, 6);
	arp_out->target_ip = arp_in->sender_ip;

	return 42; // 42 bytes sizeof(arp_packet_t)
}


int net_get_link_speed(XAxiEthernet *AxiEthernetInstance)
{
  /* link partner ability register (5) */
  u16 val;

  XAxiEthernet_PhyRead(AxiEthernetInstance,  0x7, 0x05, &val);

  //xil_printf("PHY register 05 0x%04X\n", val);

  if ( val & 0x8000 ) { /* bit 15 */
    /* LOG(MODULE, LOG_INFO, "1000 mbit"); */
    return 1000;
  } else if ( val & 0x0380) { /* bit 7 = 100half, 8 = 100full, bit 9 = 100baset4 */
    /* LOG(MODULE, LOG_INFO, "100 mbit"); */
    return 100;
  } else if ( val & 0x0060) { /* bit 5 = 10 half, bit 6 = 10 full */
    /* LOG(MODULE, LOG_INFO, "10 mbit"); */
    return 10;
  }

  return 0;
}


int current_link_speed = 0;

int check_update_link_speed(void)
{

	int link_speed = net_get_link_speed(&AxiEthernetInstance);

	if (link_speed == current_link_speed) return link_speed; // no change

	if (link_speed != 0) {
		XAxiEthernet_SetOperatingSpeed(&AxiEthernetInstance, (u16)link_speed);
		sleep(2);
	}

	// just print what happened
	if (current_link_speed == 0) {
		debug_print("Link is up %d MBit.", link_speed);
	} else {
		if (link_speed == 0)
			debug_print("Link down.");
		else
			debug_print("Link speed changed from %d to %d MBit.", current_link_speed, link_speed);
	}

	current_link_speed = link_speed;
	return link_speed;
}

void hardware_reboot(void)
{
	// pull gpio pin down
	debug_print("Initiating reconfiguration ...");
	sleep(5);
	XIo_Out32(XPAR_LEDS_4BITS_BASEADDR + XGPIO_TRI2_OFFSET, 0x00);
	XIo_Out32(XPAR_LEDS_4BITS_BASEADDR + XGPIO_DATA2_OFFSET, 0x00);
	debug_print("should not reach this code");
}


void led_set(u8 id, u8 state) {
	static u8 led_state = 0; // all on at first
	// modify led_state and write
	if (state == 0) { // turn led off, put bit UP
		led_state |= 1 << id;
	} else {
		// turn on, clear bit
		led_state &= ~(1 << id); // xor
	}
	XIo_Out32(XPAR_LEDS_4BITS_BASEADDR, led_state);
}

void led_rotate(int msec)
{
	// 5 leds
	int pos = 0;
	int num_led = 5;

	while (1) {
		led_set(pos,1);
		led_set( ((pos-1) < 0) ? (pos + num_led - 1) : (pos - 1), 0   );

		pos++; pos = pos % num_led; // advance
		usleep(msec*1000);
	}
}

void led_blink_all(int msec)
{
	// blinking all leds
	while(1){
		XIo_Out32(XPAR_LEDS_4BITS_BASEADDR, 0xFF);
		usleep(msec*1000);
		XIo_Out32(XPAR_LEDS_4BITS_BASEADDR, 0x00);
		usleep(msec*1000);
	}
}

void led_countdown(int msec)
{
	led_set(0,1);
	led_set(1,1);
	led_set(2,1);
	led_set(3,1);
	led_set(4,1);
	usleep(msec * 1000);

	usleep(msec * 1000);
	led_set(4,0);
	usleep(msec * 1000);
	led_set(3,0);
	usleep(msec * 1000);
	led_set(2,0);
	usleep(msec * 1000);
	led_set(1,0);
	usleep(msec * 1000);
	led_set(0,0);

}

int eth_send_single_frame_blocking(const unsigned char *frame_data, int frame_length)
{
	XAxiDma_Bd *bd;
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(&AxiDmaInstance);
	XAxiDma_BdRingAlloc(TxRingPtr, 1, &bd);

	Xil_DCacheFlushRange((u32)frame_data, frame_length);


	XAxiDma_BdSetBufAddr(bd, (u32)frame_data);
	XAxiDma_BdSetLength(bd, frame_length);
	XAxiDma_BdSetCtrl(bd, XAXIDMA_BD_CTRL_TXSOF_MASK |
			XAXIDMA_BD_CTRL_TXEOF_MASK); // sending, SOF | EOF bd


#define BD_USR0_OFFSET	0	/* AXI4-Stream Control Word offset from
				 * the start of app user words in BD. Offset
				 * 0 means, Control Word 0, used for enabling
				 * checksum offloading.
				 */
#define BD_USR1_OFFSET	1	/* AXI4-Stream Control Word offset from
				 * the start of app user words in BD. Offset
				 * 1 means, Control Word 1, used for mentioning
				 * checksum begin and checksum insert points
				 */
#define BD_USR2_OFFSET	2	/* AXI4-Stream Control Word offset from
				 * the start of app user words in BD. Offset
				 * 2 means, Control Word 2, used for mentioning
				 * checksum seed.
				 */

#define PARTIAL_CSUM_ENABLE	0x00000001	/* Option for partial csum enable */
#define FULL_CSUM_ENABLE	0x00000002	/* Option for full csum enable */

	// enable checksum offload
	XAxiDma_BdSetAppWord(bd, BD_USR0_OFFSET, FULL_CSUM_ENABLE);


	// start dma transfer
	XAxiDma_BdRingToHw(TxRingPtr,1,bd);
	while (XAxiDma_BdRingFromHw(TxRingPtr,1,&bd) == 0) {
		//xil_printf("T");
	}
	//debug_print("Packet sent!");
	XAxiDma_BdRingFree(TxRingPtr,1,bd); // free the bd

	return XST_SUCCESS;
}

int eth_arp_send_reply(arp_packet_t *arp_in)
{
	if (eth_arp_form_reply(arp_in, (arp_packet_t *)gratious_arp_packet, myMAC, myIP)
			!= 0)
		return eth_send_single_frame_blocking(gratious_arp_packet, sizeof(arp_packet_t));
	else
		return XST_FAILURE;

}

int gfw_send_pong(const unsigned char *ethin)
{
	unsigned char *ethout = TxFrame;
	ip_udp_packet_t *udpp = (ip_udp_packet_t *)ethin;
	int payload = sizeof(gfw_datagram_t) + PONG_DATA_LEN;

	eth_frame_fill_ether_ip_udp_hdr(ethout,
			myMAC, BroadcastMAC,
			myIP, 0xFFFFFFFF,
			SRC_PORT, ntohs(udpp->udp_hdr.source_port),
			payload);

	gfw_datagram_t *gfww = (gfw_datagram_t *)  ((ip_udp_packet_t *)ethout)->data;

	gfww->op = GFW_OP_PING_ACK;
	gfww->id = 1;
	gfww->magic1 = 'G';
	gfww->magic2 = 'F';
	gfww->magic3 = 'W';
	gfww->param1 = 0;
	gfww->data_length = PONG_DATA_LEN;
	memcpy(&gfww->data[0], myMAC, 6);
	memcpy(&gfww->data[6], &myIP, 4);
	memcpy(&gfww->data[10], &self_test_result, 4);

	// wait for a 'random' amount of time (get from mac)
	usleep( (myMAC[5] | ( myMAC[4] << 8) ) / 2  ); // wait up to 255^2 = 65 msec / 2

	eth_send_single_frame_blocking(ethout, sizeof(ip_udp_packet_t) + payload);

	return XST_SUCCESS;
}

int gfw_send_flash_data_transfer_ack(const unsigned char *ethin)
{
	unsigned char *ethout = TxFrame;
	ip_udp_packet_t *udpp = (ip_udp_packet_t *)ethin;
	int payload = sizeof(gfw_datagram_t);

	eth_frame_fill_ether_ip_udp_hdr(ethout,
			myMAC, udpp->ether_hdr.src_mac,
			myIP, udpp->ip_hdr.source_ip,
			SRC_PORT, ntohs(udpp->udp_hdr.source_port),
			payload);

	gfw_datagram_t *gfww = (gfw_datagram_t *)  ((ip_udp_packet_t *)ethout)->data;

	gfww->op = GFW_OP_FLASH_DATA_TRANSFER_ACK;
	gfww->id = ((gfw_datagram_t *)
			(((ip_udp_packet_t *)ethin)->data))->id;
	gfww->magic1 = 'G';
	gfww->magic2 = 'F';
	gfww->magic3 = 'W';
	gfww->param1 = 0;
	gfww->data_length = 0;

	eth_send_single_frame_blocking(ethout, sizeof(ip_udp_packet_t) + payload);

	return XST_SUCCESS;
}

int gfw_write_flash_from_ram(void)
{
	tic();

	XIo_Out32(XPAR_LEDS_4BITS_BASEADDR, 0xFF); // tyrn off leds


	debug_print("Flash complete erase");

	if ( flash_complete_erase() != XST_SUCCESS) {
		debug_print("Flash erase failed.");
		return XST_FAILURE;
	}

	toc();
	debug_print("Copying from ram to flash..");
	if (ram2flash((char *)FLASH_IMAGE_RAM_ADDR, FLASH_IMAGE_SIZE, 0x0) != XST_SUCCESS) {

		debug_print("Copying to flash failed.");
	}

	debug_print("Flash writing complete in %d sec.", (int)toc());

	XIo_Out32(XPAR_LEDS_4BITS_BASEADDR, 0x00); // tyrn on leds

	return XST_SUCCESS;
}


int gfw_handle_udp_packet(const unsigned char *packet, int length)
{
	ip_udp_packet_t * udpp = (ip_udp_packet_t *)packet;

	if (length < (sizeof(ip_udp_packet_t) + sizeof(gfw_datagram_t)))
	{
		debug_print("Discarding UDP packet, size (%d bytes) too short.", length);
		return XST_FAILURE;
	}

	// check magic bytes
	gfw_datagram_t *gfww = (gfw_datagram_t *)udpp->data;
	if ((gfww->magic1 != MAGIC1) ||
			(gfww->magic2 != MAGIC2) ||
			(gfww->magic3 != MAGIC3)) {
		debug_print("Discarding: magic bytes not correct.");
		return XST_FAILURE;
	}

	switch (gfww->op) {
	case GFW_OP_PING_CMD:
		debug_print("Ping CMD");
		gfw_send_pong(packet);

		break;
	case GFW_OP_FLASH_DATA_TRANSFER_CMD:
		//debug_print("Data transfer. Id %d out of %d, %d bytes", gfww->id, gfww->number_of_packets,	gfww->data_length);
		xil_printf("%d/%d\r\n", gfww->id, gfww->number_of_packets);

		memcpy((void *)(FLASH_IMAGE_RAM_ADDR + gfww->address_offset),
				gfww->data, gfww->data_length);

		gfw_send_flash_data_transfer_ack(packet);

		break;

	case GFW_OP_FLASH_WRITE_CMD:
		debug_print("Start flash write cmd received.");

		// send ack?

		gfw_write_flash_from_ram();
		// blinking led
		led_countdown(500);
		hardware_reboot();

		debug_print("Flash write completed.");

		break;
	default:
		debug_print("Unknown operation in packet header. Discarding.");
		return XST_FAILURE;
		break;
	}

	return XST_SUCCESS;
}

// check if the packet should be ignored or processed
int eth_packet_check(unsigned char *frame, int length)
{
#if 0
	// check destination mac address (already checked by hardware)
	if ((memcmp(frame, myMAC, 6) != 0) && (memcmp(frame, BroadcastMAC, 6) != 0))
		return PACKET_TYPE_IGNORE;
#endif


	if (frame[12] != 0x08) return PACKET_TYPE_IGNORE;

	if (frame[13] == 0x06)
	{
		// arp packet
		arp_packet_t *arp = (arp_packet_t *)frame;

		if (arp->target_ip != myIP) return PACKET_TYPE_IGNORE; // doesn't concern us

		return PACKET_TYPE_ARP;


	} else if (frame[13] == 0x00) {
		// ip packet
		ip_udp_packet_t *udp = (ip_udp_packet_t *)frame;
		// check destination IP
		if ((udp->ip_hdr.destination_ip != myIP) &&
				(udp->ip_hdr.destination_ip != 0xFFFFFFFF))  return PACKET_TYPE_IGNORE;

		// now we have an ip packet for us

		if (udp->ip_hdr.protocol != 0x11) return PACKET_TYPE_IGNORE; // not UDP
		//port checking failed on some computers
//		if (udp->udp_hdr.destination_port != DST_PORT) return PACKET_TYPE_IGNORE; // we listen only to dst_port

		return PACKET_TYPE_UDP;
	}

	return PACKET_TYPE_IGNORE;
}


int dma_init(void)
{
	int Status;
	XAxiDma *DmaInstancePtr = &AxiDmaInstance;
	u32 AxiDmaDeviceId = XPAR_AXIDMA_0_DEVICE_ID;

	XAxiDma_BdRing *RxRingPtr = XAxiDma_GetRxRing(DmaInstancePtr);
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(DmaInstancePtr);
	XAxiDma_Bd BdTemplate;



	XAxiDma_Config* DmaConfig = XAxiDma_LookupConfig(AxiDmaDeviceId);
	/*
	 * Initialize AXIDMA engine. AXIDMA engine must be initialized before
	 * AxiEthernet. During AXIDMA engine initialization, AXIDMA hardware is
	 * reset, and since AXIDMA reset line is connected to AxiEthernet, this
	 * would ensure a reset of AxiEthernet.
	 */
	Status = XAxiDma_CfgInitialize(DmaInstancePtr, DmaConfig);
	if(Status != XST_SUCCESS) {
		xil_printf("Error initializing DMA\r\n");
		return XST_FAILURE;
	}


	/*
	 * Setup RxBD space.
	 *
	 * We have already defined a properly aligned area of memory to store
	 * RxBDs at the beginning of this source code file so just pass its
	 * address into the function. No MMU is being used so the physical and
	 * virtual addresses are the same.
	 *
	 * Setup a BD template for the Rx channel. This template will be
	 * copied to every RxBD. We will not have to explicitly set these
	 * again.
	 */

	/*
	 * Disable all RX interrupts before RxBD space setup
	 */
	XAxiDma_BdRingIntDisable(RxRingPtr, XAXIDMA_IRQ_ALL_MASK);

	/*
	 * Create the RxBD ring
	 */
	Status = XAxiDma_BdRingCreate(RxRingPtr, (u32) &RxBdSpace,
			(u32) &RxBdSpace, BD_ALIGNMENT, RXBD_CNT);
	if (Status != XST_SUCCESS) {
		xil_printf("Error setting up RxBD space");
		return XST_FAILURE;
	}

	XAxiDma_BdClear(&BdTemplate);
	Status = XAxiDma_BdRingClone(RxRingPtr, &BdTemplate);
	if (Status != XST_SUCCESS) {
		xil_printf("Error initializing RxBD space");
		return XST_FAILURE;
	}

	/*
	 * Setup TxBD space.
	 *
	 * Like RxBD space, we have already defined a properly aligned area of
	 * memory to use.
	 */

	/*
	 * Create the TxBD ring
	 */
	Status = XAxiDma_BdRingCreate(TxRingPtr, (u32) &TxBdSpace,
			(u32) &TxBdSpace, BD_ALIGNMENT, TXBD_CNT);
	if (Status != XST_SUCCESS) {
		xil_printf("Error setting up TxBD space");
		return XST_FAILURE;
	}
	/*
	 * We reuse the bd template, as the same one will work for both rx
	 * and tx.
	 */
	Status = XAxiDma_BdRingClone(TxRingPtr, &BdTemplate);
	if (Status != XST_SUCCESS) {
		xil_printf("Error initializing TxBD space");
		return XST_FAILURE;
	}

	return XST_SUCCESS;
}

int eth_initt(void)
{
	u32 AxiEthernetDeviceId = XPAR_AXIETHERNET_0_DEVICE_ID;
	XAxiEthernet *AxiEthernetInstancePtr = &AxiEthernetInstance;
	int Status;
	XAxiEthernet_Config *MacCfgPtr;

	/*
	 *  Get the configuration of AxiEthernet hardware.
	 */
	MacCfgPtr = XAxiEthernet_LookupConfig(AxiEthernetDeviceId);

	/*
	 * Initialize AxiEthernet hardware.
	 */
	Status = XAxiEthernet_CfgInitialize(AxiEthernetInstancePtr, MacCfgPtr,
					MacCfgPtr->BaseAddress);
	if (Status != XST_SUCCESS) {
		xil_printf("Error in initialize");
		return XST_FAILURE;
	}

	/*
	 * Set the MAC address
	 */
	debug_print("Setting mac address to %02x:%02x:%02x:%02x:%02x:%02x",
			myMAC[0],myMAC[1],myMAC[2],myMAC[3],myMAC[4],myMAC[5]);
	Status = XAxiEthernet_SetMacAddress(AxiEthernetInstancePtr,
							myMAC);
	if (Status != XST_SUCCESS) {
		xil_printf("Error setting MAC address");
		return XST_FAILURE;
	}

#if 0
	int LoopbackSpeed;

	/*
	 * Set PHY to loopback, speed depends on phy type.
	 * MII is 100 and all others are 1000.
	 */
	if (XAxiEthernet_GetPhysicalInterface(AxiEthernetInstancePtr) ==
						XAE_PHY_TYPE_MII) {
		LoopbackSpeed = 100;
	} else {
		LoopbackSpeed = 1000;
	}
	//AxiEthernetUtilEnterLoopback(AxiEthernetInstancePtr, LoopbackSpeed);
	/*
	 * Set PHY<-->MAC data clock
	 */
	LoopbackSpeed = 100;
	Status =  XAxiEthernet_SetOperatingSpeed(AxiEthernetInstancePtr,
							(u16)LoopbackSpeed);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}


	/*
	 * Setting the operating speed of the MAC needs a delay.  There
	 * doesn't seem to be register to poll, so please consider this
	 * during your application design.
	 */


#endif

	sleep(2);
	return XST_SUCCESS;
}

void eth_frame_fill_gratious_arp(unsigned char *packet, const unsigned char *mac, u32 ip)
{
	arp_packet_t *arp = (arp_packet_t *)packet;

	memset(arp->ether_hdr.dst_mac, 0xFF, 6);
	memcpy(arp->ether_hdr.src_mac, mac, 6);
	arp->ether_hdr.ethertype = Xil_Htons(0x0806);

	arp->hw_type = Xil_Htons(0x0001);
	arp->protocol_type = Xil_Htons(0x0800);
	arp->hw_size = 6;
	arp->protocol_size = 4;
	arp->opcode = Xil_Htons(0x0001);
	memcpy(arp->sender_mac, mac, 6);
	arp->sender_ip = ip;
	memset(arp->target_mac, 0, 6);
	arp->target_ip = ip;
}

int main_init(void)
{
	int Status;

	debug_print("Flash init");
	if (flash_init() != XST_SUCCESS) {
		debug_print("Flash failed to init.");
		self_test_result |= SELF_TEST_FLASH;
	}

	gratious_arp_packet = (unsigned char *)malloc(42);
	eth_frame_fill_gratious_arp(gratious_arp_packet, myMAC, myIP);

	Status = dma_init();
	Status |= eth_initt();

	if (Status != XST_SUCCESS) self_test_result |= SELF_TEST_ETHERNET;

	// start both dma channels
	XAxiDma_BdRingStart(XAxiDma_GetRxRing(&AxiDmaInstance));
	XAxiDma_BdRingStart(XAxiDma_GetTxRing(&AxiDmaInstance));

	// set both channells
	XAxiEthernet_Start(&AxiEthernetInstance);

	return XST_SUCCESS;
}


int listen_server_loop(void)
{
	XAxiDma_BdRing *RxRingPtr = XAxiDma_GetRxRing(&AxiDmaInstance);

	XAxiDma_Bd *bd, *bdset;
	while(1)
	{

		debug_print("Allocating 1 BD for rx");
		// allocate 1 bd
		XAxiDma_BdRingAlloc(RxRingPtr, 1, &bdset);

		memset(&RxFrame, 0, sizeof(RxFrame));

		Xil_DCacheFlushRange((u32)&RxFrame, sizeof(RxFrame));


		//
		bd = bdset;
		XAxiDma_BdSetBufAddr(bd, (u32)&RxFrame);
		XAxiDma_BdSetLength(bd, sizeof(RxFrame));
		XAxiDma_BdSetCtrl(bd, 0); // receiving bd

		// start dma transfer
		XAxiDma_BdRingToHw(RxRingPtr,1,bd);


		while (XAxiDma_BdRingFromHw(RxRingPtr,1,&bdset) == 0) {
			xil_printf(".");
		}

		// bum we received a packet

		XAxiDma_BdRingFree(RxRingPtr,1,bdset); // free the bd

		print_hex_array(RxFrame, 256); // print 256 bytes of the memory area

	}

	return XST_SUCCESS;

}




int echo_server_loop(void)
{
	XAxiDma_BdRing *RxRingPtr = XAxiDma_GetRxRing(&AxiDmaInstance);
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(&AxiDmaInstance);

	XAxiDma_Bd *bd, *bdset;
	while(1)
	{

		//debug_print("Allocating 1 BD for rx");
		// allocate 1 bd
		XAxiDma_BdRingAlloc(RxRingPtr, 1, &bdset);

		memset(RxFrame, 0, sizeof(RxFrame));

		Xil_DCacheFlushRange((u32)RxFrame, sizeof(RxFrame));


		//
		bd = bdset;
		XAxiDma_BdSetBufAddr(bd, (u32)RxFrame);
		XAxiDma_BdSetLength(bd, sizeof(RxFrame));
		XAxiDma_BdSetCtrl(bd, 0); // receiving bd

		// start dma transfer
		XAxiDma_BdRingToHw(RxRingPtr,1,bd);


		while (XAxiDma_BdRingFromHw(RxRingPtr,1,&bdset) == 0) {
			xil_printf(".");
			//xil_printf("\b");

		}

		// bum we received a packet

		XAxiDma_BdRingFree(RxRingPtr,1,bdset); // free the bd

		//print_hex_array(RxFrame, 256); // print 256 bytes of the memory area

		// send it back
		//debug_print("Allocating 1 BD for tx");
				// allocate 1 bd
		XAxiDma_BdRingAlloc(TxRingPtr, 1, &bdset);
		int aa; // fill in our mac!
		for (aa = 0; aa < 6; aa++)
			RxFrame[6+aa] = AxiEthernetMAC[aa];
		Xil_DCacheFlushRange((u32)RxFrame, sizeof(RxFrame));

		//
		bd = bdset;
		XAxiDma_BdSetBufAddr(bd, (u32)RxFrame);
		XAxiDma_BdSetLength(bd, 256);
		XAxiDma_BdSetCtrl(bd, XAXIDMA_BD_CTRL_TXSOF_MASK |
				XAXIDMA_BD_CTRL_TXEOF_MASK); // sending, SOF | EOF bd

		// start dma transfer
		XAxiDma_BdRingToHw(TxRingPtr,1,bd);
		while (XAxiDma_BdRingFromHw(TxRingPtr,1,&bdset) == 0) {
					xil_printf("_");
				}
		//debug_print("Packet sent!");
		XAxiDma_BdRingFree(TxRingPtr,1,bdset); // free the bd



	}

	return XST_SUCCESS;

}

int print_checksum_status(u32 cksumstatus)
{
	char *msg;
	switch (cksumstatus) {
	case IP_OFF_TCPUDP_OFF: msg = "IP not checked, TCP/UDP not checked."; break;
	case IP_OK_TCPUDP_OFF: msg = "IP OK, TCP/UDP not checked."; break;
	case IP_OK_TCP_OK: msg = "IP OK, TCP OK"; break;
	case IP_OK_UDP_OK: msg = "IP OK, UDP OK"; break;

	case IP_NOTOK_TCPUDP_OFF: msg = "IP NOT OK, TCP/UDP not checked"; break;
	case IP_OK_TCP_NOTOK: msg = "IP OK, TCP NOT OK"; break;
	case IP_OK_UDP_NOTOK: msg = "IP OK, UDP NOT OK"; break;
	default: msg = "Unknown checksum status value";
	};

	debug_print("Checksum validation result: %s", msg);
	return XST_SUCCESS;
}

int dma_add_1_rx_bd(u32 destination_address, u32 max_length)
{
	XAxiDma_BdRing *RxRingPtr = XAxiDma_GetRxRing(&AxiDmaInstance);
	XAxiDma_Bd *bd;
	// allocate 1 bd
	if (XAxiDma_BdRingAlloc(RxRingPtr, 1, &bd) != XST_SUCCESS) {
		return XST_FAILURE;
	}

	// clear memory
	memset((char *)destination_address, 0, max_length);
	Xil_DCacheFlushRange(destination_address, max_length);

	// fill in BD
	XAxiDma_BdSetBufAddr(bd, destination_address);
	XAxiDma_BdSetLength(bd, max_length);
	XAxiDma_BdSetCtrl(bd, 0); // receiving bd

	// start dma transfer
	return XAxiDma_BdRingToHw(RxRingPtr,1,bd);
}

int dma_rx_maybe(u32 *crcresult)
{
	XAxiDma_BdRing *RxRingPtr = XAxiDma_GetRxRing(&AxiDmaInstance);
	XAxiDma_Bd *bdset;

	if (XAxiDma_BdRingFromHw(RxRingPtr,1,&bdset) == 0) {
		return -1; // nothing to recieve, no need to put in another BD
	}

	u32 BdSts = XAxiDma_BdGetSts(bdset);
	if ((BdSts & XAXIDMA_BD_STS_ALL_ERR_MASK) ||
			(!(BdSts & XAXIDMA_BD_STS_COMPLETE_MASK))) {
		debug_print("Rx Error");
		XAxiDma_BdRingFree(RxRingPtr,1,bdset); // free the bd
		return 0; // rx error, need to put in anoter BD
	}

	// bum we received a packet
	u32 plength = (XAxiDma_BdRead(bdset,XAXIDMA_BD_USR4_OFFSET)) & 0x0000FFFF;
	//debug_print("boom packet received rx %d plength", plength);


	/*
	 * Read the full checksum validation status from AXI4 Status Word.
	 */
	u32 FullCsumStatus = (((XAxiDma_BdRead(bdset, XAXIDMA_BD_USR2_OFFSET)) &
						FULL_CSUM_STATUS_MASK) >> 3);

	*crcresult = FullCsumStatus;
	//print_checksum_status(FullCsumStatus);

	XAxiDma_BdRingFree(RxRingPtr,1,bdset); // free the bd

	return plength;
}




int pool_rx_blocking(u32 *crcresult)
{
	XAxiDma_BdRing *RxRingPtr = XAxiDma_GetRxRing(&AxiDmaInstance);

	XAxiDma_Bd *bd, *bdset;
	//debug_print("pooling rx");
	// allocate 1 bd
	XAxiDma_BdRingAlloc(RxRingPtr, 1, &bdset);

	memset(RxFrame, 0, sizeof(RxFrame));
	Xil_DCacheFlushRange((u32)RxFrame, sizeof(RxFrame));

	//
	bd = bdset;
	XAxiDma_BdSetBufAddr(bd, (u32)RxFrame);
	XAxiDma_BdSetLength(bd, sizeof(RxFrame));
	XAxiDma_BdSetCtrl(bd, 0); // receiving bd

	// start dma transfer
	XAxiDma_BdRingToHw(RxRingPtr,1,bd);


	//debug_print("RX...\n");
	while (XAxiDma_BdRingFromHw(RxRingPtr,1,&bdset) == 0) {
		util_advance_spinner();
	}

	u32 BdSts = XAxiDma_BdGetSts(bdset);
	if ((BdSts & XAXIDMA_BD_STS_ALL_ERR_MASK) ||
			(!(BdSts & XAXIDMA_BD_STS_COMPLETE_MASK))) {
		debug_print("Rx Error");
		XAxiDma_BdRingFree(RxRingPtr,1,bdset); // free the bd
		return 0;
	}

	// bum we received a packet
	u32 plength = (XAxiDma_BdRead(bdset,XAXIDMA_BD_USR4_OFFSET)) & 0x0000FFFF;
	debug_print("boom packet received rx %d plength", plength);


	/*
	 * Read the full checksum validation status from AXI4 Status Word.
	 */
	u32 FullCsumStatus = (((XAxiDma_BdRead(bdset, XAXIDMA_BD_USR2_OFFSET)) &
						FULL_CSUM_STATUS_MASK) >> 3);

	*crcresult = FullCsumStatus;
	//print_checksum_status(FullCsumStatus);


	XAxiDma_BdRingFree(RxRingPtr,1,bdset); // free the bd

	return plength;
}

int flash_writer_server_loop(void)
{
	debug_print("Flash writer server loop---------");

	debug_print("setting RAM at 0x%08X to 0xFF, %d Kb ---------", FLASH_IMAGE_RAM_ADDR,
			FLASH_IMAGE_SIZE/ 1024);
	memset((char *)FLASH_IMAGE_RAM_ADDR, 0xFF, FLASH_IMAGE_SIZE);

	// put in first RX BD
	if (dma_add_1_rx_bd((u32)RxFrame, sizeof(RxFrame)) != XST_SUCCESS) {
		debug_print("Failed to put a RX BD into DMA.");
		return XST_FAILURE;
	}

	u32 len, crcresult;
	while(1)
	{
		if (check_update_link_speed() == 0) {
			//xil_printf("Link down\r\n");
			continue; // link down
		}

		len = dma_rx_maybe(&crcresult);

		if (len == -1) continue; // dma recieved nothing


		if ((len == 0) ||
				(crcresult == IP_NOTOK_TCPUDP_OFF) ||
				(crcresult == IP_OK_TCP_NOTOK) ||
				(crcresult == IP_OK_UDP_NOTOK)) {
			debug_print("RX packet CRC error or zero length.");
			dma_add_1_rx_bd((u32)RxFrame, sizeof(RxFrame)); // put in another bd;
			continue; // ignore packet, go on, crc is wrong
		}

		// now there is a packet at RxFrame with length len
		// we need to put in another BD as soon as possible, so we don't miss packets
		// in fact ethernet has a buffer so this might not be as important
		// but to be on the safe side, copy the received packet, add the RX BD
		// and then process the packet (which can take some time)

		memcpy((void *)RAM_RX_SPACE, RxFrame, len); // copy data
		dma_add_1_rx_bd((u32)RxFrame, sizeof(RxFrame)); // put in another bd;


		// process received packet at RAM_RX_SPACE
		unsigned char *rx_packet = (unsigned char *)RAM_RX_SPACE;
		//print_hex_array(rx_packet, len);

		int ptype = eth_packet_check(rx_packet, len);
		switch (ptype) {
		case PACKET_TYPE_IGNORE:
			//debug_print("Ignoring packet");
			xil_printf("I");
			break;
		case PACKET_TYPE_ARP:
			debug_print("ARP packet RX");
			eth_arp_send_reply((arp_packet_t *) rx_packet);
			break;
		case PACKET_TYPE_UDP:
			//debug_print("UDP packet RX");
			gfw_handle_udp_packet(rx_packet, len);
			break;
		default: break;
		}




	} // while(1)

	return XST_SUCCESS;
}

const unsigned char hpMAC[6] = { 0x00, 0x17, 0x08, 0x3a, 0x83, 0x9d};
//const unsigned char hpMAC[6] = { 0x70, 0x5a, 0xb6, 0x91, 0xa0, 0x7d};

const u32 hpIP = 0x010aa8c0;

int tx_speed_test(void)
{
	debug_print("--- TX SPEED TEST -----");
	int num_of_bds = 8; // how much BDs at the same time to use for TX

	///// generate packets in RAM
	u32 tx_start_addr = RAM_TX_SPACE;
	tx_start_addr = 0xC3000000;
	u32 tx_size = 64 * 1024 * 1024; // bytes of tx space for data

	// single packet size: (should be divisable by 4 bytes (32-bit aligment)
	u32 payload_length = 1472; // + 14 + 20 + 8 = 42 => 1514
	u32 frame_length = payload_length + ETH_IP_UDP_HEADER_LENGTH;

	int num_of_allocated_tx_packets = tx_size / frame_length;

	int i_packet = 0; // a variable between 0-num_of_allocated_tx_packets-1 used to fill the next BD

	// setup data for TX

	debug_print("Setting up packet data for %d packets with %d bytes length (together %d kB) at ram addr 0x%08x",
			num_of_allocated_tx_packets, frame_length,
			(frame_length * num_of_allocated_tx_packets) / 1024, tx_start_addr);
	int i;


	for (i = 0; i < num_of_allocated_tx_packets; i++) {
		u32 frame_addr = tx_start_addr + i * frame_length;

		eth_frame_fill_ether_ip_udp_hdr((unsigned char *)frame_addr,
				myMAC, hpMAC,
				myIP, hpIP,
				5333, 5000, payload_length);

		unsigned char *payload_data = (unsigned char*)(frame_addr + ETH_IP_UDP_HEADER_LENGTH);

		util_16pattern((char *)payload_data, payload_length);

		payload_data[0] = (u8)i; // first data byte is the counter.
	}
	// flush cache for this region
	Xil_DCacheFlushRange(tx_start_addr, (i+1)*frame_length);

	debug_print("Done");

	// set up bd ring

	XAxiDma_Bd *bdset, *bd;
	XAxiDma_BdRing *TxRingPtr = XAxiDma_GetTxRing(&AxiDmaInstance);
	if (XAxiDma_BdRingAlloc(TxRingPtr, num_of_bds, &bdset) != XST_SUCCESS) {
		debug_print("Couldn't get %d BDs", num_of_bds);
		return XST_FAILURE;
	}

	bd = bdset;
	// fill in BDs
	for (i = 0; i < num_of_bds; i++) {

		u32 frame_addr = tx_start_addr + i_packet * frame_length;


		XAxiDma_BdSetBufAddr(bd, (u32)frame_addr);
		XAxiDma_BdSetLength(bd, frame_length);
		XAxiDma_BdSetCtrl(bd, XAXIDMA_BD_CTRL_TXSOF_MASK |
				XAXIDMA_BD_CTRL_TXEOF_MASK); // sending, SOF | EOF bd

		// enable checksum offload
		XAxiDma_BdSetAppWord(bd, BD_USR0_OFFSET, FULL_CSUM_ENABLE);

		bd = XAxiDma_BdRingNext(TxRingPtr, bd);
		i_packet++; i_packet = i_packet % num_of_allocated_tx_packets; // move to next packet in ram

	}
	// start dma transfer
	XAxiDma_BdRingToHw(TxRingPtr,num_of_bds,bdset);



#if 0
	int num = num_of_bds;
	while (num--){
		while (XAxiDma_BdRingFromHw(TxRingPtr,1,&bd) == 0) {
			xil_printf("T");
		}
		debug_print("Packet sent! %d", num);
		XAxiDma_BdRingFree(TxRingPtr,1,bd); // free the bd


	}
#endif

	while (1) { // send packet endlessly
		int num_bd = XAxiDma_BdRingFromHw(TxRingPtr,XAXIDMA_ALL_BDS,&bdset);

		if (num_bd == 0) continue;

		//debug_print("%d buffer descriptors freed", num_bd);
#if 1
		XAxiDma_BdRingFree(TxRingPtr,num_bd,bdset); // free the bds

		if (XAxiDma_BdRingAlloc(TxRingPtr, num_bd, &bdset) != XST_SUCCESS) {
				debug_print("Couldn't get %d BDs", num_bd);
				continue;
		}
#endif

		bd = bdset;
		for (i = 0; i < num_bd; i++) {
			u32 frame_addr = tx_start_addr + i_packet * frame_length;


			XAxiDma_BdSetBufAddr(bd, (u32)frame_addr);
			XAxiDma_BdSetLength(bd, frame_length);
			XAxiDma_BdSetCtrl(bd, XAXIDMA_BD_CTRL_TXSOF_MASK |
					XAXIDMA_BD_CTRL_TXEOF_MASK); // sending, SOF | EOF bd

			// enable checksum offload
			XAxiDma_BdSetAppWord(bd, BD_USR0_OFFSET, FULL_CSUM_ENABLE);

			bd = XAxiDma_BdRingNext(TxRingPtr, bd);
			i_packet++; i_packet = i_packet % num_of_allocated_tx_packets; // move to next packet in ram

		}
		// start dma transfer
		//debug_print("Starting transfer for %d bds", num_bd);
		XAxiDma_BdRingToHw(TxRingPtr,num_bd,bdset);
	}


	return XST_SUCCESS;
}

int ram_test(u32 addr, u32 num_of_words)
{
	xil_printf("Testing RAM at addr 0x%08X, %d words (%d bytes) ... ", addr, num_of_words, num_of_words*4);
	int status = Xil_TestMem32((u32 *)addr, num_of_words, 0xAAAA5555, XIL_TESTMEM_ALLMEMTESTS);
	if (status == XST_SUCCESS)
		xil_printf("OK.\r\n");
	else
		xil_printf("FAILED.\r\n");

	return status;
}



u32 perform_selftest(void)
{
	u32 self_test = SELF_TEST_ALL_OK;
	debug_print("------------- Performing self-test. --------------");



	// ram test
	// test 1kb at 4 locations in both chips write, read
	debug_print("Performing RAM self-test...");

	int ram_test_size_words = 1024;
	u32 *test_area = (u32 *)malloc(sizeof(u32)*ram_test_size_words);
	int status = 0; // = XST_SUCCESS
	status |= ram_test((u32)test_area, ram_test_size_words);
	free(test_area);

	u32 addr;
	for (addr = XPAR_MCB_DDR3_S0_AXI_BASEADDR + 0x01000000; addr < (XPAR_MCB1_DDR3_S0_AXI_HIGHADDR-ram_test_size_words);
			addr += 0x02000000)
		status |= ram_test(addr, ram_test_size_words);

	if (status != XST_SUCCESS) {
		debug_print("One of RAM tests failed.");
		led_set(1,0);
		self_test |= SELF_TEST_RAM;
	} else {
		debug_print("RAM OK.");
		led_set(1,1);
	}


	// flash test
	// send command '9Fh' = JEDEC ID, expect 3 bytes. 0xEF 0x40 0x17
	debug_print("Performing FLASH self-test...");
	u16 devcode = Isf.DeviceCode;
	debug_print("Device code is 0x%04X.", devcode);

	if (devcode == 0x4017) {
		led_set(2,1);
		debug_print("Flash OK.\r\n");
	} else {
		led_set(2,0);
		debug_print("Flash FAILED.\r\n");
		self_test |= SELF_TEST_FLASH;
	}



	// eeprom test (already tested before calling this function
	if (self_test_result & SELF_TEST_EEPROM) {
		debug_print("\r\nEEPROM reading FAILED.");
		led_set(3,0);
	} else {
		debug_print("\r\nEEPROM reading ... OK.");
		led_set(3,1);
	}




	return self_test;
}


// send and receive packets
int main(void)
{
	xil_printf("\r\n\r\n---- Entering main() ---- \r\n");
#if XPAR_MICROBLAZE_USE_ICACHE
	Xil_ICacheInvalidate();
	Xil_ICacheEnable();
#endif
#if XPAR_MICROBLAZE_USE_DCACHE
	Xil_DCacheInvalidate();
	Xil_DCacheEnable();
#endif

	led_set(0,0);
	led_set(1,0); // turn leds off
	led_set(2,0);
	led_set(3,0);
	led_set(4,0);

	// initialize timer
	if (timing_init() != XST_SUCCESS) {
		debug_print("Failed to init timer.");
		self_test_result |= SELF_TEST_OTHER;
		led_set(0,0);
	} else {
		debug_print("Timer initialized.");
		led_set(0,1); // turn main led ON
	}


	// set mac address from eeprom 3 bytes.
	eeprom_readout_t ser;
	if (eeprom_get_serial(&ser) != XST_SUCCESS) {
		self_test_result |= SELF_TEST_EEPROM;
		led_set(3,0);
	} else led_set(3,1);
	myMAC[3] = ser.data[0];
	myMAC[4] = ser.data[1];
	myMAC[5] = ser.data[2];
	myIP &= 0x00FFFFFF;
	u8 lastbyte = myMAC[5];
	if (lastbyte <= 10) lastbyte +=10; if (lastbyte == 255) lastbyte--;
	myIP |= lastbyte<<24;
	debug_print("Ip address: %d.%d.%d.%d",
			(myIP&0x000000FF),
			(myIP&0x0000FF00) >> 8,
			(myIP&0x00FF0000) >> 16,
			(myIP&0xFF000000) >> 24

	);

	debug_print("main init");
	main_init();


	self_test_result |= perform_selftest();


	check_update_link_speed();
	led_set(4,1); // if we survived this, then reading phy succeded


	//listen_server_loop();
	//echo_server_loop();
#if 1
	eth_send_single_frame_blocking(gratious_arp_packet, sizeof(arp_packet_t));
	eth_send_single_frame_blocking(gratious_arp_packet, sizeof(arp_packet_t));
	eth_send_single_frame_blocking(gratious_arp_packet, sizeof(arp_packet_t));
	eth_send_single_frame_blocking(gratious_arp_packet, sizeof(arp_packet_t));
	eth_send_single_frame_blocking(gratious_arp_packet, sizeof(arp_packet_t));
	//return 0;
#endif



	//tx_speed_test(); return 0;
	flash_writer_server_loop();

    xil_printf("---- Exiting main() ---- \r\n");
	return 0;
}
