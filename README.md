Overview
This project implements an automatic animal feeder using an ESP32 microcontroller and a servo motor. The system allows you to remotely trigger feeding through a web interface, making it ideal for pet owners who need to feed their animals while away or want to maintain a consistent feeding schedule.

Features
Web-based control interface accessible from any device with a browser
Servo motor control to dispense food
Simple one-button operation
Automatic return to default position after feeding
Works on your local WiFi network
Configurable auto-feeding timer
Demo
Working Model

Hardware Requirements
ESP32 development board
Servo motor (standard hobby servo)
Power supply (5V for servo)
Housing/container for the feeder mechanism
Jumper wires
Optional: 3D printed food dispenser mechanism
Servo Wiring
Red wire: Connect to VCC (5V)
Black/Brown wire: Connect to GND
Orange/Yellow/Signal wire: Connect to GPIO 15
Software Requirements
ESP-IDF (Espressif IoT Development Framework)
Web browser for accessing the control interface
How It Works
The servo motor is positioned at 90 degrees by default (neutral position)
When the "Feed Now" button is pressed on the web interface, the servo moves to 75 degrees
After 5 seconds, the servo automatically returns to the 90-degree position
This motion dispenses a measured amount of food from the container
Installation and Setup
Installation Process

1. Install Prerequisites
Make sure you have ESP-IDF installed on your development machine. Follow the official installation guide if needed.

2. Clone the Repository
git clone https://github.com/yourusername/automatic-animal-feeder.git
cd automatic-animal-feeder
3. Configure WiFi Settings
Edit the WiFi credentials in main/automatic_animal_feeder.c:

#define WIFI_SSID       "pet_feeder"
#define WIFI_PASSWORD   "12341234"
4. Build and Flash
idf.py build
idf.py -p PORT flash monitor
Replace PORT with your ESP32's serial port (e.g., /dev/ttyUSB0 on Linux or COM3 on Windows).

5. Access the Web Interface
Connect to the "pet_feeder" WiFi network using the password "12341234"

WiFi QR Code

Open a web browser and navigate to 172.20.10.2

Web Interface QR Code

You should see the control interface with a "Feed Now" button

Customization Options
Adjusting Servo Angles
You can modify the servo positions by changing these definitions in the code:

#define SERVO_90_DEGREES    (4096 * 0.5 / 20)  // 90 degrees (center position)
#define SERVO_75_DEGREES    (4096 * 1.5 / 20)  // 75 degrees (rotate)
Changing the Feeding Duration
To change how long the servo stays in the feeding position:

servo_reset_timer = xTimerCreate("servo_reset_timer", pdMS_TO_TICKS(5000),
                                 pdFALSE, 0, servo_reset_timer_callback);
Modify the 5000 value to change the duration in milliseconds.

GPIO Pin Assignment
If you need to use a different GPIO pin for the servo:

#define SERVO_PIN           15       // GPIO pin for servo control
Auto-Feeding Timer
The system includes an automatic feeding timer that can be configured through the web interface. Options range from 30 minutes to 24 hours.

Mechanical Design Considerations
Design your feeder so that the servo motion effectively dispenses the appropriate amount of food
Consider using a funnel or hopper design for consistent food flow
Ensure the food container is securely attached to prevent spills
Position the servo to maximize leverage with minimal strain
Troubleshooting
Servo Not Moving
Check wiring connections
Verify power supply is adequate (servos typically need 5V)
Ensure GPIO pin definition matches your wiring
Can't Connect to Web Interface
Verify ESP32 is connected to WiFi (check serial monitor output)
Confirm you're using the correct IP address (172.20.10.2)
Make sure your device is connected to the "pet_feeder" WiFi network
Inconsistent Food Dispensing
Adjust servo angles for optimal dispensing
Modify the mechanical design to improve food flow
Consider using a smaller feed opening for more precise portions
Future Enhancements
Add scheduled feeding functionality based on time of day
Implement multiple feeding profiles for different animals
Add a camera to monitor your pet
Create a mobile app for remote control
Add sensors to detect food levels
Implement multiple user access controls
Contributing
Contributions are welcome! Please feel free to submit a Pull Request.
