Wifi-enabled Model Train Controller

*Disclaimer: This project has not been tested extensively. I do not guarantee that it will
not damage your model trains. Use at your own risk*

This is a model train controller designed to drive up to 6 n-scale locomotives. It uses the ledc module of an ESP-32-S2 running server.c to control an H-bridge with PWM. The ESP-32 is controlled remotely by a PC or laptop running client.c.

The controller should be powered with 11-15 volts/3A.

server.c is built with the Espressif ESP-IDF. You must install it in order to program the esp-32.

client.c requires the IP address of the esp-32 as an argument. The server.c provides the IP address over the serial port when it connects to Wi-Fi. Use putty or similar to read it.

client.c was written for a unix-like system. To run on Windows you will need to change every instance of system("clear") to system("cls").
