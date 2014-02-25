/*
 * Driver for Communicating with 433HMz Transmitters via Bitbanging GPIO
 * -> EG usable for ELRO / KAKU / ...
 * Based on: https://bitbucket.org/fuzzillogic/433mhzforarduino/wiki/Home
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include <linux/platform_device.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include "transmitter_data.h"

#define DEVICE_NAME "transmitter"

 ///<Offsets
#define GPIO_0_OFFSET 		0x44E07000	///< GPIO0 Offset in AM335x
#define GPIO_1_OFFSET 		0x4804C000	///< GPIO1 Offset in AM335x
#define GPIO_0_REG_SIZE		0x1000	///< GPIO0 register size in AM335x
#define GPIO_1_REG_SIZE		0x1000	///< GPIO1 register size in AM335x
#define CONFIG_MODULE		0x44E10800	///< Config module Offset in AM335x
#define CONFIG_MODULE_SIZE  0x2000	///< Config module register size in AM335x

#define GPIO_OE_OFFSET		0x134	//Determins input / output of gpio pins

#define	GPIO_BANK_SIZE 			32	///< GPIO Bank Size

#define	GPIO0_BANK_OFFSET		(0)	///< GPIO Bank Offset GPIO0
#define GPIO1_BANK_OFFSET		(GPIO0_BANK_OFFSET + GPIO_BANK_SIZE)	///< GPIO Bank Offset GPIO1
#define GPIO2_BANK_OFFSET		(GPIO1_BANK_OFFSET + GPIO_BANK_SIZE)	///< GPIO Bank Offset GPIO2
#define GPIO3_BANK_OFFSET		(GPIO2_BANK_OFFSET + GPIO_BANK_SIZE)	///< GPIO Bank Offset GPIO3

#define	GPIO_0(pin) 			(GPIO0_BANK_OFFSET + pin)	///< GPIO0 Pin number Macro
#define	GPIO_1(pin) 			(GPIO1_BANK_OFFSET + pin)	///< GPIO1 Pin number Macro
#define	GPIO_2(pin) 			(GPIO2_BANK_OFFSET + pin)	///< GPIO2 Pin number Macro
#define	GPIO_3(pin) 			(GPIO3_BANK_OFFSET + pin)	///< GPIO3 Pin number Macro

//MACROs To set gpio pins high / low
#define GPIO0 remap_gpio0	///< Address of GPIO 0
#define GPIO1 remap_gpio1	///< Address of GPIO 1
#define GPIO0_CLR GPIO0 + 0x190	///< MACRO to clear GPIO value
#define GPIO0_SET GPIO0 + 0x194	///< MACRO to set GPIO 0 value
#define GPIO0_GET GPIO0 + 0x138	///< MACRO to get GPIO 0 value
#define GPIO1_CLR GPIO1 + 0x190	///< MACRO to clear GPIO 1 value
#define GPIO1_SET GPIO1 + 0x194	///< MACRO to set GPIO 1 value

#define PIN_DATA	2 //data pin number

//MACROs to set GPIO
#define REG(addr) (*(volatile unsigned int *)(addr))	///< MACRO FUNCTION to write to addres
#define pin_high()  	 REG(GPIO1_SET) = (1<<PIN_DATA)	///< START ACQ HIGH
#define pin_low()   	 REG(GPIO1_CLR) = (1<<PIN_DATA)	///< START ACQ LOW

static const char this_device_name[] = DEVICE_NAME;
static void *remap_gpio1;
static void *remap_config_module;
static void transmit_code(unsigned long code, int periodusec, int repeats);

//static DEVICE_ATTR(node_name, S_IWUSR, NULL, func);

struct transmitter_dev_t {
	dev_t devt;		/* Char dev region (major,minor) */
	struct cdev *cdev;	/* Char dev related structure */
	struct class *class;	/* Udev / HAL related */
};

static struct transmitter_dev_t transmitter_dev;

static ssize_t transmitter_write(struct file *file,
			  const char __user *buf, size_t count, loff_t *ppos)
{

	transmit_data dataStruct;
	dataStruct.pin = 9;
	dataStruct.code = 0xFF;
	dataStruct.period_usec = 320;
	dataStruct.repeats = 3;

	sscanf(buf, "[%d] [%ld] [%d] [%d]",
					&dataStruct.pin, &dataStruct.code,
					&dataStruct.period_usec, &dataStruct.repeats);

	transmit_code(dataStruct.code, dataStruct.period_usec, dataStruct.repeats);
	return count;
}

static void transmit_code(unsigned long code, int periodusec, int repeats)
{
	int i;
	int j = 0;
	unsigned long dataBase4 = 0;

	pr_alert("TRANS: Sending 0x%lx (delay %d, repeats[%d])\n", code, periodusec, repeats);

	if(repeats > 100) //must be wrong...
	{
		return;
	}

	code &= 0xFFFFF;

	for (i=0; i<12; i++) {
		dataBase4<<=2;
		dataBase4|=(code%3);
		code/=3;
	}

	repeats = 1 << (repeats & 0x07); // repeats := 2^repeats;

	for (j=0;j<repeats;j++) {
		// Sent one telegram

		// Recycle code as working var to save memory
		code=dataBase4;
		for (i=0; i<12; i++) {
			switch (code & 0x03) {
				case 0:
					pin_high();
					udelay(periodusec);
					pin_low();
					udelay(periodusec*3);
					pin_high();
					udelay(periodusec);
					pin_low();
					udelay(periodusec*3);
					break;
				case 1:
					pin_high();
					udelay(periodusec*3);
					pin_low();
					udelay(periodusec);
					pin_high();
					udelay(periodusec*3);
					pin_low();
					udelay(periodusec);
					break;
				case 2: // KA: X or float
					pin_high();
					udelay(periodusec);
					pin_low();
					udelay(periodusec*3);
					pin_high();
					udelay(periodusec*3);
					pin_low();
					udelay(periodusec);
					break;
			}
			// Next trit
			code>>=2;
		}

		// Send termination/synchronization-signal. Total length: 32 periods
		pin_high();
		udelay(periodusec);
		pin_low();
		udelay(periodusec*31);
	}

	pin_low(); //Make sure pin is low!!
}

static const struct file_operations transmitter_fops = {
	.owner = THIS_MODULE,
	.write = transmitter_write,
};

static int init_chardev(void)
{
	int ret = 0;

	ret = alloc_chrdev_region(&transmitter_dev.devt, 0, 1, this_device_name);
	if (ret < 0) {
		pr_err("%s: alloc_chrdev_region() failed: %d\n",
		       this_device_name, ret);
		return -1;
	}

	transmitter_dev.cdev = cdev_alloc();
	cdev_init(transmitter_dev.cdev, &transmitter_fops);

	ret = cdev_add(transmitter_dev.cdev, transmitter_dev.devt, 1);
	if (ret < 0) {
		pr_err("%s: cdev_add failed: %d\n", this_device_name, ret);
		goto fail_cdev;
	}
	/* udev & HAL usage */
	transmitter_dev.class = class_create(THIS_MODULE, "transmitter_class");
	if (device_create(transmitter_dev.class, NULL, transmitter_dev.devt, NULL,
			  this_device_name) == NULL) {
		pr_err("%s: device_create failed: %d\n", this_device_name, ret);
		ret = -1;
		goto fail_class;
	}

	return 0;

fail_class:
	cdev_del(transmitter_dev.cdev);

fail_cdev:
	unregister_chrdev_region(transmitter_dev.devt, 1);
	return ret;
}

static int init_gpio(void)
{
	int val = 0;

	remap_config_module = ioremap(CONFIG_MODULE, CONFIG_MODULE_SIZE);
    if (!remap_config_module)
    	return -1;

	writel(0x0F, remap_config_module + 0x08); //GPMC_AD2 (gpio1_2) mux to mode 7 for gpio!

	remap_gpio1 = ioremap(GPIO_1_OFFSET, GPIO_1_REG_SIZE);
	if (!remap_gpio1) {
		iounmap(remap_config_module);
		return -1;
	}

	val = gpio_request(GPIO_1(PIN_DATA), "TransmitterData");
	if (val) {
		pr_err("TRANS: Data line request Error[%d]", PIN_DATA);
		iounmap(remap_gpio1);
		iounmap(remap_config_module);
		return -1;
	}

    val = readl(remap_gpio1 + GPIO_OE_OFFSET);      ///< Read current register
    val &= ~(1 << PIN_DATA);      ///< Set GPIO DATA PIN as OUTPUT
    writel(val, remap_gpio1 + GPIO_OE_OFFSET);

	pin_low(); //Always start with pin low!!!
	return 0;
}

static void free_gpio(void)
{
	gpio_free(GPIO_1(PIN_DATA));
	iounmap(remap_gpio1);
	iounmap(remap_config_module);
}

static int bee_quattro_init(void)
{
    int ret;
    pr_warn("TRANS: %s\n", __func__);

    memset(&transmitter_dev, 0, sizeof(struct transmitter_dev_t));

	ret = init_chardev();
	if (ret != 0) {
        return ret;
    }

	ret = init_gpio();
	if (ret != 0){
		return ret;
	}

	return 0;
}

static void __exit bee_quattro_exit(void)
{
    // chardev cleanup
	device_destroy(transmitter_dev.class, transmitter_dev.devt);
	class_unregister(transmitter_dev.class);
	class_destroy(transmitter_dev.class);
	cdev_del(transmitter_dev.cdev);
	unregister_chrdev_region(transmitter_dev.devt, 1);

	free_gpio();
}

module_init(bee_quattro_init);
module_exit(bee_quattro_exit);

MODULE_AUTHOR("Benchmark");
MODULE_DESCRIPTION("QUATTRO SPI driver for BEE");
MODULE_LICENSE("GPL");

