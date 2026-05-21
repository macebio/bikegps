/*
 * NimBLE-Arduino 2.x compatibility shim for ESP32 Arduino framework.
 * Routes nimconfig.h requests to the ESP-IDF sdkconfig.h which is
 * always on the include path when building with esp32:esp32.
 */
#pragma once
#include "sdkconfig.h"
