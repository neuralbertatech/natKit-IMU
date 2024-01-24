#pragma once

#include "time.h"
#include "esp_mesh.h"

unsigned long epochTime; 

unsigned long getTime() {
  // time_t now;
  // struct tm timeinfo;
  // if (!getLocalTime(&timeinfo)) {
  //   return 0;
  // }
  // time(&now);
  // return now;
  return esp_mesh_get_tsf_time();
}