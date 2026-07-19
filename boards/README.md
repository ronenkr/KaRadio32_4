# BOARDS Description

This directory contains some samples of common boards found on the market.  
They are to be modified to your needs and peripheral connected.  

standard_adb.csv is the default description of the gpio used when no hardware partition is flashed.

standard_psram.bin  is the default description for a wrover cpu

standard_touch.bin  is the default description for a wroom cpu with a touch screen

lolin32pro.csv is adapted to a wrover lolin pro or ttgo board.  

ttgot8.csv, ttgotm.csv,ttgov4.csv are for the correspondant boards.

`ttgotdisplay.csv` is for the original ESP32 T-Display and is not compatible with T-Display S3.

`ttgo_tdisplay_s3.csv` targets the standard LilyGO TTGO T-Display S3. Build it with the ESP32-S3 configuration, then flash its generated hardware-NVS binary at `0x622000`; see the T-Display S3 section in the root README.

odroid.csv is for the Odroid Go device. See https://github.com/pepelnyy/KaRadio32-on-ODROID-GO
