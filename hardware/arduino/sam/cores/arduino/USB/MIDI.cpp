#include "Arduino.h"
#include "USBAPI.h"
#include "Reset.h"
#include "LineInfo.h"

#ifdef MIDI_ENABLED

#define MIDI_BUFFER_SIZE	128

struct ring_bufferMIDI
{
	midiEventPacket_t midiEvent[MIDI_BUFFER_SIZE];
	volatile uint32_t head;
	volatile uint32_t tail;
};

ring_bufferMIDI midi_rx_buffer = {{0,0,0,0 }, 0, 0};



static volatile LineInfo _midiLineInfo = { 
    31250, // dWDTERate
    0x00,  // bCharFormat
    0x00,  // bParityType
    0x08,  // bDataBits
    0x00   // lineState
};



_Pragma("pack(1)")
static const MIDIDescriptor _midiInterface =
{
#ifdef CDC_ENABLED
	D_IAD(MIDI_AC_INTERFACE,MIDI_INTERFACE_COUNT, MIDI_AUDIO, MIDI_AUDIO_CONTROL, 0),
#endif
	D_INTERFACE(MIDI_AC_INTERFACE,0,MIDI_AUDIO,MIDI_AUDIO_CONTROL,0),
	D_AC_INTERFACE(0x1, MIDI_INTERFACE),
	D_INTERFACE(MIDI_INTERFACE,2, MIDI_AUDIO,MIDI_STREAMING,0),
	D_AS_INTERFACE,
	D_MIDI_INJACK(MIDI_JACK_EMD, 0x1),
	D_MIDI_INJACK(MIDI_JACK_EXT, 0x2),
	D_MIDI_OUTJACK(MIDI_JACK_EMD, 0x3, 1, 2, 1),
	D_MIDI_OUTJACK(MIDI_JACK_EXT, 0x4, 1, 1, 1),
	D_MIDI_JACK_EP(USB_ENDPOINT_OUT(MIDI_ENDPOINT_OUT),USB_ENDPOINT_TYPE_BULK,0x0040),
	D_MIDI_AC_JACK_EP(1, 1),
	D_MIDI_JACK_EP(USB_ENDPOINT_IN(MIDI_ENDPOINT_IN),USB_ENDPOINT_TYPE_BULK,0x0040),
	D_MIDI_AC_JACK_EP (1, 3)
};
_Pragma("pack()")

int WEAK MIDI_GetInterface(uint8_t* interfaceNum)
{
	interfaceNum[0] += 2;	// uses 2
	return USBD_SendControl(0,&_midiInterface,sizeof(_midiInterface));
}
bool WEAK MIDI_Setup(Setup& setup)
{
	//Support requests here if needed. Typically these are optional
	uint8_t r = setup.bRequest;
	uint8_t requestType = setup.bmRequestType;


	// if (REQUEST_HOSTTODEVICE_CLASS_INTERFACE == requestType)
	// {
		// needed???
		// if (CDC_SET_LINE_CODING == r)
		// {
		// 	USBD_RecvControl((void*)&_midiLineInfo,7);
		// 	return true;
		// }

		// make sure we track when it's connected, for write()
		if (CDC_SET_CONTROL_LINE_STATE == r)
		{
			_midiLineInfo.lineState = setup.wValueL;
			return true;
		}
	//}
	return false;
}

void MIDI_::begin()
{
	//Nothing to do
}

void MIDI_::accept(void)
{
	static uint32_t mguard = 0;

	// synchronized access to guard
	do {
		if (__LDREXW(&mguard) != 0) {
			__CLREX();
			return;  // busy
		}
	} while (__STREXW(1, &mguard) != 0); // retry until write succeed

	ring_bufferMIDI *buffer = &midi_rx_buffer;
	uint32_t i = (uint32_t)(buffer->head+1) % MIDI_BUFFER_SIZE;

	// if we should be storing the received character into the location
	// just before the tail (meaning that the head would advance to the
	// current location of the tail), we're about to overflow the buffer
	// and so we don't write the character or advance the head.
	while (i != buffer->tail) {
		int c;
		midiEventPacket_t event;
		if (!USBD_Available(MIDI_RX)) {
			udd_ack_fifocon(MIDI_RX);
			break;
		}
		c = USBD_Recv(MIDI_RX, &event, sizeof(event) );
		
		//MIDI paacket has to be 4 bytes
		if(c < 4)
			break; // make sure to release the guard!
		buffer->midiEvent[buffer->head] = event;
		buffer->head = i;

		i = (i + 1) % MIDI_BUFFER_SIZE;
	}

	// release the guard
	mguard = 0;
}

uint32_t MIDI_::available(void)
{
	
	ring_bufferMIDI *buffer = &midi_rx_buffer;
	return (uint32_t)(MIDI_BUFFER_SIZE + buffer->head - buffer->tail) % MIDI_BUFFER_SIZE;
}


midiEventPacket_t MIDI_::read(void)
{
	ring_bufferMIDI *buffer = &midi_rx_buffer;
	midiEventPacket_t c = buffer->midiEvent[buffer->tail];
	c.header = 0;
	c.byte1 = 0;
	c.byte2 = 0;
	c.byte3 = 0;

	// if the head isn't ahead of the tail, we don't have any characters
	if (buffer->head == buffer->tail)
	{
		return c;
	}
	else
	{
		midiEventPacket_t c = buffer->midiEvent[buffer->tail];
		buffer->tail = (uint32_t)(buffer->tail + 1) % MIDI_BUFFER_SIZE;
		if (USBD_Available(MIDI_RX))
			accept();
		return c;
	}
}

void MIDI_::flush(void)
{
	USBD_Flush(MIDI_TX);
}

size_t MIDI_::write(const uint8_t *buffer, size_t size)
{
	/* only try to send bytes if the high-level MIDI connection itself
	 is open (not just the pipe) - the OS should set lineState when the port
	 is opened and clear lineState when the port is closed.
	 bytes sent before the user opens the connection or after
	 the connection is closed are lost - just like with a UART. */

	// TODO - ZE - check behavior on different OSes and test what happens if an
	// open connection isn't broken cleanly (cable is yanked out, host dies
	// or locks up, or host virtual serial port hangs)

	// first, check the TX buffer to see if there's any space left.
	// this may fill up if there's no one listening on the other end.
	// in that case, we don't want to block waiting for the buffer to empty 
	// (which is what USBD_Send() does.)
	// instead, we'll just drop the packets and hope the caller figures it out.
	//if (USBD_SendSpace(MIDI_TX) > size) {
	if (_midiLineInfo.lineState > 0) {
		int r = USBD_Send(MIDI_TX, buffer, size);

		if (r > 0)
		{
			return r;
		} else
		{
			return 0;
		}
	}
	return 0;
}

void MIDI_::sendMIDI(midiEventPacket_t event)
{
	uint8_t data[4];
	data[0] = event.header;
	data[1] = event.byte1;
	data[2] = event.byte2;
	data[3] = event.byte3;
	write(data, 4);
}

MIDI_ MidiUSB;
#endif