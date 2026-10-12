/*
  Copyright (c) 2014-2015 Arduino LLC.  All right reserved.
  Copyright (c) 2016 Sandeep Mistry All right reserved.
  Copyright (c) 2018, Adafruit Industries (adafruit.com)

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "variant.h"
#include "nrf.h"
#include "wiring_constants.h"
#include "wiring_digital.h"

#include "configuration.h"
#include "mesh/NodeDB.h"

const uint32_t g_ADigitalPinMap[] = {
    // P0
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,

    // P1
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47};

void initVariant()
{
    // LED1 & LED2
    pinMode(PIN_LED1, OUTPUT);
    ledOff(PIN_LED1);

    // 3V3 Power Rail
    pinMode(PIN_3V3_EN, OUTPUT);
    digitalWrite(PIN_3V3_EN, HIGH);
}

// USERPREFS_CONFIG_DEVICE_ROLE refuses ROUTER and hop_limit has no userPrefs macro, so this
// build's role, hop limit and Bluetooth defaults are set here, last in installDefaultConfig().
void variantDefaultConfig()
{
    config.device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY;
    config.lora.hop_limit = 7;
    config.bluetooth.enabled = false;
    owner.has_is_unmessagable = true;
    owner.is_unmessagable = true;
}

// USERPREFS_FIXED_GPS is gated on reboot_count == 1 and only ESP32 ever increments that counter,
// so the fixed position is applied here instead, and re-applied on every boot.
void lateInitVariant()
{
    meshtastic_Position fixed = meshtastic_Position_init_default;
    fixed.latitude_i = 414098000; // 41.4098 N
    fixed.longitude_i = 22160000; // 2.2160 E
    fixed.has_latitude_i = true;
    fixed.has_longitude_i = true;
    fixed.altitude = 5;
    fixed.has_altitude = true;
    fixed.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
    nodeDB->setLocalPosition(fixed);
    if (!config.position.fixed_position) {
        config.position.fixed_position = true;
        nodeDB->saveToDisk(SEGMENT_CONFIG);
    }
}
