/*
 * Copyright (c) 2020 Laird Connectivity
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <bluetooth/gatt.h>
#include <bluetooth/bluetooth.h>

#define IS_NOTIFIABLE(v) ((v) == BT_GATT_CCC_NOTIFY) ? true : false;

/** The upper 8 bits of a 16 bit value */
#define MSB_16(a) (((a)&0xFF00) >> 8)
/** The lower 8 bits (of a 16 bit value) */
#define LSB_16(a) ((a)&0x00FF)

/* Client Characteristic Configuration Descriptor */
struct lbt_ccc_element {
	struct bt_gatt_ccc_cfg cfg[BT_GATT_CCC_MAX];
	bool notify;
};

/* Link a CCC handler 
 * assumes lbt_ccc_element array named ccc
 * and a function of the form name_ccc_handler
 */
#define LBT_GATT_CCC(name) BT_GATT_CCC(ccc.name.cfg, name##_ccc_handler)


/**
 *  @brief Helper function for reading an integer from the Bluetooth stack.
 */
ssize_t lbt_read_integer(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, u16_t len, u16_t offset);

/**
 *  @brief Helper function for reading a string characteristic from 
 * the Bluetooth stack.
 */
ssize_t lbt_read_string(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, u16_t len, u16_t offset,
			u16_t max_str_length);

/** 
 * @brief Helper function for writing a string characteristic from the 
 * Bluetooth stack.
 * @param max_str_length is the maximum string length of the item.  
 * It is assumed by this function the the actual size of the item is 1 character 
 * longer so that the string can be NULL terminated.
 */
ssize_t lbt_write_string(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, u16_t len, u16_t offset, u8_t flags,
			 u16_t max_str_length);
