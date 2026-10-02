/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <platform/CHIPDeviceLayer.h>

struct k_timer;

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();

	void UpdateClustersState();

private:
	CHIP_ERROR Init();

	static void MatterEventHandler(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg);

	static void MeasurementsTimerHandler();

#ifdef CONFIG_NCS_SAMPLE_MATTER_LEDS
	static void UpdateStatusLED();
#endif

	void UpdateTemperatureClusterState();

	void UpdateHumidityClusterState();
	void InitPowerSourceCluster();

	void UpdateBatteryClusterState();

};
