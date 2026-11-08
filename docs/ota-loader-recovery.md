# OTA por Bluetooth: "Device does not have a valid OTA Loader"

## Síntoma

La app Android de Meshtastic, en _Actualización de firmware_, devuelve:

- `The selected file "firmware-…factory.bin" does not match …`: es normal. El `.factory.bin`
  es solo para flashear por cable. Para OTA hay que elegir el `.bin` normal (sin `factory`).
- `Cannot start OTA: Device does not have a valid OTA Loader` (versiones antiguas del firmware
  lo escribían como "does have"): la partición `ota_1` del nodo está vacía o no contiene el loader.

## Causa

En ESP32 el OTA por BLE/WiFi lo hace un programa pequeño, el **OTA Loader**, guardado en la
partición `ota_1`. El `.factory.bin` **no lo incluye**: lleva bootloader, tabla de particiones y
app, escritos en `0x0`. Si el nodo se flashea solo con el `.factory.bin`, `ota_1` queda sin loader
y el OTA falla (`AdminModule.cpp`, caso `ota_request`).

## Cómo comprobarlo

Desde el propio nodo, el comando de información OTA (`NexusModule`) muestra `Available: yes/no/error`
y el tipo de loader (`MeshtasticOTA`, `MeshtasticOTA-BLE` o `MeshtasticOTA-WiFi`).

## Recuperación (una sola vez, por cable)

Con los ficheros de la release en el mismo directorio
(`firmware-<target>-<version>.factory.bin`, `.mt.json`, `littlefs-<target>-<version>.bin` y
`mt-<mcu>-ota.bin`):

```bash
./bin/device-install.sh -f firmware-heltec-v4-<version>.factory.bin
```

El script borra la flash y escribe, en este orden: el `.factory.bin` en `0x0`, el loader en el
offset de `ota_1` (se lee del `.mt.json`; 16 MB = `0x650000`) y el LittleFS en `spiffs`.
Después de esto el OTA por BLE vuelve a funcionar y las siguientes actualizaciones ya no
necesitan cable.

Si solo se quiere añadir el loader sin borrar nada:

```bash
esptool.py --chip esp32s3 write_flash 0x650000 mt-esp32s3-ota.bin
```

(para tablas de 8 MB o 4 MB, el offset de `ota_1` es el del `.mt.json`).
