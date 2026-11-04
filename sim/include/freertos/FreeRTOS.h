#pragma once

#include <stdint.h>

typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;

#define configRUN_TIME_COUNTER_TYPE uint32_t
#define portNUM_PROCESSORS 2

#ifndef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#define CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS 1
#endif

#define configGENERATE_RUN_TIME_STATS 1
#define INCLUDE_xTaskGetIdleTaskHandle 1

#ifndef CONFIG_FREERTOS_SMP
#define CONFIG_FREERTOS_SMP 0
#endif
