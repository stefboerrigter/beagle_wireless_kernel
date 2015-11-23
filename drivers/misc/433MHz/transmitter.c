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
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include "transmitter_data.h"

#define DEVICE_NAME "transmitter"

//MACROs to set GPIO
#define pin_high()  gpio_set_value(radio_gpio, 1)   //	 REG(GPIO1_SET) = (1<<PIN_DATA)	///< START ACQ HIGH
#define pin_low()   gpio_set_value(radio_gpio, 0)   //	 REG(GPIO1_CLR) = (1<<PIN_DATA)	///< START ACQ LOW



static const char this_device_name[] = DEVICE_NAME;

static void transmit_code(unsigned long code, int periodusec, int repeats);

//header Definitions
static int transmitter_probe(struct platform_device *pdev);
static int transmitter_remove(struct platform_device *pdev);

static int transmitter_chdev_init(void );
static void transmitter_chdev_destroy(void );

//static DEVICE_ATTR(node_name, S_IWUSR, NULL, func);

struct transmitter_dev_t {
	dev_t devt;		/* Char dev region (major,minor) */
	struct cdev *cdev;	/* Char dev related structure */
	struct class *class;	/* Udev / HAL related */
};

#define DEVICE_COMPATIBLE_PRE		"ti"
#define compatible(_name)		{ .compatible = DEVICE_COMPATIBLE_PRE "," DEVICE_NAME }


// use macro to get the device name definition
#define device_name			transmitter_device_name

// as this is not a static, use a unique name (fix by using device_name macro)
const char transmitter_device_name[] = DEVICE_NAME;

// Set the device tree compatible types
static struct of_device_id compatible_dt_ids[] = {
	compatible(DEVICE_NAME),
	{}
};

// Define platform driver specification (device tree)
static struct platform_driver platform_driver = {
	.probe = transmitter_probe,
	.remove = transmitter_remove,
	.driver = {
		.name = DEVICE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = compatible_dt_ids,
	},
};

static struct chdev_struct {
	dev_t dev_num;		/* Char dev region (major,minor), reterned by kernel on alloc_chrdev_region() */
	struct cdev *cdev;	/* Char dev related structure */
	struct class *class;	/* Udev / HAL related */
} device_struct;

//Gpio Used for radio
static int radio_gpio = -1;

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

	pr_alert("%s: Sending 0x%lx (delay %d, repeats[%d])\n", device_name, code, periodusec, repeats);

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


/**
 * Initialize character device
 */
static int transmitter_chdev_init(void)
{
	int ret = 0;

	ret = alloc_chrdev_region(&device_struct.dev_num, 0, 1, device_name);
	if (ret < 0) {
		pr_err("%s: alloc_chrdev_region() failed with %d\n", device_name, ret);
		return -1;
	}

	device_struct.cdev = cdev_alloc();
	cdev_init(device_struct.cdev, &transmitter_fops);

	ret = cdev_add(device_struct.cdev, device_struct.dev_num, 1);
	if (ret < 0) {
		pr_err("%s: cdev_add failed: %d\n", device_name, ret);
		goto fail_cdev;
	}

	/* udev & HAL usage */
	device_struct.class = class_create(THIS_MODULE, DEVICE_NAME "_class");

	if (device_create(device_struct.class, NULL, device_struct.dev_num, NULL,
			  device_name) == NULL) {
		pr_err("%s: device_create failed: %d\n", device_name, ret);
		ret = -1;
		goto fail_class;
	}

	return 0;

fail_class:
	cdev_del(device_struct.cdev);

fail_cdev:
	unregister_chrdev_region(device_struct.dev_num, 1);
	return ret;

}

/**
 * Cleanup character device
 */
static void transmitter_chdev_destroy(void)
{
	device_destroy(device_struct.class, device_struct.dev_num);
	class_unregister(device_struct.class);
	class_destroy(device_struct.class);
	cdev_del(device_struct.cdev);
	unregister_chrdev_region(device_struct.dev_num, 1);
}


static int transmitter_probe(struct platform_device *pdev)
{
  const struct of_device_id * of_id;
	int ret = 0;

	// variables to use to request GPIO
	enum of_gpio_flags ofgpioflags;
	unsigned long gpioflags;

	// Check on which device we match (can be used to make special configurations)
	of_id = of_match_device(compatible_dt_ids, &(pdev->dev));
  
  if(of_id){
		pr_alert("%s: matching device id found, name: %s, type: %s, compatible: %s?\n", device_name, of_id->name, of_id->type, of_id->compatible);
	}else{
		pr_err("%s: no matching device id found?\n", device_name);// todo
		return -1;
	}
  
  // Configure GPIO
	radio_gpio = of_get_named_gpio_flags(pdev->dev.of_node, "radio-gpio", 0, &ofgpioflags);
	if (IS_ERR_VALUE(radio_gpio)) {
		dev_err(&pdev->dev, "%s: error obtaining GPIO from devicetree(%d)\n", device_name, radio_gpio);
		return radio_gpio;
	} else {
		gpioflags = GPIOF_DIR_OUT;
		if (ofgpioflags & OF_GPIO_ACTIVE_LOW) {
			gpioflags |= GPIOF_INIT_LOW;
		} else {
			gpioflags |= GPIOF_INIT_HIGH;
		}
		ret = devm_gpio_request_one(&pdev->dev, radio_gpio, gpioflags, DEVICE_NAME ":radio");
		if (ret != 0) {
			dev_err(&pdev->dev, "%s: failed to request GPIO\n", device_name);
			return ret;
		}
	}

  // initialize character device
	ret = transmitter_chdev_init();
	if(ret < 0){
		pr_err("%s: error initializing character device\n", device_name);
    return ret;
	}

  pr_alert("%s - start with line low\n", device_name);
	pin_low(); //Always start with pin low!!!
  return 0;
}


/**
 * Called when driver is removed
 */
static int transmitter_remove(struct platform_device *pdev)
{
  transmitter_chdev_destroy();
  gpio_free(radio_gpio);
  
  return 0;
}

/**
 * Initialize transmitter driver (driver entry, setup everything to register
 * driver later on)
 */
static int transmitter_init(void)
{
  pr_warn("%s - Init: %s\n", device_name, __func__);

  radio_gpio = -1;
  
  platform_driver_register(&platform_driver);

	return 0;
}

/**
 * Cleanup transmitter driver
 */
static void __exit transmitter_exit(void)
{
  pr_alert("%s: exit platform device\n", device_name);
	platform_driver_unregister(&platform_driver);
}

module_init(transmitter_init);
module_exit(transmitter_exit);

MODULE_AUTHOR("Stef");
MODULE_DESCRIPTION("Radio Transmitter Driver");
MODULE_LICENSE("GPL");

