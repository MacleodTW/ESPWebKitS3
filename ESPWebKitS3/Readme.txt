1. Connect ESP32-S3 with USB cable to computer
2. Run flash_download_tool_3.9.11 app
3. Select as following
    ChipType: ESP32-S3
    WorkMode: Develop
    LoadMode: UART
4. ESP32-S3 Dongle -> Hold button and plugin power
5. Select COM Port and BAUD 115200
6. Select ERASE -> Wait FINISH
7. Add following 4 files and fill address then press START 
    V 01_bootloader.bin  @ 0x0
    V 02_partitions.bin  @ 0x8000
    V 03_boot_app0.bin   @ 0xE000
    V 04_firware.bin     @ 0x10000
    V 05_filesystem.bin  @ 0x110000
8. Remove power and connect again
9. UART connect BAUD rate 115200




