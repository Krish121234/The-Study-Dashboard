📚 The Study Dashboard

A smart desk companion built with an ESP8266 that automatically tracks study sessions based on user presence and provides a simple web dashboard for monitoring productivity.

Overview

The Study Dashboard is an IoT-based productivity device designed to help maintain structured study sessions. Using an ultrasonic sensor, the system detects whether the user is sitting at their desk and automatically starts or pauses study sessions. It also manages break periods and provides real-time feedback through an OLED display, LED indicators, and a web-based dashboard.

The device runs entirely on an ESP8266 microcontroller and hosts its own local web server, allowing users to monitor and control their study sessions from any device connected to the same network.

Features
	•	📡 Presence Detection using an ultrasonic sensor
	•	⏱ Automatic Study/Break Timer with configurable durations
	•	📊 Session Tracking with total study time and session history
	•	🖥 Local Web Dashboard for real-time status and controls
	•	✅ Built-in To-Do List accessible from the web interface
	•	📟 OLED Display showing system state and countdown timer
	•	🌐 WiFi Enabled with a built-in web server

How It Works
	1.	The ultrasonic sensor continuously monitors the distance in front of the device.
	2.	When the user is detected within a defined range, a study session begins automatically.
	3.	If the user leaves the desk, the session pauses and resumes when they return.
	4.	After a study session finishes, the system automatically switches to a break timer.
	5.	The OLED display shows the current state and remaining time.
	6.	A web dashboard allows users to view statistics, change session durations, reset sessions, and manage tasks.

Hardware Used
	•	ESP8266 (NodeMCU)
	•	Ultrasonic Distance Sensor
	•	0.96” I2C OLED Display
	•	Breadboard / custom enclosure
	•	Battery pack or USB power

Web Interface

The device hosts a local web interface that includes:
	•	Dashboard with session statistics
	•	Study and break duration settings
	•	Session reset controls
	•	Session history
	•	A simple to-do list manager


