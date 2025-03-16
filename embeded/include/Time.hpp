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


  //return esp_mesh_get_tsf_time();

  struct timeval tv_now;
  gettimeofday(&tv_now, NULL);
  int64_t time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
  return time_us;
}

static int64_t getTimeNowAsUs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);

  return (int64_t)tv.tv_usec + tv.tv_sec * 1000000ll;
}