/*
 * Copyright (c) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __BOARD_H__
#define __BOARD_H__

/**
 * @brief Fetch board ID from RVP/XEP.
 *
 * @retval board id value read from HW.
 */
int read_board_id(void);

/**
 * @brief Returns the last call of read_board_id
 *
 * @retval board id value read during boot.
 */
uint8_t get_board_id(void);

/**
 * @brief Gets hardware id.
 *
 * @retval board id value read during boot.
 */
uint8_t get_hw_id(void);

/**
 * @brief Gets board EC HW straps.
 *
 * @retval byte representing the EC HW straps status read during boot.
 */
uint8_t get_hw_straps(void);

/**
 * @brief Gets board fabrication id.
 *
 * @retval id value use to identify different HW revisions within same SKU.
 */
uint8_t get_fab_id(void);

/**
 * @brief Gets board platform information.
 *
 * @retval return the raw platform data read during boot.
 *
 * Note: This is the raw data as expected by BIOS.
 */
uint16_t get_platform_id(void);

/**
 * @brief Check status for device drivers required.
 *
 * @retval indicate if all required device drivers are correctly initialized or not.
 *
 * Note: List is generated from device tree entries.
 */
int board_devices_check(void);

/**
 * @brief Apply board/app specific pinctrl changes via zephyr, user device tree node
 *
 * @retval indicate if pin control/muxing from device tree was applied successfully.
 *
 */
int board_dts_pin_muxing(void);

#endif /* BOARD_H */
