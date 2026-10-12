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

// Identity baked into the image: a config reset or a lost /prefs would otherwise mint a new
// keypair, which changes the NodeNum and makes peers drop our NodeInfo (they reject a key that
// differs from the one they already hold). NodeNum here is crc32(BAKED_IDENTITY_PUBLIC_KEY).
static const uint8_t BAKED_IDENTITY_PRIVATE_KEY[32] = {0xA8, 0x9D, 0x3A, 0xB1, 0xC5, 0x88, 0xB9, 0x4E, 0x05, 0xF9, 0x0B,
                                                       0x33, 0x6D, 0x9B, 0x66, 0xAD, 0xB8, 0x94, 0xEF, 0x32, 0xBF, 0x28,
                                                       0x10, 0xE3, 0x71, 0x9F, 0x2F, 0xEF, 0x00, 0x49, 0x48, 0x6B};
static const uint8_t BAKED_IDENTITY_PUBLIC_KEY[32] = {0x67, 0xF2, 0x75, 0x5F, 0x9B, 0x45, 0xFF, 0xCE, 0x7F, 0x1A, 0x44,
                                                      0x44, 0xBD, 0x60, 0xDA, 0xAF, 0x74, 0xFF, 0xEC, 0xF9, 0x75, 0x5A,
                                                      0xA4, 0x71, 0xC4, 0xE3, 0xC9, 0xCB, 0x61, 0x73, 0x75, 0x45};

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

    memcpy(config.security.private_key.bytes, BAKED_IDENTITY_PRIVATE_KEY, sizeof(BAKED_IDENTITY_PRIVATE_KEY));
    config.security.private_key.size = sizeof(BAKED_IDENTITY_PRIVATE_KEY);
    memcpy(config.security.public_key.bytes, BAKED_IDENTITY_PUBLIC_KEY, sizeof(BAKED_IDENTITY_PUBLIC_KEY));
    config.security.public_key.size = sizeof(BAKED_IDENTITY_PUBLIC_KEY);
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
