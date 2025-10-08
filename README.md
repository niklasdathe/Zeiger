# Zeiger
## Components
- E-Ink Display https://www.waveshare.com/product/displays/e-paper/epaper-1/7.5inch-e-paper.htm
- ESP32 E-Ink Display Driver https://www.waveshare.com/product/arduino/displays/e-paper/e-paper-esp32-driver-board.htm

## Code Config Guide
- Change the secrets_EXAMPLE.h filename to secrets.h and change the defines to your SSID, password and ICS-Link. Don't share these things with anyone!  
    The ICS-Link can be found by going to your google calendar dashboard -> Settings -> Your calendars name under settings for my calendar -> Secret adress in iCal format.  
    Copy this link and paste it into the placeholder in secrets.h

- Edit the AppConfig.h to change the dashboard to your liking:  
    *DarkMode*: switch between black or white background color  
    *locale*: switch the language of your dashboard (soon: DE, EN, FR)  
    *use24h*: Use the 24h time format
      


  
