/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "lib/core/CHIPError.h"
#include "lib/support/CodeUtils.h"

#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>

#include <hal/nrf_power.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/power-source-server/power-source-server.h>

#include <cmath>
#include <iterator>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace {
	// Set the measurement interval.
	constexpr size_t kMeasurementsIntervalMs = 300000; // 5 Minutes

	// Endpoint and attribute constants for Temperature Measurement
	constexpr uint8_t kTemperatureMeasurementEndpointId = 1;
	constexpr int16_t kTemperatureMeasurementAttributeMaxValue = 0x7fff;
	constexpr int16_t kTemperatureMeasurementAttributeMinValue = 0x954d;
	constexpr int16_t kTemperatureMeasurementAttributeInvalidValue = 0x8000;

	// Endpoint and attribute constants for Relative Humidity Measurement
	constexpr uint8_t kHumidityMeasurementEndpointId = 2;
	constexpr uint16_t kHumidityMeasurementAttributeMaxValue = 10000; // 100.00%
	constexpr uint16_t kHumidityMeasurementAttributeMinValue = 0; // 0.00%
	constexpr uint16_t kHumidityMeasurementAttributeInvalidValue = 0xffff; // Invalid value

	k_timer sMeasurementsTimer;
	bool sIsMeasurementTimerStarted = false;

	const device *sht4x_dev = DEVICE_DT_GET_ONE(sensirion_sht4x);

#ifdef CONFIG_NCS_SAMPLE_MATTER_LEDS
	// Status LED, only lit while commissioning to save battery. LED3 is the Xiao's blue LED.
	constexpr Nrf::DeviceLeds kStatusLed = Nrf::DeviceLeds::LED3;
	// Short flash once a second while advertising and waiting to be commissioned.
	constexpr uint32_t kAdvertisingLedOnMs = 50;
	constexpr uint32_t kAdvertisingLedOffMs = 950;
	// Fast even blink while a commissioner is connected over Bluetooth.
	constexpr uint32_t kBleConnectedLedOnMs = 100;
	constexpr uint32_t kBleConnectedLedOffMs = 100;
#endif

	// Power Source cluster, reporting the 2 x AA lithium batteries.
	constexpr EndpointId kPowerSourceEndpointId = 0;
	constexpr uint8_t kBatteryQuantity = 2;
	constexpr char kPowerSourceDescription[] = "Battery";
	constexpr char kBatteryReplacementDescription[] = "2 x AA lithium (Li-FeS2)";

	// Battery pack voltages at which the charge level drops to Warning and Critical.
	constexpr int32_t kBatteryWarningMv = 2800;
	constexpr int32_t kBatteryCriticalMv = 2400;

	// Approximate state of charge for 2 x AA lithium (Li-FeS2) cells in series, under light load.
	// The discharge curve is flat for most of the battery life and then falls quickly, so this is
	// only a rough guide. Points must be in descending voltage order.
	struct BatteryLevelPoint {
		int32_t millivolts;
		uint8_t percent;
	};
	constexpr BatteryLevelPoint kBatteryLevelCurve[] = {
		{ 3500, 100 }, { 3300, 80 }, { 3000, 40 }, { 2800, 20 }, { 2400, 5 }, { 2200, 0 },
	};

	// The batteries feed the 3V3 rail directly, so the nRF52840 supply voltage (VDD) is the
	// battery voltage. Boards without the ADC channel in their overlay don't measure it.
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#define BATTERY_ADC_AVAILABLE 1
	const adc_dt_spec kBatteryAdc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);
	bool sBatteryAdcReady = false;

	int ReadBatteryMillivolts(int32_t &millivolts)
	{
		int16_t sample = 0;
		adc_sequence sequence = {};
		sequence.buffer = &sample;
		sequence.buffer_size = sizeof(sample);

		int rc = adc_sequence_init_dt(&kBatteryAdc, &sequence);
		if (rc == 0) {
			rc = adc_read_dt(&kBatteryAdc, &sequence);
		}
		if (rc != 0) {
			return rc;
		}

		// Single-ended readings can dip slightly below zero.
		millivolts = sample < 0 ? 0 : sample;
		return adc_raw_to_millivolts_dt(&kBatteryAdc, &millivolts);
	}
#endif

	uint8_t BatteryPercentFromMillivolts(int32_t millivolts)
	{
		if (millivolts >= kBatteryLevelCurve[0].millivolts) {
			return kBatteryLevelCurve[0].percent;
		}

		// Interpolate linearly between the two surrounding points of the curve.
		for (size_t i = 1; i < std::size(kBatteryLevelCurve); i++) {
			const BatteryLevelPoint &high = kBatteryLevelCurve[i - 1];
			const BatteryLevelPoint &low = kBatteryLevelCurve[i];
			if (millivolts >= low.millivolts) {
				return static_cast<uint8_t>(low.percent + (millivolts - low.millivolts) * (high.percent - low.percent) /
								  (high.millivolts - low.millivolts));
			}
		}
		return 0;
	}

	// When USB is plugged in, the Xiao's regulator powers the 3V3 rail, so VDD no longer tells
	// us anything about the batteries.
	bool IsUsbPowered()
	{
		return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
	}

	void LogIfFailed(Protocols::InteractionModel::Status status, const char *attributeName)
	{
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating %s failed: %x", attributeName, to_underlying(status));
		}
	}

} //namespace

void AppTask::MatterEventHandler(const ChipDeviceEvent *event, intptr_t arg)
{
	// On initial commissioning, start the timer.
	// We use a flag to ensure this only happens once per boot.
	if (event->Type == DeviceEventType::kCommissioningComplete && !sIsMeasurementTimerStarted) {
		LOG_INF("Commissioning complete, starting measurements.");

		// Take an immediate reading by calling UpdateClustersState directly.
		// This is posted to the event queue to ensure it runs in the correct context.
		Nrf::PostTask([] { Instance().UpdateClustersState(); });

		// Start the periodic timer. K_NO_WAIT is NOT used here because we just
		// triggered the first reading manually. The timer will fire after the first interval.
		k_timer_start(&sMeasurementsTimer, K_MSEC(kMeasurementsIntervalMs), K_MSEC(kMeasurementsIntervalMs));
		sIsMeasurementTimerStarted = true;
	}
}

#ifdef CONFIG_NCS_SAMPLE_MATTER_LEDS
void AppTask::UpdateStatusLED()
{
	// Replaces the default Nordic status LED handler, which keeps the LED solid on once
	// commissioned and would drain the battery. Here the LED is off unless commissioning.
	Nrf::LEDWidget &statusLed = Nrf::GetBoard().GetLED(kStatusLed);
	statusLed.Set(false);

	switch (Nrf::GetBoard().GetDeviceState()) {
	case Nrf::DeviceState::DeviceAdvertisingBLE:
		statusLed.Blink(kAdvertisingLedOnMs, kAdvertisingLedOffMs);
		break;
	case Nrf::DeviceState::DeviceConnectedBLE:
		statusLed.Blink(kBleConnectedLedOnMs, kBleConnectedLedOffMs);
		break;
	default:
		break;
	}
}
#endif

void AppTask::UpdateTemperatureClusterState()
{
	struct sensor_value sTemperature;
	Protocols::InteractionModel::Status status;
	int result = sensor_channel_get(sht4x_dev, SENSOR_CHAN_AMBIENT_TEMP, &sTemperature);

	if (result == 0) {
		// The MeasuredValue attribute is in 1/100ths of a degree Celsius.
		// First, get the temperature in 1/100ths of a degree.
		int32_t temp_in_hundredths = sTemperature.val1 * 100 + sTemperature.val2 / 10000;
		int16_t newValue;

		// Round to the nearest half degree (i.e., nearest 50 hundredths).
		// This logic correctly handles rounding for both positive and negative temperatures.
		if (temp_in_hundredths >= 0) {
			newValue = static_cast<int16_t>(((temp_in_hundredths + 25) / 50) * 50);
		} else {
			newValue = static_cast<int16_t>(((temp_in_hundredths - 25) / 50) * 50);
		}

		if (newValue > kTemperatureMeasurementAttributeMaxValue || newValue < kTemperatureMeasurementAttributeMinValue) {
			newValue = kTemperatureMeasurementAttributeInvalidValue;
		}
		LOG_DBG("New temperature measurement: %d.%06d *C, rounded attribute value: %d", sTemperature.val1,
			sTemperature.val2, newValue);

		status = Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(kTemperatureMeasurementEndpointId, newValue);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating temperature measurement failed: %x", to_underlying(status));
		}
	} else {
		LOG_ERR("Getting temperature measurement data from BME280 failed with: %d", result);
	}
}

void AppTask::UpdateHumidityClusterState()
{
	struct sensor_value sHumidity;
	Protocols::InteractionModel::Status status;
	int result = sensor_channel_get(sht4x_dev, SENSOR_CHAN_HUMIDITY, &sHumidity);

	if (result == 0) {
		// Round the humidity reading to the nearest whole percentage.
		// The sensor_value struct provides the integer part in val1 and the fractional part
		// in val2 (in millionths). We round up if the fractional part is >= 0.5 (500,000 millionths).
		int32_t rounded_humidity_percent = sHumidity.val1;
		if (sHumidity.val2 >= 500000) {
			rounded_humidity_percent++;
		}

		// The Relative Humidity MeasuredValue attribute is in 1/100ths of a percent.
		uint16_t newValue = static_cast<uint16_t>(rounded_humidity_percent * 100);

		LOG_DBG("New humidity measurement: %d.%06d %%RH, rounded to: %d %%, attribute value: %u", sHumidity.val1,
			sHumidity.val2, rounded_humidity_percent, newValue);

		// Validate the reading is within the defined range for the attribute.
		if (newValue > kHumidityMeasurementAttributeMaxValue || newValue < kHumidityMeasurementAttributeMinValue) {
			newValue = kHumidityMeasurementAttributeInvalidValue;
			LOG_WRN("Humidity value out of range, setting to invalid.");
		}

		status = Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(kHumidityMeasurementEndpointId, newValue);
		if (status != Protocols::InteractionModel::Status::Success) {
			LOG_ERR("Updating humidity measurement failed: %x", to_underlying(status));
		}
	} else {
		LOG_ERR("Getting humidity measurement data from BME280 failed with: %d", result);
	}
}
void AppTask::InitPowerSourceCluster()
{
	namespace PowerSource = Clusters::PowerSource;
	namespace Attributes = Clusters::PowerSource::Attributes;

	// Fixed details of the batteries, which don't change at runtime.
	LogIfFailed(Attributes::Status::Set(kPowerSourceEndpointId, PowerSource::PowerSourceStatusEnum::kActive), "Status");
	LogIfFailed(Attributes::Order::Set(kPowerSourceEndpointId, 0), "Order");
	LogIfFailed(Attributes::Description::Set(kPowerSourceEndpointId, CharSpan::fromCharString(kPowerSourceDescription)),
		    "Description");
	LogIfFailed(Attributes::BatReplaceability::Set(kPowerSourceEndpointId, PowerSource::BatReplaceabilityEnum::kUserReplaceable),
		    "BatReplaceability");
	LogIfFailed(Attributes::BatReplacementDescription::Set(kPowerSourceEndpointId,
						   CharSpan::fromCharString(kBatteryReplacementDescription)),
		    "BatReplacementDescription");
	LogIfFailed(Attributes::BatCommonDesignation::Set(kPowerSourceEndpointId, PowerSource::BatCommonDesignationEnum::kAa),
		    "BatCommonDesignation");
	LogIfFailed(Attributes::BatApprovedChemistry::Set(kPowerSourceEndpointId,
					      PowerSource::BatApprovedChemistryEnum::kLithiumIronDisulfide),
		    "BatApprovedChemistry");
	LogIfFailed(Attributes::BatQuantity::Set(kPowerSourceEndpointId, kBatteryQuantity), "BatQuantity");

	// Tell controllers which endpoints this battery powers. Without this the EndpointList is empty and
	// controllers don't link the battery to the sensor endpoints, so it isn't shown on the device card.
	EndpointId poweredEndpoints[] = { kPowerSourceEndpointId, kTemperatureMeasurementEndpointId,
					  kHumidityMeasurementEndpointId };
	const CHIP_ERROR err = Clusters::PowerSourceServer::Instance().SetEndpointList(kPowerSourceEndpointId,
										 Span<EndpointId>(poweredEndpoints));
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Setting power source endpoint list failed: %" CHIP_ERROR_FORMAT, err.Format());
	}

#ifdef BATTERY_ADC_AVAILABLE
	if (!adc_is_ready_dt(&kBatteryAdc)) {
		LOG_ERR("Battery ADC device not ready");
	} else {
		const int rc = adc_channel_setup_dt(&kBatteryAdc);
		if (rc != 0) {
			LOG_ERR("Setting up battery ADC channel failed with: %d", rc);
		}
		sBatteryAdcReady = (rc == 0);
	}
#endif
}

void AppTask::UpdateBatteryClusterState()
{
	namespace PowerSource = Clusters::PowerSource;
	namespace Attributes = Clusters::PowerSource::Attributes;

	int32_t millivolts = 0;
	bool measured = false;

	if (IsUsbPowered()) {
		LOG_DBG("USB powered, battery voltage not measured");
	} else {
#ifdef BATTERY_ADC_AVAILABLE
		const int rc = sBatteryAdcReady ? ReadBatteryMillivolts(millivolts) : -ENODEV;
		if (rc == 0) {
			measured = true;
		} else {
			LOG_ERR("Reading battery voltage failed with: %d", rc);
		}
#endif
	}

	if (!measured) {
		// Unknown battery level: report null rather than a misleading value, and don't raise
		// low battery warnings.
		LogIfFailed(Attributes::BatVoltage::SetNull(kPowerSourceEndpointId), "BatVoltage");
		LogIfFailed(Attributes::BatPercentRemaining::SetNull(kPowerSourceEndpointId), "BatPercentRemaining");
		LogIfFailed(Attributes::BatChargeLevel::Set(kPowerSourceEndpointId, PowerSource::BatChargeLevelEnum::kOk), "BatChargeLevel");
		LogIfFailed(Attributes::BatReplacementNeeded::Set(kPowerSourceEndpointId, false), "BatReplacementNeeded");
		return;
	}

	const uint8_t percent = BatteryPercentFromMillivolts(millivolts);

	PowerSource::BatChargeLevelEnum chargeLevel = PowerSource::BatChargeLevelEnum::kOk;
	if (millivolts < kBatteryCriticalMv) {
		chargeLevel = PowerSource::BatChargeLevelEnum::kCritical;
	} else if (millivolts < kBatteryWarningMv) {
		chargeLevel = PowerSource::BatChargeLevelEnum::kWarning;
	}

	LOG_DBG("New battery measurement: %d mV, %u %%, charge level: %u", millivolts, percent,
		to_underlying(chargeLevel));

	LogIfFailed(Attributes::BatVoltage::Set(kPowerSourceEndpointId, static_cast<uint32_t>(millivolts)), "BatVoltage");
	// BatPercentRemaining is in half percent units (0-200).
	LogIfFailed(Attributes::BatPercentRemaining::Set(kPowerSourceEndpointId, static_cast<uint8_t>(percent * 2)),
		    "BatPercentRemaining");
	LogIfFailed(Attributes::BatChargeLevel::Set(kPowerSourceEndpointId, chargeLevel), "BatChargeLevel");
	LogIfFailed(Attributes::BatReplacementNeeded::Set(kPowerSourceEndpointId,
					      chargeLevel != PowerSource::BatChargeLevelEnum::kOk),
		    "BatReplacementNeeded");
}

void AppTask::MeasurementsTimerHandler()
{
	Instance().UpdateClustersState();
}

void AppTask::UpdateClustersState()
{
	// Fetch a new sample from the sensor. This updates all channels.
	const int result_bme = sensor_sample_fetch(sht4x_dev);

	// This runs on the application thread, so hold the Matter stack lock while updating attributes.
	DeviceLayer::StackLock lock;

	if (result_bme == 0) {
		// Update both clusters with the new data.
		UpdateTemperatureClusterState();
		UpdateHumidityClusterState();
	} else {
		LOG_ERR("Fetching data from SHT4X sensor failed with: %d", result_bme);
	}
	UpdateBatteryClusterState();
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());

#ifdef CONFIG_NCS_SAMPLE_MATTER_LEDS
	const bool boardInitialized = Nrf::GetBoard().Init(nullptr, UpdateStatusLED);
#else
	const bool boardInitialized = Nrf::GetBoard().Init();
#endif
	if (!boardInitialized) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Register Matter event handler that controls the connectivity status LED based on the captured Matter network
	 * state. */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(MatterEventHandler, 0));

	if (!device_is_ready(sht4x_dev)) {
		LOG_ERR("SHT4X sensor device not ready");
		return chip::System::MapErrorZephyr(-ENODEV);
	}

	k_timer_init(&sMeasurementsTimer, [](k_timer *) { Nrf::PostTask([] { MeasurementsTimerHandler(); }); }, nullptr);

	ReturnErrorOnFailure(Nrf::Matter::StartServer());

	{
		DeviceLayer::StackLock lock;
		InitPowerSourceCluster();
	}

	// On reboot, if the device is already commissioned, start the timer directly.
	if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0 && !sIsMeasurementTimerStarted) {
		LOG_INF("Device is already commissioned on boot, starting measurements.");
		// Use K_NO_WAIT to get an immediate reading on reboot.
		k_timer_start(&sMeasurementsTimer, K_NO_WAIT, K_MSEC(kMeasurementsIntervalMs));
		sIsMeasurementTimerStarted = true;
	}

	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
