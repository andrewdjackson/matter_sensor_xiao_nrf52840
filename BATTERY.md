# Battery Reporting

The battery is reported through Matter's standard **Power Source** cluster on endpoint 0, with values updated in `src/app_task.cpp`. There is no battery monitor chip: the device simply measures its own supply voltage.

## How the voltage is measured

The two AA lithium cells connect straight to the Xiao's 3V3 pin, so the nRF52840's supply voltage (VDD) *is* the battery voltage. The board overlay (`boards/xiao_ble_nrf52840.overlay`) sets up an analog-to-digital converter (ADC) channel that measures that supply internally, so no pins are used. `ReadBatteryMillivolts()` reads it in millivolts.

The battery is read along with temperature and humidity: right after commissioning (or immediately on boot if already commissioned), then every 5 minutes (`kMeasurementsIntervalMs`).

## Attributes set once at startup

Set in `InitPowerSourceCluster()`.

|Attribute|Value|
|-|-|
|Status|Active|
|Description|"Battery"|
|BatReplaceability|User replaceable|
|BatReplacementDescription|"2 x AA lithium (Li-FeS2)"|
|BatCommonDesignation|AA|
|BatApprovedChemistry|Lithium iron disulfide|
|BatQuantity|2|
|EndpointList|0, 1, 2 (links the battery to the temperature and humidity endpoints so controllers show it on the device card)|

## Attributes updated on every reading

Set in `UpdateBatteryClusterState()`.

|Attribute|How it's set|
|-|-|
|BatVoltage|The measured voltage in mV|
|BatPercentRemaining|Estimated from the voltage using the curve below. Matter counts in half percents, so the code sends `percent * 2` (0-200).|
|BatChargeLevel|OK at 2800 mV or above, Warning below 2800 mV, Critical below 2400 mV|
|BatReplacementNeeded|True whenever the charge level isn't OK|

Voltage to percentage curve (`kBatteryLevelCurve`), with straight line steps between points:

|mV|3500|3300|3000|2800|2400|2200|
|-|-|-|-|-|-|-|
|%|100|80|40|20|5|0|

Lithium AA cells hold a steady voltage for most of their life and then drop quickly, so the percentage is only a rough guide: it will stay high for months and then fall quickly. The Warning and Critical charge levels are the more dependable signal.

The curve and thresholds (`kBatteryLevelCurve`, `kBatteryWarningMv`, `kBatteryCriticalMv`) are at the top of `src/app_task.cpp`. If you fit the protection diode described in [WIRING.md](WIRING.md), the measured voltage is about 0.2-0.3 V lower than the battery voltage, so lower them to match.

## When no reading is possible

If USB is plugged in (`IsUsbPowered()`), the 3V3 rail comes from the Xiao's own regulator, so the voltage says nothing about the batteries. The same applies if the ADC fails to read. In either case:

* BatVoltage and BatPercentRemaining are set to **null** (unknown), not a misleading number.
* BatChargeLevel is set to OK and BatReplacementNeeded to false, so you don't get false low battery alerts.

## Supply voltage limit

Fresh lithium AA cells can measure about 1.8 V each, roughly 3.6 V for the pair. That's right at the nRF52840's maximum recommended supply voltage. They drop below that quickly under load, but brand new cells will sit at the edge of the spec for a while.
