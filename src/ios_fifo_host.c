//*****************************************************************************
//
//! @file ios_fifo_host.c
//!
//! @brief Example host used for demonstrating the use of the IOS FIFO.
//!
//! Purpose: This host component runs on one EVB and is used in conjunction with
//! the companion slave example, ios_fifo, which runs on a second EVB.
//!
//! The host example uses the ITM SWO to let the user know progress and
//! status of the demonstration.  The SWO is configured at 1M baud.
//! The ios_fifo example has no print output.
//!
//! This example implements the host part of a protocol for data exchange with
//! an Apollo IO Slave (IOS).  The host sends one byte commands on SPI/I2C by
//! writing to offset 0x80.
//!
//! The command is issued by the host to Start/Stop Data accumulation, and also
//! to acknowledge read-complete of a block of data.
//!
//! On the IOS side, once it is asked to start accumulating data (using START
//! command), two CTimer based events emulate sensors sending data to IOS.
//! When IOS has some data for host, it implements a state machine,
//! synchronizing with the host.
//!
//! The IOS interrupts the host to indicate data availability. The host then
//! reads the available data (as indicated by FIFOCTR) by READing using IOS FIFO
//! (at address 0x7F).  The IOS keeps accumulating any new data coming in the
//! background.
//!
//! Host sends an acknowledgement to IOS once it has finished reading a block
//! of data initiated by IOS (partitally or complete). IOS interrupts the host
//! again if and when it has more data for the host to read, and the cycle
//! repeats - till host indicates that it is no longer interested in receiving
//! data by sending STOP command.
//!
//! Additional Information:
//! In order to run this example, a slave device (e.g. a second EVB) must be set
//! up to run the companion example, ios_fifo.  The two boards can be connected
//! using fly leads between the two boards as follows.
//!
//! @verbatim
//! Pin connections for the I/O Master board to the I/O Slave board.
//! SPI:
//!     HOST (ios_fifo_host)                    SLAVE (ios_fifo)
//!     --------------------                    ----------------
//!     GPIO[40] GPIO Interrupt (slave to host) GPIO[4]  GPIO interrupt
//!     GPIO[5]  IOM0 SPI SCK                   GPIO[0]  IOS SPI SCK
//!     GPIO[7]  IOM0 SPI MOSI                  GPIO[1]  IOS SPI MOSI
//!     GPIO[6]  IOM0 SPI MISO                  GPIO[2]  IOS SPI MISO
//!     GPIO[11] IOM0 SPI nCE                   GPIO[3]  IOS SPI nCE
//!     GND                                     GND
//! @endverbatim
//
//*****************************************************************************

//*****************************************************************************
//
// Copyright (c) 2020, Ambiq Micro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// Third party software included in this distribution is subject to the
// additional license terms as defined in the /docs/licenses directory.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// This is part of revision 2.4.2 of the AmbiqSuite Development Package.
//
//*****************************************************************************

#include <stdint.h>
#include <stdbool.h>
#include "am_mcu_apollo.h"
#include "am_bsp.h"
#include "am_util.h"

#define     IOM_MODULE          0
#define     USE_SPI             1   // 0 = I2C, 1 = SPI
// How much data to read from Slave before ending the test
#define     MAX_SIZE            10000

#define     XOR_BYTE            0
#define     EMPTY_BYTE          0xEE

typedef enum
{
    AM_IOSTEST_CMD_START_DATA    = 0,
    AM_IOSTEST_CMD_STOP_DATA     = 1,
    AM_IOSTEST_CMD_ACK_DATA      = 2,
} AM_IOSTEST_CMD_E;

#define IOSOFFSET_WRITE_INTEN       0xF8
#define IOSOFFSET_WRITE_INTCLR      0xFA
#define IOSOFFSET_WRITE_CMD         0x80
#define IOSOFFSET_READ_INTSTAT      0x79
#define IOSOFFSET_READ_FIFO         0x7F
#define IOSOFFSET_READ_FIFOCTR      0x7C

#define AM_IOSTEST_IOSTOHOST_DATAAVAIL_INTMASK  1

#define HANDSHAKE_PIN            40

//*****************************************************************************
//
// Configure GPIOs for communicating with a SPI fram
// TODO: This should ideally come from BSP to keep the code portable
//
//*****************************************************************************

//*****************************************************************************
//
// Global message buffer for the IO master.
//
//*****************************************************************************
#define AM_TEST_RCV_BUF_SIZE    1024 // Max Size we can receive is 1023
uint8_t g_pui8RcvBuf[AM_TEST_RCV_BUF_SIZE];
volatile uint32_t g_startIdx = 0;
volatile bool bIosInt = false;

void *g_IOMHandle;

//*****************************************************************************
//
// Configuration structure for the IO Master.
//
//*****************************************************************************
static am_hal_iom_config_t g_sIOMSpiConfig =
{
    .eInterfaceMode = AM_HAL_IOM_SPI_MODE,
//    .ui32ClockFreq = AM_HAL_IOM_12MHZ,
//    .ui32ClockFreq = AM_HAL_IOM_8MHZ,
//    .ui32ClockFreq = AM_HAL_IOM_6MHZ,
    .ui32ClockFreq = AM_HAL_IOM_4MHZ,
//    .ui32ClockFreq = AM_HAL_IOM_3MHZ,
//    .ui32ClockFreq = AM_HAL_IOM_2MHZ,
//    .ui32ClockFreq = AM_HAL_IOM_1_5MHZ,
//    .ui32ClockFreq = AM_HAL_IOM_1MHZ,
//    .ui32ClockFreq = AM_HAL_IOM_750KHZ,
//    .ui32ClockFreq = AM_HAL_IOM_500KHZ,
//    .ui32ClockFreq = AM_HAL_IOM_400KHZ,
//    .ui32ClockFreq = AM_HAL_IOM_375KHZ,
//    .ui32ClockFreq = AM_HAL_IOM_250KHZ,
//    .ui32ClockFreq = AM_HAL_IOM_100KHZ,
//    .ui32ClockFreq = AM_HAL_IOM_50KHZ,
//    .ui32ClockFreq = AM_HAL_IOM_10KHZ,
    .eSpiMode = AM_HAL_IOM_SPI_MODE_0,
};

#define MAX_SPI_SIZE    1023

const am_hal_gpio_pincfg_t g_AM_BSP_GPIO_HANDSHAKE =
{
    .uFuncSel       = AM_HAL_PIN_40_GPIO,
    .eDriveStrength = AM_HAL_GPIO_PIN_DRIVESTRENGTH_2MA,
    .eIntDir        = AM_HAL_GPIO_PIN_INTDIR_LO2HI,
    .eGPInput       = AM_HAL_GPIO_PIN_INPUT_ENABLE,
};


// ISR callback for the host IOINT
static void hostint_handler(void)
{
    bIosInt = true;
}

//*****************************************************************************
//
// Interrupt handler for the GPIO pins.
//
//*****************************************************************************
void am_gpio_isr(void)
{
    //
    // Read and clear the GPIO interrupt status.
    //
#if defined(AM_PART_APOLLO3P)
    AM_HAL_GPIO_MASKCREATE(GpioIntStatusMask);

    am_hal_gpio_interrupt_status_get(false, pGpioIntStatusMask);
    am_hal_gpio_interrupt_clear(pGpioIntStatusMask);
    am_hal_gpio_interrupt_service(pGpioIntStatusMask);
#elif defined(AM_PART_APOLLO3)
    uint64_t ui64Status;

    am_hal_gpio_interrupt_status_get(false, &ui64Status);
    am_hal_gpio_interrupt_clear(ui64Status);
    am_hal_gpio_interrupt_service(ui64Status);
#else
    #error Unknown device.
#endif
}

void iom_slave_read(uint32_t offset, uint32_t *pBuf, uint32_t size)
{
    am_hal_iom_transfer_t       Transaction;

    Transaction.ui32InstrLen    = 1;
    Transaction.ui32Instr       = offset;
    Transaction.eDirection      = AM_HAL_IOM_RX;
    Transaction.ui32NumBytes    = size;
    Transaction.pui32RxBuffer   = pBuf;
    Transaction.bContinue       = false;
    Transaction.ui8RepeatCount  = 0;
    Transaction.ui32PauseCondition = 0;
    Transaction.ui32StatusSetClr = 0;

    Transaction.uPeerInfo.ui32SpiChipSelect = AM_BSP_IOM0_CS_CHNL;

    am_hal_iom_blocking_transfer(g_IOMHandle, &Transaction);
}

void iom_slave_write(uint32_t offset, uint32_t *pBuf, uint32_t size)
{
    am_hal_iom_transfer_t       Transaction;

    Transaction.ui32InstrLen    = 1;
    Transaction.ui32Instr       = offset;
    Transaction.eDirection      = AM_HAL_IOM_TX;
    Transaction.ui32NumBytes    = size;
    Transaction.pui32TxBuffer   = pBuf;
    Transaction.bContinue       = false;
    Transaction.ui8RepeatCount  = 0;
    Transaction.ui32PauseCondition = 0;
    Transaction.ui32StatusSetClr = 0;

    Transaction.uPeerInfo.ui32SpiChipSelect = AM_BSP_IOM0_CS_CHNL;

    am_hal_iom_blocking_transfer(g_IOMHandle, &Transaction);
}

static void iom_set_up(uint32_t iomModule)
{
    uint32_t ioIntEnable = AM_IOSTEST_IOSTOHOST_DATAAVAIL_INTMASK;

    //
    // Initialize the IOM.
    //
    am_hal_iom_initialize(iomModule, &g_IOMHandle);

    am_hal_iom_power_ctrl(g_IOMHandle, AM_HAL_SYSCTRL_WAKE, false);


    //
    // Set the required configuration settings for the IOM.
    //
    am_hal_iom_configure(g_IOMHandle, &g_sIOMSpiConfig);

    //
    // Configure the IOM pins.
    //
    am_bsp_iom_pins_enable(iomModule, AM_HAL_IOM_SPI_MODE);


    //
    // Enable all the interrupts for the IOM module.
    //
    //am_hal_iom_InterruptEnable(g_IOMHandle, 0xFF);
    //am_hal_interrupt_enable(AM_HAL_INTERRUPT_IOMASTER0);

    //
    // Enable the IOM.
    //
    am_hal_iom_enable(g_IOMHandle);
    am_hal_gpio_pinconfig(HANDSHAKE_PIN, g_AM_BSP_GPIO_HANDSHAKE);

    AM_HAL_GPIO_MASKCREATE(GpioIntMask);
    // Set up the host IO interrupt
    am_hal_gpio_interrupt_clear( AM_HAL_GPIO_MASKBIT(pGpioIntMask, HANDSHAKE_PIN));
    // Register handler for IOS => IOM interrupt
    am_hal_gpio_interrupt_register(HANDSHAKE_PIN, hostint_handler);
    am_hal_gpio_interrupt_enable(AM_HAL_GPIO_MASKBIT(pGpioIntMask, HANDSHAKE_PIN));
    NVIC_EnableIRQ(GPIO_IRQn);

    // Set up IOCTL interrupts
    // IOS ==> IOM
    iom_slave_write(IOSOFFSET_WRITE_INTEN, &ioIntEnable, 1);
}

//*****************************************************************************
//
// Main function.
//
//*****************************************************************************
int main(void)
{
    uint32_t iom = IOM_MODULE;
    bool bReadIosData = false;
    bool bDone = false;
    uint32_t data;
    uint32_t maxSize = MAX_SPI_SIZE;

    am_hal_clkgen_control(AM_HAL_CLKGEN_CONTROL_SYSCLK_MAX, 0);

    //
    // Set the default cache configuration
    //
    am_hal_cachectrl_config(&am_hal_cachectrl_defaults);
    am_hal_cachectrl_enable();

    //
    // Configure the board for low power operation.
    //
    am_bsp_low_power_init();

    //
    // Enable the ITM print interface.
    //
    am_bsp_itm_printf_enable();

    //
    // Clear the terminal and print the banner.
    //
    am_util_stdio_terminal_clear();
    am_util_stdio_printf("IOS Test Host: Waiting for at least %d bytes from the slave.", MAX_SIZE);


    //
    // Allow time for all printing to finish.
    //
    am_util_delay_ms(10);

    //
    // Enable Interrupts.
    //
    am_hal_interrupt_master_enable();

    //
    // Set up the IOM
    //
    iom_set_up(iom);

    // Make sure the print is complete
    am_util_delay_ms(100);

    // Send the START
    data = AM_IOSTEST_CMD_START_DATA;
    iom_slave_write(IOSOFFSET_WRITE_CMD, &data, 1);

    //
    // Loop forever.
    //
    while ( !bDone )
    {
        //
        // Disable interrupt while we decide whether we're going to sleep.
        //
        uint32_t ui32IntStatus = am_hal_interrupt_master_disable();

        if ( bIosInt == true )
        {
            //
            // Enable interrupts
            //
            am_hal_interrupt_master_set(ui32IntStatus);
            bIosInt = false;
            // Read & Clear the IOINT status
            iom_slave_read(IOSOFFSET_READ_INTSTAT, &data, 1);
            // We need to clear the bit by writing to IOS
            if ( data & AM_IOSTEST_IOSTOHOST_DATAAVAIL_INTMASK )
            {
                data = AM_IOSTEST_IOSTOHOST_DATAAVAIL_INTMASK;
                iom_slave_write(IOSOFFSET_WRITE_INTCLR, &data, 1);
                // Set bReadIosData
                bReadIosData = true;
            }
            if ( bReadIosData )
            {
                uint32_t iosSize = 0;

                bReadIosData = false;

                // Read the Data Size
                iom_slave_read(IOSOFFSET_READ_FIFOCTR, &iosSize, 2);
                iosSize = (iosSize > maxSize)? maxSize: iosSize;

                // Read the data
                iom_slave_read(IOSOFFSET_READ_FIFO,
                    (uint32_t *)g_pui8RcvBuf, iosSize);

                // Send the ACK/STOP
                data = AM_IOSTEST_CMD_ACK_DATA;

                if ( g_startIdx >= MAX_SIZE )
                {
                    bDone = true;
                    data = AM_IOSTEST_CMD_STOP_DATA;
                }
                iom_slave_write(IOSOFFSET_WRITE_CMD, &data, 1);
            }
        }
        else
        {
            am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_DEEP);
            //
            // Enable interrupts
            //
            am_hal_interrupt_master_set(ui32IntStatus);
        }
    }
    am_util_stdio_printf("\nTest Done - Total Received = =%d\n", g_startIdx);
    while (1);
}


