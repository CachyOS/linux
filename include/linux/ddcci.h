/*
 *  DDC/CI bus driver
 *
 *  Copyright (c) 2015 Christoph Grenz
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _DDCCI_H
#define _DDCCI_H

#include <linux/mod_devicetable.h>
#include <linux/device.h>
#include <linux/cdev.h>

#define DDCCI_MODULE_PREFIX "ddcci:"

/* Special addresses */

/* default device address (even) */
#define DDCCI_DEFAULT_DEVICE_ADDR	0x6E
/* receiving host address for communication with default device address */
#define DDCCI_HOST_ADDR_EVEN	0x50
/* sending host address for communication with default device address */
#define DDCCI_HOST_ADDR_ODD	0x51

/* Command codes */

/* Identification Request */
#define DDCCI_COMMAND_ID	0xf1
/* Identification Reply */
#define DDCCI_REPLY_ID	0xe1
/* Capabilities Request */
#define DDCCI_COMMAND_CAPS	0xf3
/* Capabilities Reply */
#define DDCCI_REPLY_CAPS	0xe3

/* Quirks */

/* Device always responds with unset protocol flag */
#define DDCCI_QUIRK_NO_PFLAG BIT(1)
/* Device needs writing one byte at a time  */
#define DDCCI_QUIRK_WRITE_BYTEWISE BIT(2)
/* Device repeats first byte on read */
#define DDCCI_QUIRK_SKIP_FIRST_BYTE BIT(3)

/* Flags */

#define DDCCI_FLAG_REMOVED BIT(1)
#define DDCCI_FLAG_DEPENDENT BIT(2)
#define DDCCI_FLAG_EXTERNAL BIT(3)

extern struct bus_type ddcci_bus_type;

struct ddcci_bus_drv_data;

/* struct ddcci_device_id - identifies DDC/CI devices for probing */
struct ddcci_device_id {
	char prot[9];
	char type[9];
	char model[9];
	char vendor[9];
	char module[9];
	kernel_ulong_t driver_data;	/* Data private to the driver */
};
#define DDCCI_ANY_ID "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"

/**
 * struct ddcci_device - represent an DDC/CI device
 * @outer_addr: Outer device address (I2C address << 1).
 * @inner_addr: Inner device address.
 * @flags: Device flags.
 * @capabilities: Device capability string.
 * @capabilities_len: Length of capability string.
 * @i2c_client: Parent I2C device.
 * @bus_drv_data: Driver internal data structure.
 * @dev: Driver model device node for the slave.
 * @cdev: Character device structure
 * @cdev_sem: RW semaphore for exclusive access on character device.
 * @prot: Device class ("protocol", from capability string)
 * @type: Device subclass ("type", from capability string)
 * @model: Device model (from capability string)
 * @vendor: Device vendor (from identification command response)
 * @module: Device module (from identification command response)
 * @device_number: Device serial (from identification command response)
 */
struct ddcci_device {
	unsigned short outer_addr;
	unsigned short inner_addr;
	int flags;
	char *capabilities;
	size_t capabilities_len;
	struct i2c_client *i2c_client;
	struct ddcci_bus_drv_data *bus_drv_data;
	struct device dev;
	struct cdev cdev;
	struct rw_semaphore cdev_sem;
	char prot[9];
	char type[9];
	char model[9];
	char vendor[9];
	char module[9];
	int device_number;
};
#define to_ddcci_device(d) container_of(d, struct ddcci_device, dev)

/**
 * struct ddcci_driver - represent an DDC/CI device driver
 * @probe: Callback for device binding
 * @remove: Callback for device unbinding
 * @driver: Device driver model driver
 * @id_table: List of DDC/CI devices supported by this driver
 *
 * The driver.owner field should be set to the module owner of this driver.
 * The driver.name field should be set to the name of this driver.
 */
struct ddcci_driver {
	int (*probe)(struct ddcci_device *, const struct ddcci_device_id *);
	int (*remove)(struct ddcci_device *);
	struct device_driver driver;
	struct ddcci_device_id *id_table;
};
#define to_ddcci_driver(d) container_of(d, struct ddcci_driver, driver)

int ddcci_register_driver(struct module *owner, struct ddcci_driver *driver);
#define ddcci_add_driver(driver) \
	ddcci_register_driver(THIS_MODULE, driver)
void ddcci_del_driver(struct ddcci_driver *driver);

struct ddcci_device *ddcci_verify_device(struct device *dev);

#define module_ddcci_driver(__ddcci_driver) \
	module_driver(__ddcci_driver, ddcci_add_driver, \
			ddcci_del_driver)

int ddcci_device_write(struct ddcci_device *, bool p_flag, unsigned char *data,
		       unsigned char length);
int ddcci_device_read(struct ddcci_device *, bool p_flag, unsigned char *buffer,
		      unsigned char length);
int ddcci_device_writeread(struct ddcci_device *, bool p_flag,
			   unsigned char *buffer, unsigned char length,
			   unsigned char maxlength);

static inline void *ddcci_get_drvdata(const struct ddcci_device *dev)
{
	return dev_get_drvdata(&dev->dev);
}

static inline void ddcci_set_drvdata(struct ddcci_device *dev, void *data)
{
	dev_set_drvdata(&dev->dev, data);
}

unsigned long ddcci_quirks(struct ddcci_device *dev);

const char *ddcci_find_capstr_item(const char *capabilities, const char *tag,
				   size_t *length);

#endif
