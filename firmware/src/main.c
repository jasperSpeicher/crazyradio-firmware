/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyradio firmware
 *
 * Copyright (C) 2011-2013 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * main.c - Main file
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "nRF24LU1p.h"
#include "nRF24L01.h"

#include "pinout.h"
#include "utils.h"
#include "radio.h"
#include "usb.h"
#include "led.h"
#ifdef PPM_JOYSTICK
#include "ppm.h"
#endif


//Compilation seems bugged on SDCC 3.1, imposing 3.2
//Comment-out the three following lines only if you know what you are doing!
//#if SDCC != 320
//#error Compiling with SDCC other than 3.2 is not supported due to a bug when launching the bootloader
//#endif

void checkBootPin();
void launchBootloader();
void handleUsbVendorSetup();
void legacyRun();
void prxRun();
void cmdRun();

//Transmit buffer
__xdata char tbuffer[64];
//Receive buffer (from the ack)
__xdata char rbuffer[64];
// Address for bulk tx
__xdata char bulkAddress[5] = { 0xE7, 0xE7, 0xE7, 0, 0 };
// data for bulk tx, starts with
#define DISPLAY_COLOR_COMMAND 0x00
__xdata char bulkNodePacket[4] = { DISPLAY_COLOR_COMMAND, 0, 0, 0 };

//Limits the scann result to 63B to avoid having to send two result USB packet
//See usb_20.pdf #8.5.3.2
#define MAX_SCANN_LENGTH 63
#define CMD_SINGLE_TX 255

static char scannLength;

static bool contCarrier=false;
static bool needAck = true;

static volatile unsigned char mode = MODE_LEGACY;

void main()
{
  CKCON = 2;

  mode = MODE_LEGACY;

  checkBootPin();

  //Init the chip ID
  initId();
  //Init the led and set the leds until the usb is not ready
#ifndef CRPA
  ledInit(CR_LED_RED, CR_LED_GREEN);
#else
  ledInit(CRPA_LED_RED, CRPA_LED_GREEN);
#endif
  ledSet(LED_GREEN | LED_RED, true);

  // Initialise the radio
#ifdef CRPA
    // Enable LNA (PA RX)
    P0DIR &= ~(1<<CRPA_PA_RXEN);
    P0 |= (1<<CRPA_PA_RXEN);
#endif
  radioInit(RADIO_MODE_PTX);
#ifdef PPM_JOYSTICK
  // Initialise the PPM acquisition
  ppmInit();
#endif //PPM_JOYSTICK
  // Initialise and connect the USB
  usbInit();

  //Globally activate the interruptions
  IEN0 |= 0x80;

  //Wait for the USB to be addressed
  while (usbGetState() != ADDRESS);

  //Reset the LEDs
  ledSet(LED_GREEN | LED_RED, false);

  //Wait for the USB to be ready
  while (usbGetState() != CONFIGURED);

  //Activate OUT1
  OUT1BC=0xFF;

  while(1)
  {
    if (mode == MODE_LEGACY)
    {
      // Run legacy mode
      legacyRun();
    }
    else if (mode == MODE_CMD)
    {
      // Run cmd mode
      cmdRun();
    }
    else if (mode == MODE_PRX)
    {
      // Run PRX mode
      prxRun();
    }

    //USB vendor setup handling
    if(usbIsVendorSetup())
      handleUsbVendorSetup();
  }
}

//Handles vendor control messages and ack them
void handleUsbVendorSetup()
{
  __xdata struct controllStruct *setup = usbGetSetupPacket();

  //The vendor control messages are valide only when the device is configured
  if (usbGetState() >= CONFIGURED)
  {
    if(setup->request == LAUNCH_BOOTLOADER)
    {
      //Ack the launch request
      usbAckSetup();

      launchBootloader();
      //Will never come back ...

      return;
    }
    else if(setup->request == SET_RADIO_CHANNEL)
    {
      radioSetChannel(setup->value);

      usbAckSetup();
      return;
    }
    else if(setup->request == SET_DATA_RATE)
    {
      radioSetDataRate(setup->value);

      usbAckSetup();
      return;
    }
    else if(setup->request == SET_RADIO_ADDRESS)
    {
      if((setup->length>5)||(setup->length<3))
      {
        usbDismissSetup();
        return;
      }
      
      //Arm and wait for the out transaction
      OUT0BC = BCDUMMY;
      while (EP0CS & OUTBSY);

      //Set address of the pipe given by setup's index
      radioSetAddress(OUT0BUF);

      //Ack the setup phase
      usbAckSetup();
      return;
    }
    else if(setup->request == SET_RADIO_POWER)
    {
      radioSetPower(setup->value);

      usbAckSetup();
      return;
    }
    else if(setup->request == SET_RADIO_ARD)
    {
      radioSetArd(setup->value);

      usbAckSetup();
      return;
    }
    else if(setup->request == SET_RADIO_ARC)
    {
      radioSetArc(setup->value);

      usbAckSetup();
      return;
    }
    else if(setup->request == SET_CONT_CARRIER)
    {
      radioSetContCarrier((setup->value)?true:false);
      contCarrier = (setup->value)?true:false;

      ledTimeout = -1;
      ledSet(LED_RED, (setup->value)?true:false);

      usbAckSetup();
      return;
    }
    else if(setup->request == ACK_ENABLE)
    {
        needAck = (setup->value)?true:false;

        usbAckSetup();
        return;
    }

    /*
       GENERIC CONTROL FUNCTIONS
       New functions to allow low-level access to control registers
       that are not needed for CrazyFlie use but might be for other custom apps
    */
    else if(setup->request == SHOCKBURST)
    {
      char val=setup->value;
      if(val>6) val=6;
      if(val<0) val=0;
         
      radioShockburstPipes(val);
      usbAckSetup();
      return;
    }
    else if(setup->request == CRC)
    {
      if(setup->value == 1) {
        radioSetCRC(true);
      } else {
        radioSetCRC(false);
      }
      usbAckSetup();
      return;
    }
    else if(setup->request == CRC_LEN)
    {
      if(setup->value == 1) {
        radioSetCRCLen(0);
      } else {
        radioSetCRCLen(1);
      }
      usbAckSetup();
      return;
    }
    else if(setup->request == ADDR_LEN)
    {
      radioSetAddrLen(setup->value);
      usbAckSetup();
      return;
    }
    else if(setup->request == EN_RX_PIPES)
    {
      radioEnableRxPipe(setup->value);
      usbAckSetup();
      return;
    }
    else if(setup->request == DISABLE_RETRY)
    {
      radioDisableRetry();
      usbAckSetup();
      return;
    }
    else if(setup->request == DYNPD)
    {
      char val=setup->value;
      // FIXME hack - do all 6 pipes the same
      int x;
      for(x=0;x<7;x++)
      {
        // disable dynamic payload AND set the new payload len
        radioRxDynPayload(x, false);
        radioRxPayloadLen(x, val);
      }
      usbAckSetup();
      return;
    }
    else if(setup->request == EN_DPL)
    {
      radioTxDynPayload(true);
      return;
    }
    else if(setup->request == EN_ACK_PAY)
    {
      if(setup->value==1) radioPayloadAck(true);
      else  radioPayloadAck(false);
      return;
    }
    else if(setup->request == EN_DYN_ACK)
    {
      if(setup->value==1) radioPayloadAck(true);
      else  radioPayloadAck(false);
      return;
    }
    /*
       END GENERIC CONTROL FUNCTIONS
    */

    else if(setup->request == CHANNEL_SCANN && setup->requestType == 0x40)
    {
      int i;
      char rlen;
      char status;
      char inc = 1;
      unsigned char start, stop;
      scannLength = 0;

      if(setup->length < 1)
      {
        usbDismissSetup();
        return;
      }

      //Start and stop channels
      start = setup->value;
      stop = (setup->index>125)?125:setup->index;

      if (radioGetDataRate() == DATA_RATE_2M)
        inc = 2; //2M channel are 2MHz wide

      //Arm and wait for the out transaction
      OUT0BC = BCDUMMY;
      while (EP0CS & OUTBSY);

      memcpy(tbuffer, OUT0BUF, setup->length);
      for (i=start; i<stop+1 && scannLength<MAX_SCANN_LENGTH; i+=inc)
      {
        radioSetChannel(i);
        status = radioSendPacket(tbuffer, setup->length, rbuffer, &rlen);

        if (status)
          IN0BUF[scannLength++] = i;

        ledTimeout = 2;
        ledSet(LED_GREEN | LED_RED, false);
        if(status)
          ledSet(LED_GREEN, true);
        else
          ledSet(LED_RED, true);
      }

      //Ack the setup phase
      usbAckSetup();
      return;
    }
    else if(setup->request == CHANNEL_SCANN && setup->requestType == 0xC0)
    {
      //IN0BUF already contains the right data
      //(if a scann has been launched before ...)
      IN0BC = (setup->length>scannLength)?scannLength:setup->length;
      while (EP0CS & INBSY);

      //Ack the setup phase
      usbAckSetup();
      return;
    }
    else if(setup->request == SET_MODE && setup->requestType == 0x40)
    {
      mode = setup->value;
      if (mode == MODE_PRX)
      {
        radioSetMode(RADIO_MODE_PRX);
      }
      else
      {
        radioSetMode(RADIO_MODE_PTX);
      }

      usbAckSetup();
      return;
    }
    else if (setup->index == 0x0004 && setup->request == MSFT_ID_FEATURE_DESCRIPTOR)
    {
      usbHandleMsftFeatureIdDescriptor();
      return;
    }
  }

  //Stall in error if nothing executed!
  usbDismissSetup();
}

void checkBootPin()
{
  int i;
  void (*bootloader)() = (void (*)())0x7800;

  // Detect hard short to GCC
  for (i=0; i<200; i++) {
    if ((P0 & (1<<5)) == 0) {
      return;
    }
  }

  //Deactivate the interruptions
  IEN0 = 0x00;

  //Reset memory wait-state to default
  CKCON = 1;

  //Call the bootloader
  bootloader();
}

// De-init all the peripherical,
// and launch the Nordic USB bootloader located @ 0x7800
void launchBootloader()
{
  void (*bootloader)() = (void (*)())0x7800;

  //Deactivate the interruptions
  IEN0 = 0x00;

  //Deinitialise the USB
  usbDeinit();

  //Reset memory wait-state to default
  CKCON = 1;

  //Deinitialise the radio
  radioDeinit();

  //Call the bootloader
  bootloader();
}

/* 'Legacy' pre-1.0 protocol, handles only radio packets and require the
 *  computer to ping-pong sending and receiving
 */
void legacyRun()
{
  char status;
  char tlen;  //Transmit length
  char rlen;  //Received packet length
  uint8_t ack;
  char cmdPtr;

  //Send a packet if something is received on the USB
  if (!(OUT1CS&EPBSY) && !contCarrier)
  {
    //Fetch the USB data size. Limit it to 64
    tlen = OUT1BC;
    if (tlen>64) tlen=64;

    // copy data
    memcpy(tbuffer, OUT1BUF, tlen);

    //reactivate OUT1
    OUT1BC=BCDUMMY;

    cmdPtr = 0;
    rlen = 0;
    status = 0;

    if (true)
    {
      // Limit data size to 32
      //if (tlen>32) tlen=32;

      //status = radioSendPacket(tbuffer, tlen, rbuffer, &rlen);
      // new
      // ADDR_HIGH, INDEX R G B, INDEX R G B...
      bulkAddress[3] = tbuffer[cmdPtr++]; //address high
      if(bulkAddress[3] == CMD_SINGLE_TX){
        // Skip bulk tx if address is CMD_SINGLE_TX
        status = radioSendPacket(&tbuffer[cmdPtr], tlen-cmdPtr, rbuffer, &rlen);
      } else {
        // consume packet and send out to node
        while(cmdPtr < tlen){
          bulkAddress[4] = tbuffer[cmdPtr++];
          if(bulkAddress[4] != 0xFF) {
            // IF INDEX == 255, skip it
            radioSetAddress(bulkAddress);
            // bulk node packet includes DISPLAY_COLOR command when declared
            bulkNodePacket[1] = tbuffer[cmdPtr++]; //R
            bulkNodePacket[2] = tbuffer[cmdPtr++]; //G
            bulkNodePacket[3] = tbuffer[cmdPtr++]; //B
            if(needAck){
              status = radioSendPacket(bulkNodePacket, 4, rbuffer, &rlen);
            }else{
              radioSendPacketNoAck(bulkNodePacket, 4);
            }
          }else{
            cmdPtr += 3;
          }
        }
      }

      //Set the Green LED on success and the Red one on failure
      //The SOF interrupt decrement ledTimeout and will reset the LEDs when it
      //reaches 0
      ledTimeout = 2;
      ledSet(LED_GREEN | LED_RED, false);
      if(status)
        ledSet(LED_GREEN, true);
      else
        ledSet(LED_RED, true);

      //Prepare the USB answer, state and ack data
      ack=status?1:0;
      if (ack)
      {
      if (radioGetRpd()) ack |= 0x02;
      ack |= radioGetTxRetry()<<4;
      }

      //Deactivate the USB IN
      IN1CS = 0x02;

      IN1BUF[0]=ack;
      if(!(status&BIT_TX_DS)) rlen=0;
      // hack truncate rlen
      if (rlen>32) rlen=32;
      memcpy(IN1BUF+1, rbuffer, rlen);
      //Activate the IN EP with length+status
      IN1BC = rlen+1;
    }
  }
}

/* Command mode, the bulk usb packets contains both data and configuration in a
 * command string. The host can, and should, run TX and RX in different threads.
 */
void sendError(unsigned char code, unsigned char param, unsigned char pos)
{
  //Wait for IN1 to become free
  while(IN1CS&EPBSY);

  //Return an error
  IN1BUF[0] = CMD_ERROR;
  IN1BUF[1] = code;
  IN1BUF[2] = param;
  IN1BUF[3] = pos;

  IN1BC = 4;
}

__xdata char rpbuffer[64];

void cmdRun()
{
  char status;
  char tlen;  //Transmit length
  char cmdPtr;
  char resPtr;
  unsigned char id;
  unsigned char plen;
  char rlen;  //Received packet length
  uint8_t ack;
  unsigned char cmd;

  if (!(OUT1CS&EPBSY) && !contCarrier) {
    //Fetch the USB data size, should not be higher than 64
    tlen = OUT1BC;
    if (tlen>64) tlen=64;

    //Get the command string in a ram buffer
    memcpy(tbuffer, OUT1BUF, tlen);

    //And re-activate the out EP
    OUT1BC=BCDUMMY;

    // Run commands!
    cmdPtr = 0;
    resPtr = 0;
    while (cmdPtr < tlen) {
      cmd = tbuffer[cmdPtr++];

      switch (cmd) {
        case CMD_PACKET:
          if ((tlen-cmdPtr)<3) {
            sendError(ERROR_MALFORMED_CMD, cmd, cmdPtr);
            cmdPtr = tlen;
            break;
          }
          id = tbuffer[cmdPtr++];
          plen = tbuffer[cmdPtr++];
          if ((tlen-(cmdPtr+plen))<0) {
            sendError(ERROR_MALFORMED_CMD, cmd, cmdPtr);
            cmdPtr = tlen;
            break;
          }

          // old
          status = radioSendPacket(&tbuffer[cmdPtr], plen,
                                   rpbuffer, &rlen);
          cmdPtr += plen;

          // new
          // ADDR_HIGH, INDEX R G B, INDEX R G B...
//          bulkAddress[3] = tbuffer[cmdPtr++]; //address high
//          plen--;
//          // consume packet and send out to node
//          while(plen > 0){
//            bulkAddress[4] = tbuffer[cmdPtr++];
//            if(bulkAddress[4] != 0xFF) {
//              // IF INDEX == 255, skip it
//              radioSetAddress(bulkAddress);
//              // bulk node packet includes DISPLAY_COLOR command when declared
//              bulkNodePacket[1] = tbuffer[cmdPtr++]; //R
//              bulkNodePacket[2] = tbuffer[cmdPtr++]; //G
//              bulkNodePacket[3] = tbuffer[cmdPtr++]; //B
//              status = radioSendPacket(bulkNodePacket, 4, rpbuffer, &rlen);
//            }else{
//              cmdPtr += 3;
//            }
//            plen -= 4;
//          }
//          cmdPtr++;

          ledTimeout = 2;
          ledSet(LED_GREEN | LED_RED, false);
          if(status)
            ledSet(LED_GREEN, true);
          else
            ledSet(LED_RED, true);

          //Check if there is enough place in rbuffer, flush it if not
          if ((resPtr+rlen+4)>64) {
            //Wait for IN1 to become free
            while(IN1CS&EPBSY);

            memcpy(IN1BUF, rbuffer, resPtr);
            IN1BC = resPtr;
            resPtr = 0;
          }

          //Prepare the USB answer, state and ack data
          ack=status?1:0;
          if (ack)
          {
            if (radioGetRpd()) ack |= 0x02;
              ack |= radioGetTxRetry()<<4;
          }

          if(!(status&BIT_TX_DS)) rlen=0;

          rbuffer[resPtr] = 0;
          rbuffer[resPtr+1] = id;
          rbuffer[resPtr+2] =ack;
          rbuffer[resPtr+3] = rlen;
          memcpy(&rbuffer[resPtr+4], rpbuffer, rlen);

          resPtr += rlen+4;

          break;
        case SET_RADIO_CHANNEL:
          if (((tlen-cmdPtr)<1) || (tbuffer[cmdPtr]>125)) {
            sendError(ERROR_MALFORMED_CMD, cmd, cmdPtr);
            cmdPtr = tlen;
            break;
          }

          radioSetChannel(tbuffer[cmdPtr++]);

          break;
        case SET_DATA_RATE:
          if (((tlen-cmdPtr)<1) || (tbuffer[cmdPtr]>3)) {
            sendError(ERROR_MALFORMED_CMD, cmd, cmdPtr);
            cmdPtr = tlen;
            break;
          }

          radioSetDataRate(tbuffer[cmdPtr++]);

          break;
        default:
          sendError(ERROR_UNKNOWN_CMD, cmd,  cmdPtr);
          break;
      }
    }

    // Send data that are still in the TX buffer
    if (resPtr != 0) {
      //Wait for IN1 to become free
      while(IN1CS&EPBSY);

      memcpy(IN1BUF, rbuffer, resPtr);
      IN1BC = resPtr;
      resPtr = 0;
    }
  }
}

/* PRX (Primary receiver mode). The radio will listen to incoming packets and send those to
 * the PC. Packets from the PC will be put in the acknowledgment queue.
 */
void prxRun()
{
  char tlen;  //Transmit length

  if (!radioIsRxEmpty())
  {
    ledTimeout = 2;
    ledSet(LED_GREEN, true);
    IN1BC = radioRxPacket(IN1BUF);
  }
  //Send a packet if something is received on the USB
  if (!(OUT1CS&EPBSY) && !contCarrier)
  {
    //Deactivate the USB IN
    IN1CS = 0x02;
    //Fetch the USB data size. Limit it to 32
    tlen = OUT1BC;
    if (tlen>32) tlen=32;
    radioAckPacket(0, OUT1BUF, tlen);
    //reactivate OUT1
    OUT1BC=BCDUMMY;
  }
}
