# ESPWebKitS3
ESP32 PS5 Webkit Autoloader is made from VS Code with PlatformIO.<BR>
It contains DNS filter configuration and provide PS5 Webkit Autoloader web pages<BR>
ESPWebKitS3 has tested on ESP32-S3FN8 dongle with PS5 firmware 10.01<BR>

ESP32-S3 Dongle<BR>
![image](https://github.com/MacleodTW/ESPWebKitS3/blob/main/.github/ESP32%20S3%20Dongle.png)

WiFi/DNS configuration<BR>
![image](https://github.com/MacleodTW/ESPWebKitS3/blob/main/.github/ESP32%20S3%20Dashboard.png)

Requirement:<BR>
。ESP32 S3 8MB flash or more
<BR>

Usage:
01. Download release ESPWebKitS3_xxx.zip
02. Flash ESP32-S3 dongle with the tool inside zip
03. Plug ESP32-S3 dongle to recycle power
04. Phone or PC WiFi connect to "PS_WiFi" SSID, password is "password"
05. Use browser to https://10.1.1.1/dns (Contiue insecure)
06. Set STA WiFi SSID and STA WiFi password then click "Save"
07. Reload browser get STA IP
08. PS5 set DNS server to STA IP
09. PS5 Settings -> Guide & Tips, Health and Safety, and Other information -> User's Guide -> If it shows security xxx click "Yes"
10. Click jailbreak. After all is done, it shows "Payload Manager" and "Webkit Autoloader Installer" buttons to send payload
<BR>

Compile:
01. VS Code with PlatformIO installed
02. PlatformIO open ESPWebKitS3 root directory
03. Build: Full Clean or Clean -> Platform: Build Filesystem Image -> Build: Build
04. All images in ESPWebKitS3 flash directory
<BR>

Credits:<BR>
。[itsPLK](https://github.com/itsplk) — [ps5-webkit-autoloader](https://github.com/itsPLK/ps5-webkit-autoloader)<BR>
。[idlesauce](https://github.com/idlesauce) — [umtx2](https://github.com/idlesauce/umtx2)<BR>
。[jordyidk](https://github.com/jordyidk) — [slopkit](https://github.com/jordyidk/slopkit)<BR>
。[soniciso1](https://github.com/soniciso1) — [pooP2JB](https://github.com/soniciso1/pooP2JB)<BR>
。[john-tornblom](https://github.com/john-tornblom) — [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk/) and [elfldr](https://github.com/ps5-payload-dev/elfldr)<BR>
