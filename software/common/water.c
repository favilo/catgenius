/******************************************************************************/
/* File    :	water.c							      */
/* Function:	Water valve and sensor functional implementation	      */
/* Author  :	Robert Delien						      */
/*		Copyright (C) 2010, Clockwork Engineering		      */
/* History :	30 Dec 2012 by R. Delien:				      */
/*		- Renamed from watersensor.c.				      */
/******************************************************************************/
#include <htc.h>

#include "hardware.h"			/* Flexible hardware configuration */

#include "water.h"
#include "timer.h"

#include "eventlog.h"

extern void waterdetection_event	(unsigned char	detected);
extern void watersensor_event		(unsigned int	reflectionquality);


/******************************************************************************/
/* Macros								      */
/******************************************************************************/

#define DETECTTIME		(SECOND/1000)	/*  10ms*/
#define WATERSENSORPOLLING	(SECOND/4)	/* 250ms*/
#define HYSTERESIS_MAX		8		/* Number of pollings to debounce the sensor output */

/*
 * The LM393 inverting schmitt-trigger circuit shuts the water valve autonomously
 * when the light guide is submerged. This circuit is a safe guard against
 * overflows that will work regardless of the state of software.
 * Using a multi-turn poteniometer, we have determined that an open water valve
 * will be closed by the LM393 at a value of 519-520. Due to a little hysteresis,
 * it will open again when the value lowers down to 504-503. The average of these
 * two switch points is therefore: (519+504)/2=1023/2=511,5. With a 10-bits A/D-
 * converter yielding a span of 0..1023, this value is spot-on in the middle.
 */
#define DETECTION_THRESHOLD	520		/* At an ADC value of 520 or above, the LM393 closes the water valve */
#define UNDETECTION_THRESHOLD	503		/* At an ADC value of 503 or below, the LM393 opens the water valve */

/*
 * After the LM393 has closed the water valve, naturally the water level
 * will no longer rise, hence the analog reflection quality value will no
 * longer rise. In theory, a wave of water could briefly trigger the LM393
 * to close the valve, without notifying software with a washing program
 * waiting for high water level. To avoid this race condition, software
 * should be notified at a the highest value at which the water valve is
 * still open (UNDETECTION_THRESHOLD).  Just to be sure, an extra safety
 * margin is subtracted. The hysteresis span seems appropriate.
 */
#define DETECTION_MARGIN	((DETECTION_THRESHOLD)-(UNDETECTION_THRESHOLD))
 
 
 
#define LED_ON			0
#define START_CONVERSION	1
#define PROCESS_RESULT		2


/******************************************************************************/
/* Global Data								      */
/******************************************************************************/

static struct timer	sensortimer       = EXPIRED;
static unsigned char	state             = 0;
static unsigned char	hysteresis        = 0;
static bit		filling           = 0;
static bit		detected          = 0;
static bit		ledalwayson       = 0;


/******************************************************************************/
/* Local Prototypes							      */
/******************************************************************************/


/******************************************************************************/
/* Global Implementations						      */
/******************************************************************************/

void water_init (void)
/******************************************************************************/
/* Function:	Module initialisation routine				      */
/*		- Initializes the module				      */
/* History :	16 Feb 2010 by R. Delien:				      */
/*		- Initial revision.					      */
/******************************************************************************/
{
#ifdef WATERSENSOR_ANALOG
	unsigned char	mask    = WATERSENSORANALOG_MASK;
	unsigned char	channel = 0;

	/* Dynamically determine channel# from mask */
	while (!(mask & 0x01)) {
		mask >>= 0x01;
		channel ++;
	}

	/* Power-up AD circuitry */
	ADCON0bits.ADON = 1;
	/* Select input channel */
	ADCON0bits.CHS = channel;

	/* Set output format to right-justified data */
	ADCON1bits.ADFM = 1;
	/* Set conversion clock to internal RC oscillator */
	ADCON1bits.ADCS = 7;

	/* Set negative reference to Vss, positive reference to Vdd */
	ADCON1bits.ADNREF = 0;
	ADCON1bits.ADPREF = 0;
#endif /* WATERSENSOR_ANALOG */
}
/* End: water_init */


void water_work (void)
/******************************************************************************/
/* Function:	Module worker routine					      */
/*		- Worker function for the CatGenie 120 water sensor and valve */
/* History :	12 Feb 2010 by R. Delien:				      */
/*		- Initial revision.					      */
/******************************************************************************/
{
	static unsigned int	cur_reflectionquality = 0;
	static unsigned int	old_reflectionquality = 0;
	// CMM: This is a quick/temporary hack to cut down on the noise a bit
	static unsigned char event_skips = 0;

	switch (state) {
	default:
		state = LED_ON;
	case LED_ON:
		if (!timeoutexpired(&sensortimer))
			break;
		/* Switch on the IR LED */
		WATERSENSOR_LED(LAT) |= WATERSENSOR_LED_MASK;
		/* Wait for DETECTTIME to give the IR sensor some time */
		settimeout(&sensortimer, DETECTTIME);
#ifdef WATERSENSOR_ANALOG
		state = START_CONVERSION;
#else
		state = PROCESS_RESULT;
#endif /* WATERSENSOR_ANALOG */
		break;
	case START_CONVERSION:
		if (!timeoutexpired(&sensortimer))
			break;
		/* Start A/D conversion */
		ADCON0bits.GO = 1;
		state = PROCESS_RESULT;
		break;
	case PROCESS_RESULT:
#ifdef WATERSENSOR_ANALOG
		if (ADCON0bits.nDONE)
			break;
		/* Read out the IR sensor analoguely (lower value == more light reflected == no water detected) */
		cur_reflectionquality = ADRES;
#else
		if (!timeoutexpired(&sensortimer))
			break;
		/* Read out the IR sensor digitally (lower value == more light reflected == no water detected) */
		cur_reflectionquality = (WATERSENSORANALOG(PORT) & WATERSENSORANALOG_MASK)?DETECTION_THRESHOLD:0;
#endif /* WATERSENSOR_ANALOG */
		/* Switch off the IR LED if we're not filling */
		if (!filling && !ledalwayson)
			WATERSENSOR_LED(LAT) &= ~WATERSENSOR_LED_MASK;
		/* Evaluate the result, considering a hysteresis */
		if (cur_reflectionquality <= (UNDETECTION_THRESHOLD - DETECTION_MARGIN)) {
			if ((hysteresis > 0) &&
			    (!--hysteresis && detected)) {
				detected = 0;
				waterdetection_event(detected);
				// CMM: Force a water sensor event below
				event_skips = 255;
			}
		} else {
			if ((hysteresis < HYSTERESIS_MAX) &&
			    (++hysteresis >= HYSTERESIS_MAX) && !detected) {
				detected = 1;
				waterdetection_event(detected);
				// CMM: Force a water sensor event below
				event_skips = 255;
			}
		}
		/* Check water sensor reflection quality */

		// CMM: This is a quick/temporary hack to cut down on the noise a bit
		/*
		if (cur_reflectionquality != old_reflectionquality) {
			watersensor_event(cur_reflectionquality);
			old_reflectionquality = cur_reflectionquality;
		}
		*/

		// TBD: Shouldn't we be triggering a water sensor event BEFORE a water
		//   detection event?

		// Only trigger a water sensor event if:
		//   - The current value varies from the last reported value +/- >4
		//   - The current value varies from the last and we've skipped firing
		//     the event for a minute or longer
		if ((((cur_reflectionquality > old_reflectionquality) ?
				(cur_reflectionquality - old_reflectionquality) :
				(old_reflectionquality - cur_reflectionquality)) > 4) ||
				(event_skips >= 240))
		{
			if (cur_reflectionquality != old_reflectionquality) {
				watersensor_event(cur_reflectionquality);
				old_reflectionquality = cur_reflectionquality;
			}
			event_skips = 0;
		}
		else
		{
			event_skips++;
		}			

		settimeout(&sensortimer, WATERSENSORPOLLING);
		state = LED_ON;
		break;
	}
}
/* End: water_work */


unsigned char water_detected (void)
{
	return (detected);
}
/* End: water_detected */


void water_ledalwayson (unsigned char on)
{
	ledalwayson = on;
}
/* End: water_ledalwayson */


unsigned char water_filling (void)
{
	return (filling);
}
/* End: water_filling */


void water_fill (unsigned char fill)
{
	filling = fill;

	if (filling) {
		/* Pull-up WATERVALVE */
		WATERVALVEPULLUP(LAT) |= WATERVALVEPULLUP_MASK;
	} else {
		/* Pull-down WATERVALVE */
		WATERVALVEPULLUP(LAT) &= ~WATERVALVEPULLUP_MASK;
	}

	eventlog_track(EVENTLOG_TAP, fill);
}
/* End: water_fill */


/******************************************************************************/
/* Local Implementations						      */
/******************************************************************************/
