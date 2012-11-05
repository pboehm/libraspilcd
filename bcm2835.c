// bcm2835.c
// C and C++ support for Broadcom BCM 2835 as used in Raspberry Pi
// http://elinux.org/RPi_Low-level_peripherals
// http://www.raspberrypi.org/wp-content/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
//
// Author: Mike McCauley (mikem@open.com.au)
// Copyright (C) 2011 Mike McCauley
// $Id: bcm2835.c,v 1.4 2012/07/16 23:57:59 mikem Exp mikem $

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bcm2835.h"

// This define enables a little test program (by default a blinking output on pin RPI_GPIO_PIN_11)
// You can do some safe, non-destructive testing on any platform with:
// gcc bcm2835.c -D BCM2835_TEST
// ./a.out
//#define BCM2835_TEST

// Locals to hold pointers to the hardware
static volatile uint32_t *gpio = MAP_FAILED;
static volatile uint32_t *pwm  = MAP_FAILED;
static volatile uint32_t *clk  = MAP_FAILED;
static volatile uint32_t *pads = MAP_FAILED;
static volatile uint32_t *spi0 = MAP_FAILED;

static int memfd = -1;

// This define allows us to test on hardware other than RPi.
// It prevents access to the kernel memory, and does not do any peripheral access
// Instead it prints out what it _would_ do if debug were 0
static uint8_t debug = 0;


//
// Low level register access functions
//

void  bcm2835_set_debug(uint8_t d)
{
    debug = d;
}

// safe read from peripheral
uint32_t bcm2835_peri_read(volatile uint32_t* paddr)
{
    if (debug)
    {
        printf("bcm2835_peri_read  paddr %08X\n", (unsigned) paddr);
	return 0;
    }
    else
    {
	// Make sure we dont return the _last_ read which might get lost
	// if subsequent code changes to a different peripheral
	uint32_t ret = *paddr;
	uint32_t dummy = *paddr;
	return ret;
    }
}

// read from peripheral without the read barrier
uint32_t bcm2835_peri_read_nb(volatile uint32_t* paddr)
{
    if (debug)
    {
	printf("bcm2835_peri_read_nb  paddr %08X\n", (unsigned) paddr);
	return 0;
    }
    else
    {
	return *paddr;
    }
}

// safe write to peripheral
void bcm2835_peri_write(volatile uint32_t* paddr, uint32_t value)
{
    if (debug)
    {
	printf("bcm2835_peri_write paddr %08X, value %08X\n", (unsigned) paddr, value);
    }
    else
    {
	// Make sure we don't rely on the first write, which may get
	// lost if the previous access was to a different peripheral.
	*paddr = value;
	*paddr = value;
    }
}

// write to peripheral without the write barrier
void bcm2835_peri_write_nb(volatile uint32_t* paddr, uint32_t value)
{
    if (debug)
    {
	printf("bcm2835_peri_write_nb paddr %08X, value %08X\n",
               (unsigned) paddr, value);
    }
    else
    {
	*paddr = value;
    }
}

// Set/clear only the bits in value covered by the mask
void bcm2835_peri_set_bits(volatile uint32_t* paddr, uint32_t value, uint32_t mask)
{
    uint32_t v = bcm2835_peri_read(paddr);
    v = (v & ~mask) | (value & mask);
    bcm2835_peri_write(paddr, v);
}

//
// Low level convenience functions
//

// Function select
// pin is a BCM2835 GPIO pin number NOT RPi pin number
//      There are 6 control registers, each control the functions of a block
//      of 10 pins.
//      Each control register has 10 sets of 3 bits per GPIO pin:
//
//      000 = GPIO Pin X is an input
//      001 = GPIO Pin X is an output
//      100 = GPIO Pin X takes alternate function 0
//      101 = GPIO Pin X takes alternate function 1
//      110 = GPIO Pin X takes alternate function 2
//      111 = GPIO Pin X takes alternate function 3
//      011 = GPIO Pin X takes alternate function 4
//      010 = GPIO Pin X takes alternate function 5
//
// So the 3 bits for port X are:
//      X / 10 + ((X % 10) * 3)
void bcm2835_gpio_fsel(uint8_t pin, uint8_t mode)
{
    // Function selects are 10 pins per 32 bit word, 3 bits per pin
    volatile uint32_t* paddr = gpio + BCM2835_GPFSEL0/4 + (pin/10);
    uint8_t   shift = (pin % 10) * 3;
    uint32_t  mask = BCM2835_GPIO_FSEL_MASK << shift;
    uint32_t  value = mode << shift;
    bcm2835_peri_set_bits(paddr, value, mask);
}

// Set output pin
void bcm2835_gpio_set(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPSET0/4 + pin/32;
    uint8_t shift = pin % 32;
    bcm2835_peri_write(paddr, 1 << shift);
}

// Clear output pin
void bcm2835_gpio_clr(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPCLR0/4 + pin/32;
    uint8_t shift = pin % 32;
    bcm2835_peri_write(paddr, 1 << shift);
}

// Read input pin
uint8_t bcm2835_gpio_lev(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPLEV0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = bcm2835_peri_read(paddr);
    return (value & (1 << shift)) ? HIGH : LOW;
}

// See if an event detection bit is set
// Sigh cant support interrupts yet
uint8_t bcm2835_gpio_eds(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPEDS0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = bcm2835_peri_read(paddr);
    return (value & (1 << shift)) ? HIGH : LOW;
}

// Write a 1 to clear the bit in EDS
void bcm2835_gpio_set_eds(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPEDS0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_write(paddr, value);
}

// Rising edge detect enable
void bcm2835_gpio_ren(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPREN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, value, value);
}
void bcm2835_gpio_clr_ren(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPREN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, 0, value);
}

// Falling edge detect enable
void bcm2835_gpio_fen(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPFEN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, value, value);
}
void bcm2835_gpio_clr_fen(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPFEN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, 0, value);
}

// High detect enable
void bcm2835_gpio_hen(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPHEN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, value, value);
}
void bcm2835_gpio_clr_hen(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPHEN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, 0, value);
}

// Low detect enable
void bcm2835_gpio_len(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPLEN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, value, value);
}
void bcm2835_gpio_clr_len(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPLEN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, 0, value);
}

// Async rising edge detect enable
void bcm2835_gpio_aren(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPAREN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, value, value);
}
void bcm2835_gpio_clr_aren(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPAREN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, 0, value);
}

// Async falling edge detect enable
void bcm2835_gpio_afen(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPAFEN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, value, value);
}
void bcm2835_gpio_clr_afen(uint8_t pin)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPAFEN0/4 + pin/32;
    uint8_t shift = pin % 32;
    uint32_t value = 1 << shift;
    bcm2835_peri_set_bits(paddr, 0, value);
}

// Set pullup/down
void bcm2835_gpio_pud(uint8_t pud)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPPUD/4;
    bcm2835_peri_write(paddr, pud);
}

// Pullup/down clock
// Clocks the value of pud into the GPIO pin
void bcm2835_gpio_pudclk(uint8_t pin, uint8_t on)
{
    volatile uint32_t* paddr = gpio + BCM2835_GPPUDCLK0/4 + pin/32;
    uint8_t shift = pin % 32;
    bcm2835_peri_write(paddr, (on ? 1 : 0) << shift);
}

// Read GPIO pad behaviour for groups of GPIOs
uint32_t bcm2835_gpio_pad(uint8_t group)
{
    volatile uint32_t* paddr = pads + BCM2835_PADS_GPIO_0_27/4 + group*2;
    return bcm2835_peri_read(paddr);
}

// Set GPIO pad behaviour for groups of GPIOs
// powerup value for al pads is
// BCM2835_PAD_SLEW_RATE_UNLIMITED | BCM2835_PAD_HYSTERESIS_ENABLED | BCM2835_PAD_DRIVE_8mA
void bcm2835_gpio_set_pad(uint8_t group, uint32_t control)
{
    volatile uint32_t* paddr = pads + BCM2835_PADS_GPIO_0_27/4 + group*2;
    bcm2835_peri_write(paddr, control);
}

// Some convenient arduino like functions
// milliseconds
void bcm2835_delay(unsigned int millis)
{
  struct timespec sleeper;

  sleeper.tv_sec  = (time_t)(millis / 1000);
  sleeper.tv_nsec = (long)(millis % 1000) * 1000000;
  nanosleep(&sleeper, NULL);
}

// microseconds
void bcm2835_delayMicroseconds(unsigned int micros)
{
    struct timespec t0, t1;
    double t_us;
    // Calling nanosleep() takes at least 100-200 us, so use it for
    // long waits and use a busy wait on the hires timer for the rest.
    clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
    if (micros > 450)
    {
	t1.tv_sec = 0;
	t1.tv_nsec = 1000 * (long)(micros - 200);
	nanosleep(&t1, NULL);
    }

    while (1)
    {
	clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
	t_us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) * 1e-3;
	if (t_us >= micros)
	    break;
    }
}

//
// Higher level convenience functions
//

// Set the state of an output
void bcm2835_gpio_write(uint8_t pin, uint8_t on)
{
    if (on)
    {
	bcm2835_gpio_set(pin);
    }
    else
    {
	bcm2835_gpio_clr(pin);
    }
}

// Set the pullup/down resistor for a pin
//
// The GPIO Pull-up/down Clock Registers control the actuation of internal pull-downs on
// the respective GPIO pins. These registers must be used in conjunction with the GPPUD
// register to effect GPIO Pull-up/down changes. The following sequence of events is
// required:
// 1. Write to GPPUD to set the required control signal (i.e. Pull-up or Pull-Down or neither
// to remove the current Pull-up/down)
// 2. Wait 150 cycles ? this provides the required set-up time for the control signal
// 3. Write to GPPUDCLK0/1 to clock the control signal into the GPIO pads you wish to
// modify ? NOTE only the pads which receive a clock will be modified, all others will
// retain their previous state.
// 4. Wait 150 cycles ? this provides the required hold time for the control signal
// 5. Write to GPPUD to remove the control signal
// 6. Write to GPPUDCLK0/1 to remove the clock
//
// RPi has P1-03 and P1-05 with 1k8 pullup resistor
void bcm2835_gpio_set_pud(uint8_t pin, uint8_t pud)
{
    bcm2835_gpio_pud(pud);
    delayMicroseconds(10);
    bcm2835_gpio_pudclk(pin, 1);
    delayMicroseconds(10);
    bcm2835_gpio_pud(BCM2835_GPIO_PUD_OFF);
    bcm2835_gpio_pudclk(pin, 0);
}

void bcm2835_spi_begin(void)
{
    // Set the SPI0 pins to the Alt 0 function to enable SPI0 access on them
    bcm2835_gpio_fsel(RPI_GPIO_P1_26, BCM2835_GPIO_FSEL_ALT0); // CE1
    bcm2835_gpio_fsel(RPI_GPIO_P1_24, BCM2835_GPIO_FSEL_ALT0); // CE0
    bcm2835_gpio_fsel(RPI_GPIO_P1_21, BCM2835_GPIO_FSEL_ALT0); // MISO
    bcm2835_gpio_fsel(RPI_GPIO_P1_19, BCM2835_GPIO_FSEL_ALT0); // MOSI
    bcm2835_gpio_fsel(RPI_GPIO_P1_23, BCM2835_GPIO_FSEL_ALT0); // CLK

    // Set the SPI CS register to the some sensible defaults
    volatile uint32_t* paddr = spi0 + BCM2835_SPI0_CS/4;
    bcm2835_peri_write(paddr, 0); // All 0s

    // Clear TX and RX fifos
    bcm2835_peri_write_nb(paddr, BCM2835_SPI0_CS_CLEAR);
}

void bcm2835_spi_end(void)
{
    // Set all the SPI0 pins back to input
    bcm2835_gpio_fsel(RPI_GPIO_P1_26, BCM2835_GPIO_FSEL_INPT); // CE1
    bcm2835_gpio_fsel(RPI_GPIO_P1_24, BCM2835_GPIO_FSEL_INPT); // CE0
    bcm2835_gpio_fsel(RPI_GPIO_P1_21, BCM2835_GPIO_FSEL_INPT); // MISO
    bcm2835_gpio_fsel(RPI_GPIO_P1_19, BCM2835_GPIO_FSEL_INPT); // MOSI
    bcm2835_gpio_fsel(RPI_GPIO_P1_23, BCM2835_GPIO_FSEL_INPT); // CLK
}

void bcm2835_spi_setBitOrder(uint8_t order)
{
    // BCM2835_SPI_BIT_ORDER_MSBFIRST is the only one suported by SPI0
}

// defaults to 0, which means a divider of 65536.
// The divisor must be a power of 2. Odd numbers
// rounded down. The maximum SPI clock rate is
// of the APB clock
void bcm2835_spi_setClockDivider(uint16_t divider)
{
    volatile uint32_t* paddr = spi0 + BCM2835_SPI0_CLK/4;
    bcm2835_peri_write(paddr, divider);
}

void bcm2835_spi_setDataMode(uint8_t mode)
{
    volatile uint32_t* paddr = spi0 + BCM2835_SPI0_CS/4;
    // Mask in the CPO and CPHA bits of CS
    bcm2835_peri_set_bits(paddr, mode << 2, BCM2835_SPI0_CS_CPOL | BCM2835_SPI0_CS_CPHA);
}

// Writes (and reads) a single byte to SPI
uint8_t bcm2835_spi_transfer(uint8_t value)
{
    volatile uint32_t* paddr = spi0 + BCM2835_SPI0_CS/4;
    volatile uint32_t* fifo = spi0 + BCM2835_SPI0_FIFO/4;

    // This is Polled transfer as per section 10.6.1
    // BUG ALERT: what happens if we get interupted in this section, and someone else
    // accesses a different peripheral?
    // Clear TX and RX fifos
    bcm2835_peri_set_bits(paddr, BCM2835_SPI0_CS_CLEAR, BCM2835_SPI0_CS_CLEAR);

    // Set TA = 1
    bcm2835_peri_set_bits(paddr, BCM2835_SPI0_CS_TA, BCM2835_SPI0_CS_TA);

    // Maybe wait for TXD
    while (!(bcm2835_peri_read(paddr) & BCM2835_SPI0_CS_TXD))
	delayMicroseconds(10);

    // Write to FIFO, no barrier
    bcm2835_peri_write_nb(fifo, value);

    // Wait for DONE to be set
    while (!(bcm2835_peri_read_nb(paddr) & BCM2835_SPI0_CS_DONE))
	delayMicroseconds(10);

    // Read any byte that was sent back by the slave while we sere sending to it
    uint32_t ret = bcm2835_peri_read_nb(fifo);

    // Set TA = 0, and also set the barrier
    bcm2835_peri_set_bits(paddr, 0, BCM2835_SPI0_CS_TA);

    return ret;
}

// Writes (and reads) an number of bytes to SPI
void bcm2835_spi_transfernb(char* tbuf, char* rbuf, uint32_t len)
{
    volatile uint32_t* paddr = spi0 + BCM2835_SPI0_CS/4;
    volatile uint32_t* fifo = spi0 + BCM2835_SPI0_FIFO/4;

    // This is Polled transfer as per section 10.6.1
    // BUG ALERT: what happens if we get interupted in this section, and someone else
    // accesses a different peripheral?

    // Clear TX and RX fifos
    bcm2835_peri_set_bits(paddr, BCM2835_SPI0_CS_CLEAR, BCM2835_SPI0_CS_CLEAR);

    // Set TA = 1
    bcm2835_peri_set_bits(paddr, BCM2835_SPI0_CS_TA, BCM2835_SPI0_CS_TA);

    uint32_t i;
    for (i = 0; i < len; i++)
    {
	// Maybe wait for TXD
	while (!(bcm2835_peri_read(paddr) & BCM2835_SPI0_CS_TXD))
	    delayMicroseconds(10);

	// Write to FIFO, no barrier
	bcm2835_peri_write_nb(fifo, tbuf[i]);

	// Wait for RXD
	while (!(bcm2835_peri_read(paddr) & BCM2835_SPI0_CS_RXD))
	    delayMicroseconds(10);

	// then read the data byte
	rbuf[i] = bcm2835_peri_read_nb(fifo);
    }
    // Wait for DONE to be set
    while (!(bcm2835_peri_read_nb(paddr) & BCM2835_SPI0_CS_DONE))
	delayMicroseconds(10);

    // Set TA = 0, and also set the barrier
    bcm2835_peri_set_bits(paddr, 0, BCM2835_SPI0_CS_TA);
}

// Writes (and reads) an number of bytes to SPI
// Read bytes are copied over onto the transmit buffer
void bcm2835_spi_transfern(char* buf, uint32_t len)
{
    bcm2835_spi_transfernb(buf, buf, len);
}

void bcm2835_spi_chipSelect(uint8_t cs)
{
    volatile uint32_t* paddr = spi0 + BCM2835_SPI0_CS/4;
    // Mask in the CS bits of CS
    bcm2835_peri_set_bits(paddr, cs, BCM2835_SPI0_CS_CS);
}

void bcm2835_spi_setChipSelectPolarity(uint8_t cs, uint8_t active)
{
    volatile uint32_t* paddr = spi0 + BCM2835_SPI0_CS/4;
    uint8_t shift = 21 + cs;
    // Mask in the appropriate CSPOLn bit
    bcm2835_peri_set_bits(paddr, active << shift, 1 << shift);
}

// Allocate page-aligned memory.
void *malloc_aligned(size_t size)
{
  void *mem;
  errno = posix_memalign(&mem, BCM2835_PAGE_SIZE, size);
  return (errno ? NULL : mem);
}

// Map 'size' bytes starting at 'off' in file 'fd' to memory.
// Return mapped address on success, MAP_FAILED otherwise.
// On error print message.
static void *mapmem(const char *msg, size_t size, int fd, off_t off)
{
    void *map = mmap(NULL, size, (PROT_READ | PROT_WRITE), MAP_SHARED, fd, off);
    if(MAP_FAILED == map){
	fprintf(stderr, "bcm2835_init: %s mmap failed: %s\n", msg, strerror(errno)) ;
    }
    return map;
}

static void unmapmem(void **pmem, size_t size)
{
    if(*pmem == MAP_FAILED) return;
    munmap(*pmem, size);
    *pmem = MAP_FAILED;
}

// Initialise this library.
int bcm2835_init(void)
{
    if (debug) {
	pads = (uint32_t*)BCM2835_GPIO_PADS;
	clk = (uint32_t*)BCM2835_CLOCK_BASE;
	gpio = (uint32_t*)BCM2835_GPIO_BASE;
	pwm = (uint32_t*)BCM2835_GPIO_PWM;
	spi0 = (uint32_t*)BCM2835_SPI0_BASE;
	return 1; // Success
    }
    int ok = 0;
    // Open the master /dev/memory device
    if ((memfd = open("/dev/mem", O_RDWR | O_SYNC) ) < 0) {
	fprintf(stderr, "bcm2835_init: Unable to open /dev/mem: %s\n",
		strerror(errno)) ;
	goto exit;
    }

    // GPIO:
    gpio = mapmem("gpio", BCM2835_BLOCK_SIZE, memfd, BCM2835_GPIO_BASE);
    if (gpio == MAP_FAILED) goto exit;

    // PWM
    pwm = mapmem("pwm", BCM2835_BLOCK_SIZE, memfd, BCM2835_GPIO_PWM);
    if (pwm == MAP_FAILED) goto exit;

    // Clock control (needed for PWM)
    clk = mapmem("clk", BCM2835_BLOCK_SIZE, memfd, BCM2835_CLOCK_BASE);
    if (clk == MAP_FAILED) goto exit;

    pads = mapmem("pads", BCM2835_BLOCK_SIZE, memfd, BCM2835_GPIO_PADS);
    if (pads == MAP_FAILED) goto exit;

    spi0 = mapmem("spi0", BCM2835_BLOCK_SIZE, memfd, BCM2835_SPI0_BASE);
    if (spi0 == MAP_FAILED) goto exit;

    ok = 1;
  exit:
    if(!ok){
	bcm2835_close();
    }
    return ok;
}

// Close this library and deallocate everything
int bcm2835_close(void)
{
    int ok = 1; // Success.
    if (debug) return ok;
    unmapmem((void**) &gpio, BCM2835_BLOCK_SIZE);
    unmapmem((void**) &pwm,  BCM2835_BLOCK_SIZE);
    unmapmem((void**) &clk,  BCM2835_BLOCK_SIZE);
    unmapmem((void**) &spi0, BCM2835_BLOCK_SIZE);
    if (memfd >= 0)
    {
	close(memfd);
	memfd = -1;
    }
    return ok;
}

#ifdef BCM2835_TEST
// this is a simple test program that prints out what it will do rather than
// actually doing it
int main(int argc, char **argv)
{
    // Be non-destructive
    bcm2835_set_debug(1);

    if (!bcm2835_init())
	return 1;

    // Configure some GPIO pins fo some testing
    // Set RPI pin P1-11 to be an output
    bcm2835_gpio_fsel(RPI_GPIO_P1_11, BCM2835_GPIO_FSEL_OUTP);
    // Set RPI pin P1-15 to be an input
    bcm2835_gpio_fsel(RPI_GPIO_P1_15, BCM2835_GPIO_FSEL_INPT);
    //  with a pullup
    bcm2835_gpio_set_pud(RPI_GPIO_P1_15, BCM2835_GPIO_PUD_UP);
    // And a low detect enable
    bcm2835_gpio_len(RPI_GPIO_P1_15);
    // and input hysteresis disabled on GPIOs 0 to 27
    bcm2835_gpio_set_pad(BCM2835_PAD_GROUP_GPIO_0_27, BCM2835_PAD_SLEW_RATE_UNLIMITED|BCM2835_PAD_DRIVE_8mA);

#if 1
    // Blink
    while (1)
    {
	// Turn it on
	bcm2835_gpio_write(RPI_GPIO_P1_11, HIGH);

	// wait a bit
	bcm2835_delay(500);

	// turn it off
	bcm2835_gpio_write(RPI_GPIO_P1_11, LOW);

	// wait a bit
	bcm2835_delay(500);
    }
#endif

#if 0
    // Read input
    while (1)
    {
	// Read some data
	uint8_t value = bcm2835_gpio_lev(RPI_GPIO_P1_15);
	printf("read from pin 15: %d\n", value);

	// wait a bit
	bcm2835_delay(500);
    }
#endif

#if 0
    // Look for a low event detection
    // eds will be set whenever pin 15 goes low
    while (1)
    {
	if (bcm2835_gpio_eds(RPI_GPIO_P1_15))
	{
	    // Now clear the eds flag by setting it to 1
	    bcm2835_gpio_set_eds(RPI_GPIO_P1_15);
	    printf("low event detect for pin 15\n");
	}

	// wait a bit
	bcm2835_delay(500);
    }
#endif

    if (!bcm2835_close())
	return 1;

    return 0;
}
#endif
