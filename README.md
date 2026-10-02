# IKEA Temperature / Humidty Sensor

This is the code for the Matter Temperature and Humidity sensor I build over on my YT Channel. Code is built on the Seeed Studio Xiao nrf52840 using the nrf Connect Sdk

## Components
* Seeed Studio Xiao nrf52840
* Sensirion SHT41 (I2C Addr: 0x44) (Any SH4X model will work)
* 2 x AA lithium (Li-FeS2) batteries in a holder with an on/off switch

## Pin Mapping
The default I2C pins on the xiao have been remapped so that the clock and data pins (SCL and SDA) are next to the GND and 3V3 pins for convenience. This differs from Seeed's pinout, which shows I2C on D4/D5.

|Xiao nrf52840|SHT4X|
|-|-|
|GND|GND|
|3V3|VIN|
|D10 (P1.15)|SDA|
|D9 (P1.14)|SCL|

See [WIRING.md](WIRING.md) for the full wiring diagram, including buttons and running from 2 × AA lithium batteries.

## Build

1. Install v3.2.1 of the nrf Connect SDK and Toolchain for VS Code using the instructions for the nordic semi conductor website [here](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/recommended_versions.html#requirements-nrfvsc)
2. Open the project in VS code and create a new build configuration. Change the following settings:
    * **Board Target**: xiao_ble/nrf52840 (toggle to "Nordic SOC" for this to be visible)
    * **System Build (sysbuild)**: No sysbuild
3. Click **Generate and Build** to compile the firmware

## Flash

1. Locate the firmware: `./build/zephyr/zephyr.uf2` and open file location in file explorer
2. Plug in the xiao and double tap on the reset button. It should appear as a USB mass storage device
3. Drag the `zephyr.uf2` file onto the xiao and wait for the flash to complete (You may get an error about this failing due to the device disappearing, this is usually a false positive and the firmware flashed successfully)

## Commissioning

Set this up like any other matter device, using either of the default test codes:

* **Manual pairing code:** `34970112332`
* **QR code payload:** `MT:Y.K9042C00KA0648G00` (paste it into any QR code generator to get a scannable image)

Do not enter `20202021` as the pairing code. That is the setup passcode, which is only one part of the pairing code, and controllers will reject it with "Invalid pairing code". Both codes are also printed on the serial log at boot.

|Setting|Value|
|-|-|
|Passcode|20202021|
|Discriminator|3840 (0xF00)|
|Vendor ID / Product ID|0xFFF1 / 0x8000|

The device only advertises over Bluetooth while it is not commissioned, and advertising stops after a timeout. While it is advertising, the Xiao's blue LED gives a short flash once a second, and it blinks quickly while a commissioner is connected over Bluetooth. At all other times the LED is off to save battery. If the LED is off and the controller can't find the device, short press button 0 (D3) or power cycle the device. If it still doesn't flash, the device still holds an old pairing (for example, it was removed in the app while out of range). Factory reset it by holding button 0 for about 6 seconds, see [WIRING.md](WIRING.md#buttons).

These are test credentials shared by every device built from this code. They are fine for home use but not for a product.

### What you need

* **A Thread border router.** The sensor is a Matter over Thread device, so it can only join a Thread network. Examples are an Apple HomePod mini or Apple TV 4K, a Google Nest hub, an Aqara Hub M100, or a Raspberry Pi running OpenThread Border Router (OTBR).
* **A commissioner with Bluetooth.** Pairing happens over Bluetooth, and the commissioner hands the sensor the Thread network credentials. Usually this is a phone app (Apple Home, Google Home, Aqara Home, or the Home Assistant Companion app, which needs a Home Assistant server).

The sensor is built as a sleepy, battery-saving Thread device (ICD). It never relays traffic for other devices, so it must be within range of a Thread router: the border router itself or a mains-powered Thread device such as a plug or bulb. Test it in its final location before sealing it in an enclosure.

### Pairing distance

Keep the sensor in the same room as the commissioner, within about 1-3 m and with a clear line of sight. The XIAO's chip antenna is small, and batteries, wires or an enclosure close to it reduce range. A weak Bluetooth link causes timeouts part way through pairing, which look like random failures rather than "out of range". Once paired, only the distance to a Thread router matters.

### Using the Open Home Foundation (OHF) Matter Server without Home Assistant

The Home Assistant Companion app only works with a Home Assistant server. Without one, there are two ways to pair with a standalone OHF Matter Server.

#### Option A: pair with another ecosystem, then share to the Matter Server

This works with a closed Thread border router such as the Aqara Hub M100, whose Thread credentials can't be exported.

1. Add the sensor in the ecosystem's app (for example Aqara Home, with the M100 as the hub) using `34970112332` or the QR code. Keep the phone next to the sensor.
2. In the device's settings in that app, use the option to share it with another platform (often called a pairing code or Matter sharing). This generates a new 11 digit code. Not every app offers this for third party devices.
3. Commission that new code in the Matter Server. Bluetooth isn't used for this step, but the Matter Server host needs an IPv6 route into the Thread network:
    * Run the Matter Server container with host networking.
    * Allow the route adverts sent by the border router (use `wlan0` instead of `eth0` if the host is on Wi-Fi). Add these to `/etc/sysctl.d/` to keep them after a reboot.
    ```sh
    sudo sysctl -w net.ipv6.conf.eth0.accept_ra=1
    sudo sysctl -w net.ipv6.conf.eth0.accept_ra_rt_info_max_plen=64
    ```
    If this step times out while the device shows as online in the other app, the IPv6 routing is almost always the cause.

#### Option B: run your own Thread border router on the Matter Server host

Run OpenThread Border Router on the Raspberry Pi with an 802.15.4 radio, such as a Nordic nRF52840 dongle or a Home Assistant Connect ZBT-1. A spare Xiao nrf52840 flashed with Nordic's OpenThread RCP (radio co-processor) firmware also works. This gives the Matter Server its own Thread network. An existing border router such as an M100 can keep running its own separate network.

1. Enable Bluetooth commissioning in the Matter Server and pass the host's Bluetooth adapter into the container. It's off by default. The Raspberry Pi's built in Bluetooth is weak and shares its radio with Wi-Fi, so a USB Bluetooth dongle on a short extension cable is more reliable.
2. Get the Thread dataset from OTBR and give it to the Matter Server with its `set_thread_dataset` command:
    ```sh
    sudo ot-ctl dataset active -x
    ```
3. Commission `34970112332` from the Matter Server with the sensor within a metre or two of the Pi.

## Modifications

### Temperature and Humidity Reading Frequency
This project is configured to report temperature and humidity every 5 minutes. To change the frequency modify the following line in `src/app_task.cpp`. Value is in milliseconds.

```cpp
constexpr size_t kMeasurementsIntervalMs = 300000;
```

### Battery Monitoring
The batteries are reported through the Matter **Power Source** cluster on endpoint 0, so controllers can show the battery level and raise low battery alerts. No extra wiring or fuel gauge is needed: the batteries feed the 3V3 pin directly, so the nRF52840's own supply voltage is the battery voltage, and it is measured with the chip's internal ADC.

The battery is read on the same 5 minute timer as temperature and humidity, and reports:

|Attribute|Value|
|-|-|
|BatVoltage|Battery pack voltage in mV|
|BatPercentRemaining|Estimated from the voltage (see below)|
|BatChargeLevel|OK, Warning below 2.8 V, Critical below 2.4 V|
|BatReplacementNeeded|True when the charge level is Warning or Critical|
|BatReplaceability, BatReplacementDescription, BatCommonDesignation, BatApprovedChemistry, BatQuantity|User replaceable, "2 x AA lithium (Li-FeS2)", AA, lithium iron disulfide, 2|

Lithium AA cells hold an almost flat voltage for most of their life and then drop quickly, so the percentage is only a rough guide: it will stay high for months and then fall quickly. The low battery warnings (charge level) are the reliable part. The voltage to percentage curve and the warning thresholds are at the top of `src/app_task.cpp`. If you fit the protection diode described in [WIRING.md](WIRING.md), the measured voltage is about 0.2-0.3 V lower than the battery voltage, so lower the thresholds to match.

When USB is plugged in, the Xiao's regulator powers the 3V3 rail, so the battery can't be measured. The firmware detects USB power and reports the voltage and percentage as unknown (null), with the charge level as OK.

See [BATTERY.md](BATTERY.md) for full details of how the battery state is measured and reported.

To change the Power Source cluster, edit `src/default_zap/template.zap` with `west zap-gui`, then regenerate the code with `west zap-generate`.
