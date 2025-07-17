// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Lukasz Bielinski

#ifndef DISPLAYM_H
#define DISPLAYM_H

/**
 * @brief DisplayM module initialization
 * This function initializes the display module, sets up the I2C bus,
 * configures the display panel, and initializes LVGL.
 */
void DisplayM_Init(void);

/**
 * @brief Enable or disable demo mode
 * This function switches between normal mode and demo mode.
 * In demo mode, the display shows a predefined animation.
 * In normal mode, it shows the current channel data.
 *
 * @param enable true to enable demo mode, false to disable it
 */
void DisplayM_EnableDemoMode(bool enable);

#endif // DISPLAYM_H
