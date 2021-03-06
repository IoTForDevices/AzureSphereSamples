/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "intercore_messages.h"
#include "mt3620-baremetal.h"
#include "mt3620-timer.h"
#include "mt3620-intercore.h"
#include "mt3620-gpio.h"

#define MT3620_RDB_LED1_RED		(8)			// GPIO_GROUP_2:GPIO_0 ==> GPIO #8
#define MT3620_RDB_LED2_GREEN	(16)		// GPIO_GROUP_4:GPIO_0 ==> GPIO #16
#define MT3620_RDB_LED3_BLUE	(20)		// GPIO_GROUP_5:GPIO_0 ==> GPIO #20



static bool bIsLedOn = false;
static const int ledGpio = MT3620_RDB_LED1_RED;
static const uint32_t auBlinkIntervalsMs[] = {125, 250, 500};
static size_t nBlinkIntervalIndex = 0;
static const size_t numBlinkIntervals = sizeof(auBlinkIntervalsMs) / sizeof(auBlinkIntervalsMs[0]);

//static const uint32_t uInterCoreTimerMs = 10;
//static void HandleInterCoreTimerIrq(void);


char strComponentId[48];


BufferHeader * outbound, * inbound;
uint32_t sharedBufSize = 0;


// ARM DDI0403E.d SB1.5.2-3
// From SB1.5.3, "The Vector table must be naturally aligned to a power of two whose alignment
// value is greater than or equal to (Number of Exceptions supported x 4), with a minimum alignment
// of 128 bytes.". The array is aligned in linker.ld, using the dedicated section ".vector_table".

extern uint32_t StackTop; // &StackTop == end of TCM0

static _Noreturn void DefaultExceptionHandler(void);
static _Noreturn void RTCoreMain(void);
static void HandleBlinkTimerIrq(void);


// The exception vector table contains a stack pointer, 15 exception handlers, and an entry for
// each interrupt.
#define INTERRUPT_COUNT 100 // from datasheet
#define EXCEPTION_COUNT (16 + INTERRUPT_COUNT)
#define INT_TO_EXC(i_) (16 + (i_))
const uintptr_t ExceptionVectorTable[EXCEPTION_COUNT] __attribute__((section(".vector_table")))
__attribute__((used)) = {
    [0] = (uintptr_t)&StackTop,                // Main Stack Pointer (MSP)
    [1] = (uintptr_t)RTCoreMain,               // Reset
    [2] = (uintptr_t)DefaultExceptionHandler,  // NMI
    [3] = (uintptr_t)DefaultExceptionHandler,  // HardFault
    [4] = (uintptr_t)DefaultExceptionHandler,  // MPU Fault
    [5] = (uintptr_t)DefaultExceptionHandler,  // Bus Fault
    [6] = (uintptr_t)DefaultExceptionHandler,  // Usage Fault
    [11] = (uintptr_t)DefaultExceptionHandler, // SVCall
    [12] = (uintptr_t)DefaultExceptionHandler, // Debug monitor
    [14] = (uintptr_t)DefaultExceptionHandler, // PendSV
    [15] = (uintptr_t)DefaultExceptionHandler, // SysTick

    [INT_TO_EXC(0)] = (uintptr_t)DefaultExceptionHandler,
    [INT_TO_EXC(1)] = (uintptr_t)Gpt_HandleIrq1,
    [INT_TO_EXC(2)... INT_TO_EXC(INTERRUPT_COUNT - 1)] = (uintptr_t)DefaultExceptionHandler};

static _Noreturn void DefaultExceptionHandler(void)
{
    for (;;) {
        //empty;
    }
}

static void HandleBlinkTimerIrq(void)
{
    bIsLedOn = !bIsLedOn;
    Mt3620_Gpio_Write(ledGpio, bIsLedOn);

    Gpt_LaunchTimerMs(TimerGpt0, auBlinkIntervalsMs[nBlinkIntervalIndex], HandleBlinkTimerIrq);
}



//static void HandleInterCoreTimerIrq(void)
//{
//  Gpt_LaunchTimerMs(TimerGpt1, uInterCoreTimerMs, HandleInterCoreTimerIrq);
//}

static _Noreturn void RTCoreMain(void)
{
    // SCB->VTOR = ExceptionVectorTable
    WriteReg32(SCB_BASE, 0x08, (uint32_t)ExceptionVectorTable);

    Gpt_Init();

	// Block 1 GPIO 0::3 on Header1
	static const GpioBlock grp0 = {
		.baseAddr = 0x38010000,.type = GpioBlock_PWM,.firstPin = 0,.pinCount = 4 };
	// Block 1 GPIO 4::7 on Header1,Header2
	static const GpioBlock grp1 = {
	.baseAddr = 0x38020000,.type = GpioBlock_PWM,.firstPin = 4,.pinCount = 4 };
	// Block 2 GPIO 8::11 includes LED1_RED(O8), LED1_GREEN(O9), LED1_BLUE(10), WIFI_BLUE(11)
	static const GpioBlock grp2 = {
        .baseAddr = 0x38030000, .type = GpioBlock_PWM, .firstPin = 8, .pinCount = 4};

    Mt3620_Gpio_AddBlock(&grp0);
	Mt3620_Gpio_AddBlock(&grp1);
	Mt3620_Gpio_AddBlock(&grp2);

    // Block 3 GPIO 12::15 includes BtnA(12), BtnB(13), WIFI_GREEN(14), LED2_RED(15)
    static const GpioBlock grp3 = {
        .baseAddr = 0x38040000, .type = GpioBlock_GRP, .firstPin = 12, .pinCount = 4};
	// Block 3 GPIO 16::19 includes LED2_GREEN(16), LED2_BLUE(17), LED3_RED(18), LED3_GREEN(19)
	static const GpioBlock grp4 = {
		.baseAddr = 0x38050000,.type = GpioBlock_GRP,.firstPin = 16,.pinCount = 4 };
	// Block 3 GPIO 20::23 includes LED3_BLUE(20), LED4_RED(21), LED4_GREEN(22), LED4_BLUE(23)
	static const GpioBlock grp5 = {
		.baseAddr = 0x38060000,.type = GpioBlock_GRP,.firstPin = 20,.pinCount = 4 };

    Mt3620_Gpio_AddBlock(&grp3);
	Mt3620_Gpio_AddBlock(&grp4);
	Mt3620_Gpio_AddBlock(&grp5);

    Mt3620_Gpio_ConfigurePinForOutput(ledGpio);

	if (GetIntercoreBuffers(&outbound, &inbound, &sharedBufSize) == -1) {
		for (;;) {
			// empty.
		}
	}

    Gpt_LaunchTimerMs(TimerGpt0, auBlinkIntervalsMs[nBlinkIntervalIndex], HandleBlinkTimerIrq);
    //Gpt_LaunchTimerMs(TimerGpt1, uInterCoreTimerMs, HandleInterCoreTimerIrq);

	// the main program loop is constantly checking the intercore communication buffer
	uint8_t buf[128];
	static const size_t payloadStart = 20;

	for (;;) {
		uint32_t dataSize = sizeof(buf);
		int nBytes=0;

		// On success, dataSize is set to the actual number of bytes which were read.
		int r = DequeueData(outbound, inbound, sharedBufSize, buf, &dataSize);
		if (r == -1 || dataSize < payloadStart) {
			continue;
		}

		InterCoreMessageLayout* pMessage = (InterCoreMessageLayout*)buf;
		size_t payloadBytes = dataSize - payloadStart;

		//nBytes = Guid_ToString(&pMessage->ComponentId, strComponentId);

		if (payloadBytes >= sizeof(InterCoreMessageHeader))
		{
			// if message is "PING", respond with "ping"
			if (InterCoreMessage_Ping.MagicValue == ((InterCoreMessageHeader*)pMessage->Payload)->MagicValue)
			{
				__builtin_memcpy(pMessage->Payload, &InterCoreMessage_ReceivedResponse, sizeof(InterCoreMessage_ReceivedResponse));
				nBytes = EnqueueData(inbound, outbound, sharedBufSize, buf, sizeof(InterCoreMessageLayout) + sizeof(InterCoreMessage_PingResponse));
			}
			// if message is "BLNK", respond with "recv"
			if (InterCoreMessage_BlinkInterval.MagicValue == ((InterCoreMessageHeader*)pMessage->Payload)->MagicValue)
			{
				nBlinkIntervalIndex = ((InterCoreMessageUint32 *)pMessage->Payload)->Value % numBlinkIntervals;
				__builtin_memcpy(pMessage->Payload, &InterCoreMessage_ReceivedResponse, sizeof(InterCoreMessage_ReceivedResponse));
				nBytes = EnqueueData(inbound, outbound, sharedBufSize, buf, sizeof(InterCoreMessageLayout)+sizeof(InterCoreMessage_ReceivedResponse));
			}
		}
	}
}
