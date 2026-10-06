/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The SenseCAP Indicator's air sensors. They hang off the RP2040, which reads
// them every few seconds and sends each value over UART; this keeps the
// latest of each. The D1S and D1Pro have CO2 (SCD41) and tVOC (SGP40) built
// in; temperature and humidity come from the Grove AHT20 in the box. The D1
// and D1L have no sensors.

#pragma once

#include "cJSON.h"

// Start the task that reads the RP2040's UART.
void sensecap_sensors_init(void);

// sensors.read: the latest reading of each sensor, with its age. A sensor
// with no recent reading is null; with none at all, an error.
cJSON *sensecap_sensors_command(void);
