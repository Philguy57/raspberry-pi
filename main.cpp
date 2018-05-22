#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <wiringPi.h>

#include <linux/input.h>
#include <linux/joystick.h>

/* NOTE: both modules 'joystick' and 'wiringPi' must be downloaded for this to work properly. */


/* mapping of LEDs on the board to the GPIO pins on the raspberry pi. */
#define LED_1		4		
#define LED_2		17		
#define LED_3		22		
#define LED_4		10		
#define LED_5		9		
#define LED_6		11		
#define BUZZER		8		

/* indexes into the various buttons. */
#define A_BUTTON	0
#define B_BUTTON	1
#define X_BUTTON	2
#define Y_BUTTON	3
#define R_BUMPER	4
#define L_BUMPER	5
#define START		6
#define BACK		7

/*
 *	turnOnLeds
 *	Description: will turn on the various LEDs depending on the buttons pressed.
 *	Inputs:	button_map -- the array of buttons, 1 for pressed, 0 for unpressed.
 *	Outputs: None.
 */
void turnOnLeds(char* button_map) {

	if (button_map[A_BUTTON] == 1) {
		digitalWrite(LED_1, HIGH);
	}
	else {
		digitalWrite(LED_1, LOW);
	}

	if (button_map[B_BUTTON] == 1) {
		digitalWrite(LED_2, HIGH);
	}
	else {
		digitalWrite(LED_2, LOW);
	}

	if (button_map[X_BUTTON] == 1) {
		digitalWrite(LED_3, HIGH);
	}
	else {
		digitalWrite(LED_3, LOW);
	}

	if (button_map[Y_BUTTON] == 1) {
		digitalWrite(LED_4, HIGH);
	}
	else {
		digitalWrite(LED_4, LOW);
	}

	if (button_map[R_BUMPER] == 1) {
		digitalWrite(LED_5, HIGH);
	}
	else {
		digitalWrite(LED_5, LOW);
	}

	if (button_map[L_BUMPER] == 1) {
		digitalWrite(LED_6, HIGH);
	}
	else {
		digitalWrite(LED_6, LOW);
	}

	if (button_map[BACK] == 1) {
		digitalWrite(BUZZER, HIGH);
	}
	else {
		digitalWrite(BUZZER, LOW);
	}

}

int main(void)
{
	int fd;
	unsigned char num_buttons = 2;
	unsigned char num_axes = 2;
	int* axis;
	char* button;
	struct js_event js;

	/* initialize wiringPi */
	wiringPiSetupSys();

	if ((fd = open("/dev/input/js0", O_RDONLY)) < 0) {
		printf("could not open");
		return -1;
	}

	/* declare all the separate LEDs and the buzzer as outputs */
	pinMode(LED_2, OUTPUT);
	pinMode(LED_1, OUTPUT);
	pinMode(LED_3, OUTPUT);
	pinMode(LED_4, OUTPUT);
	pinMode(LED_5, OUTPUT);
	pinMode(LED_6, OUTPUT);
	pinMode(BUZZER, OUTPUT);

	/* get the number of axes and buttons for the controller */
	ioctl(fd, JSIOCGBUTTONS, &num_buttons);
	ioctl(fd, JSIOCGAXES, &num_axes);

	/* allocate space for all the axes and buttons */
	axis = (int*) calloc(num_axes, sizeof(int));
	button = (char*) calloc(num_buttons, sizeof(char));

	while (1) {
		if (read(fd, &js, sizeof(struct js_event)) != sizeof(struct js_event)) {
			printf("could not read");
			free(axis);
			free(button);
			return -1;
		}

		/* check whether what changed was an axis or a button, and store the value accordingly */
		switch (js.type & ~JS_EVENT_INIT) {
			case JS_EVENT_AXIS:					// currently does nothing with axes values, will be added later.
				axis[js.number] = js.value;
				break;
			case JS_EVENT_BUTTON:
				button[js.number] = js.value;
				break;
		}
		turnOnLeds(button);
	}


}