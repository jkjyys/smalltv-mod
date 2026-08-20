// WeatherClient.h — Open-Meteo fetch + parse for the weather feature.
#pragma once
#include "Settings.h"
#include "WeatherData.h"

void               weatherInit(const Settings& s);
void               weatherForceRefresh();
void               weatherService(const Settings& s);   // call every loop tick
const WeatherNow&  weatherCurrent();
