"""Compile the installed TinyGPSPlus parser with native UART/CAN fixtures."""
import os
from pathlib import Path


def gps_build_flags(directory):
    source = Path(os.environ.get('TINYGPSPLUS_SRC', Path.home() / 'Arduino/libraries/TinyGPSPlus/src'))
    if not (source / 'TinyGPS++.cpp').is_file():
        raise RuntimeError('Install TinyGPSPlus with Arduino CLI or set TINYGPSPLUS_SRC to its src directory')
    directory = Path(directory)
    (directory / 'Arduino.h').write_text('''#pragma once
#include <stdint.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
using byte=uint8_t;
uint32_t millis();
constexpr double TWO_PI=6.2831853071795864769;
inline double radians(double n){return n*0.01745329251994329577;}
inline double degrees(double n){return n*57.295779513082320877;}
template<class T> T sq(T n){return n*n;}
''')
    return ['-Wno-implicit-fallthrough', '-DARDUINO=10819', '-I', str(directory), '-I', str(source), str(source / 'TinyGPS++.cpp')]
