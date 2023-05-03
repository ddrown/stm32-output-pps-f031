# stm32 output PPS

## This project provides a high accuracy PPS suitable for a stratum 2+ NTP server.

It can be expensive to get GPS antennas in datacenters, especially if you don't own your own datacenter building. As an alternative to a stratum 1 NTP server using GPS, a stratum 2 NTP server can benefit from a local frequency standard to increase its accuracy.

This project has three pieces:
* hardware - Raspberry Pi Hat with a stm32f031 microcontroller+TCXO (hardware/ directory)
* firmware - Outputs a PPS based on the TCXO's frequency, controlled via i2c (top directory)
* clients - Talks to the hat via i2c commands (clients/ directory)

The TCXO's frequency vs temperature curve is measured, stored in flash, and applied to the PPS in order to increase frequency accuracy.
![Frequency vs Temperature](https://blog.dan.drown.org/content/images/2017/02/ch2-freq-vs-temp-2-1.png)

Chrony can be configured to accept the PPS as a local frequency reference, combining it with normal NTP servers to improve time accuracy: https://blog.dan.drown.org/strat-2-tcxo/

The hat also contains an RTC with sub-second precision. Usually i2c RTCs will only tell you time to the nearest second, so you have to poll them to find out when the second changes. The RTC runs off the 32khz crystal, but can be measured in hardware against the TCXO with `clients/rtc compare`

Timers used
* TIM2 - main 32 bit timer, 1 channel is setup as PWM/PPS output (HAT_CH1 or HAT_CH2)
* TIM14 - 16 bit timer, measures RTC (32khz LSE) vs TCXO on channel 1

Hat pins connected to SBC/stm32 micro:
* SBC pin - stm32 use
* pin3 - I2C SDA
* pin5 - I2C SCL
* pin11 - stm32 rst (via jumper)
* pin12 - PPS output HAT_CH1
* pin13 - stm32 BOOT0 (via jumper)
* pin18 - SWD dio (via jumper)
* pin22 - SWD clk (via jumper)
* pin33 - PPS output HAT_CH2

firmware code:

* Src/system_stm32f0xx.c - bootup, start clocks
* Src/main.c - init peripherals, main loop
* Src/stm32f0xx_hal_msp.c - init irq, init gpio routing, init clocks, etc
* Src/stm32f0xx_it.c - irq handlers (mostly call into HAL)
* Src/adc.c - measure temperature, vcc, and rtc battery
* Src/flash.c - store TCXO parameters in flash
* Src/i2c_slave.c - i2c interface to talk to SBC
* Src/timer.c - manages TIM2, TIM14, and RTC
* Src/uart.c - debug uart interface
* Drivers/STM32F0xx_HAL_Driver - stm32 Hardware Abstraction Layer (HAL)

SBC client code:

* clients/timestamps-i2c.c - compare SBC system time vs TCXO, request via i2c
* clients/input-capture-i2c.c - compare SBC system frequency vs TCXO, via SBC output pwm
* clients/pi-pwm-setup.c - wiringPi setup SBC output pwm
* clients/pi-pwm-disable.c - wiringPi disable SBC output pwm
* clients/odroid-c2-setup - sysfs setup SBC output pwm
* clients/rtc.c - read/set/sync/manage rtc
* clients/set-calibration-data.c - get/set TCXO calibration data in flash
* clients/ds3231.c - DS3231 RTC i2c client
* clients/pcf2129.c - PCF2129 RTC i2c client
* clients/calibration.c - measure RTC vs TCXO
* clients/dump-adc.c - test comparing floating vs fixed point math

SBC library code:

* clients/adc_calc.c - read ADC data over i2c
* clients/aging.c - estimate TCXO frequency from aging effects
* clients/avg.c - calculate averages
* clients/float.c - convert between native float and i2c compatible
* clients/i2c.c - i2c interface functions
* clients/i2c_registers.c - read stm32's exported data via i2c
* clients/rtc_data.c - convert between RTC data and system time
* clients/tempcomp.c - calculate TCXO's temperature compensation
* clients/timespec.c - struct timespec utilities
* clients/vref_calc.c - adc voltage calculations

Programming the stm32 can be done via openocd on the connected SBC.

Example openocd configuration:
* openocd.cfg - raspberry pi & stlink example
* openocd-odroid.cfg - gpio sysfs on odroid-c2 SBC
* openocd-stm32mp1.cfg - gpio sysfs on stm32mp157a-dk1 SBC
