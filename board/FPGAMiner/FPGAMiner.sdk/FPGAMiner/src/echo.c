/******************************************************************************
*
* Copyright (C) 2009 - 2014 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "xparameters.h"
#include "xgpio_l.h"
#include "platform.h"
#include "xstatus.h"
#include "xintc_l.h"
#include "xparameters.h"
#include "xil_io.h"
#include "xuartlite_l.h"
#include "xil_exception.h"
#include "platform_config.h"

#include "lwip/err.h"
#include "lwip/tcp.h"
#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#endif

#define INTC_BASEADDR			XPAR_INTC_0_BASEADDR
#define INTC_DEVICE_ID			XPAR_INTC_0_DEVICE_ID
#define FIT_TIMER_0_INT_ID		XPAR_MICROBLAZE_0_AXI_INTC_FIT_TIMER_0_INTERRUPT_INTR
#define FIT_TIMER_0_INT_MASK	XPAR_FIT_TIMER_0_INTERRUPT_MASK

static unsigned int loadingBarActive = 0;

int transfer_data() {
	return 0;
}

void print_app_header()
{
	xil_printf("\n\r\n\r-----lwIP TCP echo server ------\n\r");
	xil_printf("TCP packets sent to port 6001 will be echoed back\n\r");
}


void TimerIntCallbackHandler(void *CallbackRef) // Will be called at every timer output event
{
	static int irqCount = 0;
	static int counter = 0;
	unsigned int loadingValues[16] = {0x01, 0x03, 0x07, 0x0F, 0x01F, 0x03F, 0x07F, 0x0FF,
			0x01FF, 0x03FF, 0x07FF, 0x0FFF, 0x01FFF, 0x03FFF, 0x07FFF, 0x0FFFF};
	xil_printf("HELLOOOOOOOOOOOOO");

	irqCount++;
	// loadingBarActive codes:
	// 0 stops
	// 1 starts
	// 2 enable all
	if (loadingBarActive == 2)
		counter = 15;
	else if (irqCount == 500 && loadingBarActive) {
		irqCount = 0;
		counter = (counter+1) % 16;
	}

	// Leds
	XGpio_WriteReg(XPAR_AXI_GPIO_LED_BASEADDR, XGPIO_DATA_OFFSET, loadingValues[counter]);
}

err_t recv_callback(void *arg, struct tcp_pcb *tpcb,
                               struct pbuf *p, err_t err)
{
	int output_number_pkts = 9;
	unsigned int *recv, send[output_number_pkts];
	unsigned int r;
	int mask = 0xF0000000;
	int packet_size;

	/* do not read the packet if we are not in ESTABLISHED state */
	if (!p) {
		tcp_close(tpcb);
		tcp_recv(tpcb, NULL);
		return ERR_OK;
	}

	recv = p->payload;
	packet_size = p->len / 4;
	unsigned int tmp[packet_size];

	// Convert from big endian to little endian and remove initial zeros
	xil_printf("\nReceived new block to hash...");
	recv[packet_size] = ntohl(recv[packet_size]);
	for (int i = packet_size - 1; i >= 0; i--) {
		recv[i] = ntohl(recv[i]);
		tmp[i] = (recv[i] & 0x0000FFFF) << 16 | (recv[i + 1] & 0xFFFF0000) >> 16;
	}

	// Generate mask given the number of zeros
	tmp[packet_size - 1] = tmp[packet_size - 1] < 1 || tmp[packet_size - 1] > 8 ?
			mask : mask >> (tmp[packet_size - 1] - 1) * 4;

	// Produce hash
	for (int i = 0; i < packet_size; i++)
		putfsl(tmp[i], 0);

	// Start loading bar
	loadingBarActive = 1;

	unsigned int nonce;
	for (int i = 0; i < output_number_pkts-1; i++) {
		getfsl(r, 0);
		if (i == output_number_pkts-1)
			nonce = r;
		send[i] = htonl(r);
	}

	// Stop loading bar
	loadingBarActive = 2;

	// Read nonce
	getfsl(nonce, 0);
	send[output_number_pkts-1] = htonl(nonce);

	// Split nonce into digits
	int digits[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	unsigned int step = 0;
	while (nonce > 0 && step < 8) {
		digits[step++] = nonce % 10;
		nonce /= 10;
	}

	// Show nonce on displays
	unsigned int digitsValue = digits[7] << 28 | digits[6] << 24 | digits[5] << 20
			| digits[4] << 16 | digits[3] << 12 | digits[2] << 8 | digits[1] << 4 | digits[0];
	XGpio_WriteReg(XPAR_DISPLAYCONTROLLER_0_S00_AXI_BASEADDR, 4, digitsValue);

	xil_printf("\nBlock hashed. Sending value to remote server...");

	/* indicate that the packet has been received */
	tcp_recved(tpcb, p->len);

	/* echo back the payload */
	/* in this case, we assume that the payload is < TCP_SND_BUF */
	if (tcp_sndbuf(tpcb) > p->len) {
		err = tcp_write(tpcb, (void*)&send, output_number_pkts * 4, 1);
	} else
		xil_printf("no space in tcp_sndbuf\n\r");

	/* free the received pbuf */
	pbuf_free(p);

	return ERR_OK;
}

err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	static int connection = 1;

	/* set the receive callback for this connection */
	tcp_recv(newpcb, recv_callback);

	/* just use an integer number indicating the connection id as the
	   callback argument */
	tcp_arg(newpcb, (void*)(UINTPTR)connection);

	/* increment for subsequent accepted connections */
	connection++;

	return ERR_OK;
}

int SetupInterrupts(u32 IntcBaseAddress)
{
	/*
	 * Connect a callback handler that will be called when an interrupt for the timer occurs,
	 * to perform the specific interrupt processing for the timer.
	 */
	XIntc_RegisterHandler(IntcBaseAddress, FIT_TIMER_0_INT_ID,
						  (XInterruptHandler)TimerIntCallbackHandler, (void *)0);


	/*
	 * Enable interrupts for all devices that cause interrupts, and enable
	 * the INTC master enable bit.
	 */
	//XIntc_EnableIntr(IntcBaseAddress, FIT_TIMER_0_INT_MASK | );

	/*
	 * Set the master enable bit.
	 */
	XIntc_Out32(IntcBaseAddress + XIN_MER_OFFSET, XIN_INT_HARDWARE_ENABLE_MASK |
												  XIN_INT_MASTER_ENABLE_MASK);

	/*
	 * This step is processor specific, connect the handler for the
	 * interrupt controller to the interrupt source for the processor.
	 */
	Xil_ExceptionInit();

	/*
	 * Register the interrupt controller handler with the exception table.
	 */
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
								 (Xil_ExceptionHandler)XIntc_DeviceInterruptHandler,
								 INTC_DEVICE_ID);

	/*
	 * Enable exceptions.
	 */
	Xil_ExceptionEnable();

	return XST_SUCCESS;
}


int start_application()
{
	struct tcp_pcb *pcb;
	err_t err;
	unsigned port = 7;

	/*
	 * Run the low level example of Interrupt Controller, specify the Base
	 * Address generated in xparameters.h.
	 */
	int status = SetupInterrupts(INTC_BASEADDR);
	if (status != XST_SUCCESS) {
		xil_printf("Setup interrupts failed\r\n");
		cleanup_platform();
		return XST_FAILURE;
	}

	/* create new TCP PCB structure */
	pcb = tcp_new();
	if (!pcb) {
		xil_printf("Error creating PCB. Out of Memory\n\r");
		return -1;
	}

	/* bind to specified @port */
	err = tcp_bind(pcb, IP_ADDR_ANY, port);
	if (err != ERR_OK) {
		xil_printf("Unable to bind to port %d: err = %d\n\r", port, err);
		return -2;
	}

	/* we do not need any arguments to callback functions */
	tcp_arg(pcb, NULL);

	/* listen for connections */
	pcb = tcp_listen(pcb);
	if (!pcb) {
		xil_printf("Out of memory while tcp_listen\n\r");
		return -3;
	}

	/* specify callback to use for incoming connections */
	tcp_accept(pcb, accept_callback);

	xil_printf("TCP echo server started @ port %d\n\r", port);

	return 0;
}