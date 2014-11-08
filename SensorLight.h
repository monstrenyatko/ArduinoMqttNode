/*
 *******************************************************************************
 *
 * Purpose: Light Sensor implementation
 *
 *******************************************************************************
 * Copyright Monstrenyatko 2014.
 *
 * Distributed under the MIT License.
 * (See accompanying file LICENSE or copy at http://opensource.org/licenses/MIT)
 *******************************************************************************
 */

#ifndef SENSOR_LIGHT_H_
#define SENSOR_LIGHT_H_

#include "Sensor.h"
/* Internal Includes */
/* External Includes */
/* System Includes */


class SensorLight: public Sensor {
public:
	SensorLight(uint8_t analogPin);
	~SensorLight();
	int32_t getData();
private:
	uint8_t mPin;
};

#endif /* SENSOR_LIGHT_H_ */
